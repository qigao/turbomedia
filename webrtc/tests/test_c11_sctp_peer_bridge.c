#include "turbo_datachannel.h"
#include "turbo_datachannel_internal.h"
#include "sctp_assoc_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sctp_gmssl_mac_internal.h"
#include <gmssl/rand.h>
#include <salts/thread.h>
#include <tlog.h>

/* Private test target only: production still routes DTLS plaintext to usrsctp. */
enum { QCAP = 32, DCAP = 4096, PCAP = CNET_SCTP_ASSOC_MAX_PACKET };
typedef struct endpoint endpoint;
typedef struct { turbo_dc_peer_t *dst; size_t len; uint8_t bytes[DCAP]; } datagram;
typedef struct { cmeta_mutex_t mutex; datagram q[QCAP]; size_t head, count; int failed; } packet_queue;
struct endpoint {
  packet_queue *queue;
  turbo_dc_peer_t *peer, *remote;
  uint16_t local_port, remote_port;
  int client, assoc_initialized, started, data_sent;
  cnet_sctp_gmssl_mac cookie_mac;
  uint64_t now;
  cnet_sctp_assoc assoc;
  atomic_int failure, received, acked;
};
static void fail(endpoint *e, int code) {
  if (e) atomic_store_explicit(&e->failure, code ? code : -1, memory_order_release);
}
static int owner(void *p) {
  endpoint *e = (endpoint *)p;
  return e && dc_peer_is_on_transport_owner(e->peer);
}
static void wipe_key(void *memory, size_t length) {
  volatile uint8_t *bytes = (volatile uint8_t *)memory;
  while (length != 0u) { *bytes++ = 0u; --length; }
}
static int entropy(void *p, uint8_t *out, size_t n) {
  (void)p;
  return out && rand_bytes(out, n) == 1 ? 0 : -1;
}
static int mac(void *p, const uint8_t *msg, size_t n,
               uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  endpoint *e = (endpoint *)p;
  if (!e) return -1;
  return cnet_sctp_gmssl_mac_sign(&e->cookie_mac, msg, n, out);
}
static int verify_mac(void *p, const uint8_t *msg, size_t n,
                      const uint8_t claimed[CNET_SCTP_ASSOC_MAC_BYTES]) {
  endpoint *e = (endpoint *)p;
  if (!e) return -1;
  return cnet_sctp_gmssl_mac_verify(&e->cookie_mac, msg, n, claimed);
}
static int init_cookie_mac(endpoint *e) {
  uint8_t key[CNET_SCTP_GMSSL_KEY_BYTES] = {0};
  int status;
  if (!e || rand_bytes(key, sizeof(key)) != 1) {
    wipe_key(key, sizeof(key));
    return -1;
  }
  status = cnet_sctp_gmssl_mac_init(&e->cookie_mac, key, sizeof(key));
  wipe_key(key, sizeof(key));
  return status;
}
static void send_datagram(void *p, const void *data, size_t len) {
  endpoint *e = (endpoint *)p; packet_queue *q; size_t tail;
  if (!e || !e->queue || !e->remote || !data || !len || len > DCAP) { fail(e, -1); return; }
  q = e->queue; cmeta_mutex_lock(&q->mutex);
  if (q->count == QCAP) { q->failed = 1; cmeta_mutex_unlock(&q->mutex); fail(e, -2); return; }
  tail = (q->head + q->count) % QCAP; q->q[tail].dst = e->remote; q->q[tail].len = len;
  memcpy(q->q[tail].bytes, data, len); q->count++; cmeta_mutex_unlock(&q->mutex);
}
static int pop_packet(packet_queue *q, datagram *d) {
  cmeta_mutex_lock(&q->mutex);
  if (!q->count) { cmeta_mutex_unlock(&q->mutex); return 0; }
  *d = q->q[q->head]; q->head = (q->head + 1u) % QCAP; q->count--;
  cmeta_mutex_unlock(&q->mutex); return 1;
}
static int queue_failed(packet_queue *q) {
  int failed; cmeta_mutex_lock(&q->mutex); failed = q->failed; cmeta_mutex_unlock(&q->mutex); return failed;
}
static void init_assoc(void *p, void *result_ptr) {
  endpoint *e = (endpoint *)p; int *result = (int *)result_ptr; cnet_sctp_assoc_config c = {0};
  if (result) *result = -1;
  if (!e || !owner(e)) { fail(e, -3); return; }
  if (init_cookie_mac(e) != 0) { fail(e, -13); return; }
  c.local_port=e->local_port; c.remote_port=e->remote_port; c.receive_window=8192u;
  c.outbound_streams=4u; c.inbound_streams=4u; c.t1_ms=100u; c.t1_max_ms=400u;
  c.cookie_lifetime_ms=5000u; c.peer_binding=((uint64_t)e->local_port<<32)|e->remote_port;
  c.max_retransmits=3u; c.is_owner=owner; c.entropy=entropy; c.mac=mac; c.verify_mac=verify_mac; c.provider_context=e;
  if (cnet_sctp_assoc_init(&e->assoc, &c, 0u) != CNET_SCTP_ASSOC_OK) {
    cnet_sctp_gmssl_mac_retire(&e->cookie_mac);
    fail(e,-4);
    return;
  }
  e->assoc_initialized=1; if (result) *result=0;
}
static void cleanup_assoc(void *p, void *result_ptr) {
  endpoint *e=(endpoint *)p; int *result=(int *)result_ptr; if (result) *result=-1;
  if (!e || !owner(e) || !e->assoc_initialized) return;
  if (cnet_sctp_assoc_stop(&e->assoc)!=CNET_SCTP_ASSOC_OK) return;
  if (cnet_sctp_assoc_destroy(&e->assoc)!=CNET_SCTP_ASSOC_OK) return;
  e->assoc_initialized=0;
  cnet_sctp_gmssl_mac_retire(&e->cookie_mac);
  if (result) *result=0;
}
static void app_data(turbo_dc_peer_t *peer, const uint8_t *data, size_t len, void *p) {
  endpoint *e=(endpoint *)p; uint8_t out[PCAP]; size_t out_n=0; cnet_sctp_assoc_status st;
  static const uint8_t payload[]={'p','e','e','r','-','d','t','l','s','-','c','1','1'};
  if (!e || peer!=e->peer || !owner(e) || !data || !len || !e->assoc_initialized) { fail(e,-5); return; }
  st=cnet_sctp_assoc_input(&e->assoc,++e->now,data,len,out,sizeof(out),&out_n);
  if (st!=CNET_SCTP_ASSOC_OK) { fail(e,(int)st); return; }
  if (out_n && dtls_test_write_application_data(peer,out,out_n)!=0) { fail(e,-6); return; }
  if (!e->client && e->assoc.state==CNET_SCTP_ASSOC_ESTABLISHED) {
    cnet_sctp_data_message meta={0}; uint8_t got[64]; size_t got_n=0;
    st=cnet_sctp_assoc_take_message(&e->assoc,&meta,got,sizeof(got),&got_n);
    if (st==CNET_SCTP_ASSOC_OK) {
      if (meta.stream_id!=0u || meta.ppid!=51u || got_n!=sizeof(payload) || memcmp(got,payload,sizeof(payload))!=0) { fail(e,-7); return; }
      atomic_store_explicit(&e->received,1,memory_order_release);
    } else if (st!=CNET_SCTP_ASSOC_EMPTY) { fail(e,(int)st); return; }
  }
  if (e->client && e->assoc.state==CNET_SCTP_ASSOC_ESTABLISHED && !e->data_sent) {
    st=cnet_sctp_assoc_send_message(&e->assoc,++e->now,0u,51u,0,payload,sizeof(payload),out,sizeof(out),&out_n);
    if (st!=CNET_SCTP_ASSOC_OK || !out_n || dtls_test_write_application_data(peer,out,out_n)!=0) { fail(e,st==CNET_SCTP_ASSOC_OK?-8:(int)st); return; }
    e->data_sent=1;
  }
  if (e->client && e->data_sent && e->assoc.data.in_flight==0u)
    atomic_store_explicit(&e->acked,1,memory_order_release);
}
static void state_cb(turbo_dc_peer_t *peer,turbo_dc_state_t old,turbo_dc_state_t state,void *p) {
  endpoint *e=(endpoint *)p; uint8_t packet[PCAP]; size_t n=0; cnet_sctp_assoc_status st; (void)old;
  if (!e || peer!=e->peer || !owner(e)) { fail(e,-9); return; }
  if (!e->client || state!=TURBO_DC_STATE_CONNECTED || e->started) return;
  e->started=1; st=cnet_sctp_assoc_start(&e->assoc,++e->now,packet,sizeof(packet),&n);
  if (st!=CNET_SCTP_ASSOC_OK || !n || dtls_test_write_application_data(peer,packet,n)!=0) fail(e,st==CNET_SCTP_ASSOC_OK?-10:(int)st);
}
static void test_real_peer_dtls_c11_roundtrip(void) {
  packet_queue q={0}; endpoint a={0}, b={0}; turbo_dc_config_t ca={0}, cb={0};
  turbo_dc_context_t *ctx_a,*ctx_b; turbo_dc_peer_t *peer_a,*peer_b;
  char hash_a[32]={0},hash_b[32]={0},fp_a[128]={0},fp_b[128]={0};
  int init_result=-1, cleanup_result=-1; unsigned idle=0; datagram d;
  cmeta_mutex_init(&q.mutex); a.queue=b.queue=&q;
  a.local_port=5000u;a.remote_port=5001u;a.client=1;
  b.local_port=5001u;b.remote_port=5000u;b.client=0;
  atomic_init(&a.failure,0);atomic_init(&a.received,0);atomic_init(&a.acked,0);
  atomic_init(&b.failure,0);atomic_init(&b.received,0);atomic_init(&b.acked,0);
  ca.transport=cb.transport=TURBO_DC_TRANSPORT_ICE; ca.disable_sctp=cb.disable_sctp=1;
  ctx_a=turbo_dc_context_create(&ca);ctx_b=turbo_dc_context_create(&cb);assert(ctx_a&&ctx_b);
  {
    char short_hash[2] = {'X','X'};
    char short_fingerprint[8] = {'X','X','X','X','X','X','X','X'};
    assert(turbo_dc_context_get_local_fingerprint(ctx_a, short_hash,
        sizeof(short_hash), short_fingerprint, sizeof(short_fingerprint)) == -3);
    assert(short_hash[0] == '\0' && short_fingerprint[0] == '\0');
  }
  assert(turbo_dc_context_get_local_fingerprint(ctx_a,hash_a,sizeof(hash_a),fp_a,sizeof(fp_a))==0);
  assert(turbo_dc_context_get_local_fingerprint(ctx_b,hash_b,sizeof(hash_b),fp_b,sizeof(fp_b))==0);
  peer_a=turbo_dc_peer_create(ctx_a,NULL,0u,&a);peer_b=turbo_dc_peer_create(ctx_b,NULL,0u,&b);assert(peer_a&&peer_b);
  a.peer=peer_a;a.remote=peer_b;b.peer=peer_b;b.remote=peer_a;
  assert(turbo_dc_peer_set_dtls_role(peer_a,0)==0);assert(turbo_dc_peer_set_dtls_role(peer_b,1)==0);
  assert(turbo_dc_peer_set_remote_fingerprint(peer_a,hash_b,fp_b)==0);
  assert(turbo_dc_peer_set_remote_fingerprint(peer_b,hash_a,fp_a)==0);
  assert(dc_peer_set_test_dtls_application_handler(peer_a,app_data,&a)==0);
  assert(dc_peer_set_test_dtls_application_handler(peer_b,app_data,&b)==0);
  assert(turbo_dc_peer_set_external_transport(peer_a,&a,send_datagram)==0);
  assert(turbo_dc_peer_set_external_transport(peer_b,&b,send_datagram)==0);
  assert(dc_peer_post_owner_sync(peer_a,init_assoc,&a,&init_result)==0 && init_result==0);
  init_result=-1;assert(dc_peer_post_owner_sync(peer_b,init_assoc,&b,&init_result)==0 && init_result==0);
  /* Each Association owns an independent GmSSL cookie key. */
  assert(a.cookie_mac.active == 1u && b.cookie_mac.active == 1u);
  assert(memcmp(a.cookie_mac.key, b.cookie_mac.key,
                sizeof(a.cookie_mac.key)) != 0);
  turbo_dc_peer_on_state(peer_a,state_cb);turbo_dc_peer_on_state(peer_b,state_cb);
  assert(turbo_dc_peer_connect(peer_a)==0);
  for (unsigned i=0;i<5000u;i++) {
    if (atomic_load_explicit(&a.received,memory_order_acquire) ||
        atomic_load_explicit(&a.acked,memory_order_acquire)) {
      if (atomic_load_explicit(&b.received,memory_order_acquire) &&
          atomic_load_explicit(&a.acked,memory_order_acquire)) break;
    }
    if (atomic_load_explicit(&a.failure,memory_order_acquire) ||
        atomic_load_explicit(&b.failure,memory_order_acquire) || queue_failed(&q)) break;
    if (pop_packet(&q,&d)) {
      idle=0;turbo_dc_peer_feed_transport_data(d.dst,d.bytes,d.len);
    } else {
      if (++idle>1000u) break;
      cmeta_sleep_ms(1);
    }
  }
  assert(!atomic_load_explicit(&a.failure,memory_order_acquire));
  assert(!atomic_load_explicit(&b.failure,memory_order_acquire));
  assert(!queue_failed(&q));
  assert(atomic_load_explicit(&b.received,memory_order_acquire));
  assert(atomic_load_explicit(&a.acked,memory_order_acquire));
  assert(dc_peer_post_owner_sync(peer_a,cleanup_assoc,&a,&cleanup_result)==0 && cleanup_result==0);
  cleanup_result=-1;assert(dc_peer_post_owner_sync(peer_b,cleanup_assoc,&b,&cleanup_result)==0 && cleanup_result==0);
  /* The MAC provider is wiped only after Association stop/destroy completes. */
  assert(a.cookie_mac.active == 0u && b.cookie_mac.active == 0u);
  assert(memcmp(a.cookie_mac.key, (uint8_t[32]){0}, sizeof(a.cookie_mac.key)) == 0);
  assert(memcmp(b.cookie_mac.key, (uint8_t[32]){0}, sizeof(b.cookie_mac.key)) == 0);
  turbo_dc_peer_destroy(peer_a);turbo_dc_peer_destroy(peer_b);
  turbo_dc_context_destroy(ctx_a);turbo_dc_context_destroy(ctx_b);
  cmeta_mutex_destroy(&q.mutex);
}
int main(void) {
  /* This target links the installed Salts logger DSO, which is not itself
   * TSan-instrumented. Its async producer/consumer synchronization therefore
   * is invisible to TSan and can look like a race in the logger's copied
   * message buffer. Keep routine handshake DEBUG/INFO traffic out of this
   * lifecycle test; failures remain visible at ERROR level. Set this before
   * creating either Peer or its transport thread. */
  tlog_set_level(tlog_get_default(), SALTS_LOG_LEVEL_ERROR);
  test_real_peer_dtls_c11_roundtrip();
  puts("Real TurboMedia Peer DTLS <-> private C11 SCTP bridge PASS");
  return 0;
}
