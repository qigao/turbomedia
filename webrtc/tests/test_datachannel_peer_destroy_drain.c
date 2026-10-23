/*
 * Exercise the real TurboMedia DataChannel Peer destroy barrier.
 *
 * This test builds the first-party DataChannel/DTLS/SCTP sources directly into
 * a private executable so Linux CI can instrument those exact translation
 * units under ThreadSanitizer. It does not enable the private C11 SCTP engine
 * in production; it validates the existing Peer operation/callback drain
 * contract that a future C11 adapter must reuse.
 */
#include "turbo_datachannel.h"
#include "turbo_datachannel_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <salts/thread.h>
#include <tlog.h>

typedef struct peer_destroy_gate {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  turbo_dc_peer_t *peer;
  int callback_entered;
  int release_callback;
  int callback_returned;
  int feed_returned;
  int destroy_started;
  int destroy_returned;
} peer_destroy_gate;

static void transport_data_callback(void *opaque, const uint8_t *data,
                                    size_t len) {
  peer_destroy_gate *gate = (peer_destroy_gate *)opaque;
  assert(gate != NULL);
  /* External/ICE ingress is marshalled to the DataChannel state owner. */
  assert(dc_peer_is_on_transport_owner(gate->peer));
  assert(data != NULL && len == 4u);
  assert(data[0] == 0x80u && data[1] == 0x60u);

  /* Deregistering from inside the callback must not wait for itself. */
  turbo_dc_peer_set_transport_data_handler(gate->peer, NULL, NULL);

  cmeta_mutex_lock(&gate->mutex);
  gate->callback_entered = 1;
  cmeta_cond_broadcast(&gate->changed);
  while (!gate->release_callback)
    cmeta_cond_wait(&gate->changed, &gate->mutex);
  gate->callback_returned = 1;
  cmeta_cond_broadcast(&gate->changed);
  cmeta_mutex_unlock(&gate->mutex);
}

static void feed_thread(void *opaque) {
  peer_destroy_gate *gate = (peer_destroy_gate *)opaque;
  static const uint8_t rtp_packet[4] = {0x80u, 0x60u, 0u, 0u};
  turbo_dc_peer_feed_transport_data(gate->peer, rtp_packet,
                                    sizeof(rtp_packet));
  cmeta_mutex_lock(&gate->mutex);
  gate->feed_returned = 1;
  cmeta_cond_broadcast(&gate->changed);
  cmeta_mutex_unlock(&gate->mutex);
}

static void destroy_thread(void *opaque) {
  peer_destroy_gate *gate = (peer_destroy_gate *)opaque;
  cmeta_mutex_lock(&gate->mutex);
  gate->destroy_started = 1;
  cmeta_cond_broadcast(&gate->changed);
  cmeta_mutex_unlock(&gate->mutex);

  turbo_dc_peer_destroy(gate->peer);

  cmeta_mutex_lock(&gate->mutex);
  gate->destroy_returned = 1;
  cmeta_cond_broadcast(&gate->changed);
  cmeta_mutex_unlock(&gate->mutex);
}

static void test_peer_destroy_waits_for_inflight_transport_callback(void) {
  peer_destroy_gate gate = {0};
  cmeta_thread_t feeder = NULL;
  cmeta_thread_t destroyer = NULL;
  turbo_dc_config_t config = {0};
  unsigned attempt;
  int saw_destroying = 0;

  config.transport = TURBO_DC_TRANSPORT_ICE;
  config.disable_sctp = 1;

  cmeta_mutex_init(&gate.mutex);
  cmeta_cond_init(&gate.changed);
  assert(gate.mutex != NULL && gate.changed != NULL);

  turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  gate.peer = turbo_dc_peer_create(ctx, NULL, 0u, &gate);
  assert(gate.peer != NULL);

  turbo_dc_peer_set_transport_data_handler(gate.peer,
                                           transport_data_callback, &gate);
  assert(cmeta_thread_create(&feeder, feed_thread, &gate) == 0);

  cmeta_mutex_lock(&gate.mutex);
  while (!gate.callback_entered)
    cmeta_cond_wait(&gate.changed, &gate.mutex);
  cmeta_mutex_unlock(&gate.mutex);

  assert(cmeta_thread_create(&destroyer, destroy_thread, &gate) == 0);
  cmeta_mutex_lock(&gate.mutex);
  while (!gate.destroy_started)
    cmeta_cond_wait(&gate.changed, &gate.mutex);
  cmeta_mutex_unlock(&gate.mutex);

  /*
   * The production destroy path closes Peer admission and then waits on
   * operation_cond until active_operations and transport_data_callbacks are
   * zero. Poll the protected predicate with a bounded deadline because the
   * current implementation does not signal a separate "destroying" event.
   * The callback remains blocked, so the Peer cannot be freed in this loop.
   */
  for (attempt = 0u; attempt < 5000u; ++attempt) {
    int destroying;
    uint32_t active_operations;
    uint32_t active_callbacks;
    int ssl_alive;

    cmeta_mutex_lock(&gate.peer->operation_mutex);
    destroying = gate.peer->destroying;
    active_operations = gate.peer->active_operations;
    active_callbacks = gate.peer->transport_data_callbacks;
    ssl_alive = gate.peer->dtls.ssl != NULL;
    cmeta_mutex_unlock(&gate.peer->operation_mutex);

    if (destroying) {
      assert(active_operations >= 1u);
      assert(active_callbacks == 1u);
      assert(ssl_alive);
      saw_destroying = 1;
      break;
    }
    cmeta_sleep_ms(1u);
  }
  assert(saw_destroying);

  cmeta_mutex_lock(&gate.mutex);
  assert(!gate.callback_returned);
  assert(!gate.feed_returned);
  assert(!gate.destroy_returned);
  gate.release_callback = 1;
  cmeta_cond_broadcast(&gate.changed);
  cmeta_mutex_unlock(&gate.mutex);

  assert(cmeta_thread_join(&feeder) == 0);
  assert(cmeta_thread_join(&destroyer) == 0);

  cmeta_mutex_lock(&gate.mutex);
  assert(gate.callback_returned);
  assert(gate.feed_returned);
  assert(gate.destroy_returned);
  cmeta_mutex_unlock(&gate.mutex);

  /* The Peer was unlinked/freed by destroyer; context teardown is now safe. */
  turbo_dc_context_destroy(ctx);
  cmeta_cond_destroy(&gate.changed);
  cmeta_mutex_destroy(&gate.mutex);
}

typedef struct handshake_owner_probe {
  turbo_dc_peer_t *peer;
  unsigned sends;
  unsigned state_callbacks;
} handshake_owner_probe;

static void observe_owner_send(void *opaque, const void *data, size_t len) {
  handshake_owner_probe *probe = (handshake_owner_probe *)opaque;
  assert(probe != NULL && probe->peer != NULL);
  assert(data != NULL && len > 0u);
  assert(dc_peer_is_on_transport_owner(probe->peer));
  ++probe->sends;
}

static void observe_owner_state(turbo_dc_peer_t *peer,
                                turbo_dc_state_t old_state,
                                turbo_dc_state_t new_state,
                                void *opaque) {
  handshake_owner_probe *probe = (handshake_owner_probe *)opaque;
  (void)old_state;
  (void)new_state;
  assert(probe != NULL && probe->peer == peer);
  assert(dc_peer_is_on_transport_owner(peer));
  ++probe->state_callbacks;
}

static void test_external_dtls_handshake_runs_on_owner(void) {
  turbo_dc_config_t config = {0};
  handshake_owner_probe probe = {0};
  turbo_dc_context_t *ctx;

  config.transport = TURBO_DC_TRANSPORT_ICE;
  config.disable_sctp = 1;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  turbo_dc_peer_on_state(probe.peer, observe_owner_state);
  assert(turbo_dc_peer_set_dtls_role(probe.peer, 0) == 0);
  assert(turbo_dc_peer_is_dtls_server(probe.peer) == 0);
  assert(turbo_dc_peer_set_remote_fingerprint(
      probe.peer, "sha-256",
      "AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:"
      "AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA:AA") == 0);
  assert(turbo_dc_peer_set_external_transport(
      probe.peer, &probe, observe_owner_send) == 0);

  /* The public caller is not the DataChannel owner. External sends, the
   * ClientHello, state callbacks, and SRTP-key queries use the owner. */
  {
    static const uint8_t probe_packet[2] = {0x16u, 0xfeu};
    uint8_t key_material[64] = {0};
    turbo_dc_peer_send_transport_data(probe.peer, probe_packet,
                                      sizeof(probe_packet));
    assert(probe.sends == 1u);
    assert(turbo_dc_peer_get_srtp_keys(probe.peer, key_material) == 0);
  }
  assert(turbo_dc_peer_connect(probe.peer) == 0);
  assert(probe.sends > 0u);
  turbo_dc_peer_close(probe.peer);
  assert(probe.state_callbacks >= 1u);

  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
}

static void unused_channel_open(turbo_dc_channel_t *channel, void *opaque) {
  (void)channel;
  (void)opaque;
}

static void unused_channel_message(turbo_dc_channel_t *channel,
                                   const void *data, size_t len,
                                   int is_binary, void *opaque) {
  (void)channel;
  (void)data;
  (void)len;
  (void)is_binary;
  (void)opaque;
}

static void unused_channel_close(turbo_dc_channel_t *channel, void *opaque) {
  (void)channel;
  (void)opaque;
}

static void unused_peer_channel(turbo_dc_peer_t *peer,
                                turbo_dc_channel_t *channel, void *opaque) {
  (void)peer;
  (void)channel;
  (void)opaque;
}

static void unused_peer_error(turbo_dc_peer_t *peer, int error_code,
                              const char *error_message, void *opaque) {
  (void)peer;
  (void)error_code;
  (void)error_message;
  (void)opaque;
}

static void test_channel_callback_metadata_is_owner_dispatched(void) {
  turbo_dc_config_t config = {0};
  turbo_dc_context_t *ctx;
  turbo_dc_peer_t *peer;
  turbo_dc_channel_t *channel;
  int marker = 42;

  config.transport = TURBO_DC_TRANSPORT_ICE;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  peer = turbo_dc_peer_create(ctx, NULL, 0u, NULL);
  assert(peer != NULL);
  turbo_dc_peer_on_channel(peer, unused_peer_channel);
  turbo_dc_peer_on_error(peer, unused_peer_error);

  channel = turbo_dc_channel_create(peer, "owner-contract", NULL);
  assert(channel != NULL);
  turbo_dc_channel_on_open(channel, unused_channel_open);
  turbo_dc_channel_on_message(channel, unused_channel_message);
  turbo_dc_channel_on_close(channel, unused_channel_close);
  turbo_dc_channel_set_user_data(channel, &marker);
  assert(turbo_dc_channel_get_user_data(channel) == &marker);
  assert(strcmp(turbo_dc_channel_get_label(channel), "owner-contract") == 0);
  assert(turbo_dc_channel_get_id(channel) == channel->id);
  assert(turbo_dc_channel_is_open(channel) == 0);
  assert(turbo_dc_channel_buffered_amount(channel) == 0u);

  turbo_dc_peer_destroy(peer);
  turbo_dc_context_destroy(ctx);
}

typedef struct callback_destroy_probe {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  turbo_dc_peer_t *peer;
  unsigned callbacks;
  int callback_returned;
  int retired;
} callback_destroy_probe;

static void request_callback_destroy(callback_destroy_probe *probe) {
  turbo_dc_peer_t *peer = probe->peer;
  assert(dc_peer_is_on_transport_owner(peer));
  ++probe->callbacks;
  turbo_dc_peer_destroy(peer);
  turbo_dc_peer_destroy(peer); /* Reentrant repeat is idempotent. */
  assert(dc_peer_acquire(peer) != 0); /* Admission closes immediately. */
  assert(peer->dtls.ssl != NULL);    /* Callback still owns live storage. */
  probe->callback_returned = 1;
}

static void destroy_from_state(turbo_dc_peer_t *peer,
                               turbo_dc_state_t old_state,
                               turbo_dc_state_t new_state, void *opaque) {
  (void)peer;
  (void)old_state;
  (void)new_state;
  request_callback_destroy((callback_destroy_probe *)opaque);
}

static void destroy_from_error(turbo_dc_peer_t *peer, int code,
                               const char *message, void *opaque) {
  (void)peer;
  (void)code;
  (void)message;
  request_callback_destroy((callback_destroy_probe *)opaque);
}

static void destroy_from_transport(void *opaque, const uint8_t *data,
                                   size_t len) {
  (void)data;
  (void)len;
  request_callback_destroy((callback_destroy_probe *)opaque);
}

static void observe_retirement(turbo_dc_peer_t *peer) {
  callback_destroy_probe *probe = (callback_destroy_probe *)peer->user_data;
  assert(dc_peer_is_on_transport_owner(peer));
  assert(probe->callback_returned);
  assert(peer->active_operations == 0u);
  assert(peer->transport_data_callbacks == 0u);
  cmeta_mutex_lock(&probe->mutex);
  probe->retired = 1;
  cmeta_cond_broadcast(&probe->changed);
  cmeta_mutex_unlock(&probe->mutex);
}

static void trigger_error(void *arg1, void *arg2) {
  (void)arg2;
  dc_fail_peer((turbo_dc_peer_t *)arg1, TURBO_DC_ERROR_DTLS_HANDSHAKE,
               "callback destruction regression");
}

static void test_destroy_from_owner_callback(unsigned kind) {
  static const dc_transport_ops_t ops = {NULL, NULL, observe_retirement};
  static const uint8_t rtp[4] = {0x80u, 0x60u, 0u, 0u};
  turbo_dc_config_t config = {0};
  callback_destroy_probe probe = {0};
  turbo_dc_context_t *ctx;
  config.transport = TURBO_DC_TRANSPORT_ICE;
  config.disable_sctp = 1;
  cmeta_mutex_init(&probe.mutex);
  cmeta_cond_init(&probe.changed);
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  /* Configure before publication to the active owner. */
  assert(dc_peer_acquire(probe.peer) == 0);
  probe.peer->transport_ops = &ops;
  if (kind == 0u) {
    turbo_dc_peer_on_state(probe.peer, destroy_from_state);
    turbo_dc_peer_close(probe.peer);
  } else if (kind == 1u) {
    turbo_dc_peer_on_error(probe.peer, destroy_from_error);
    turbo_dc_peer_on_state(probe.peer, destroy_from_state);
    assert(dc_peer_post_owner_sync(probe.peer, trigger_error,
                                   probe.peer, NULL) == SALTS_OK);
  } else {
    turbo_dc_peer_set_transport_data_handler(probe.peer,
                                             destroy_from_transport, &probe);
    turbo_dc_peer_feed_transport_data(probe.peer, rtp, sizeof(rtp));
  }
  assert(probe.callback_returned);
  assert(probe.callbacks == 1u); /* No later callback after destroy request. */
  cmeta_mutex_lock(&probe.mutex);
  assert(!probe.retired); /* An independent admitted lease still holds it. */
  cmeta_mutex_unlock(&probe.mutex);
  dc_peer_release(probe.peer);
  cmeta_mutex_lock(&probe.mutex);
  while (!probe.retired)
    assert(cmeta_cond_timedwait(&probe.changed, &probe.mutex,
                               UINT64_C(5000000000)) == 0);
  cmeta_mutex_unlock(&probe.mutex);
  /* Context destruction also waits for unlink/free after the observer fires. */
  turbo_dc_context_destroy(ctx);
  cmeta_cond_destroy(&probe.changed);
  cmeta_mutex_destroy(&probe.mutex);
}

typedef struct state_reentry_probe {
  turbo_dc_peer_t *peer;
  unsigned errors;
  unsigned failed;
  unsigned disconnecting;
  unsigned closed;
  unsigned connected;
  int close_from_error;
} state_reentry_probe;

static void observe_terminal_state(turbo_dc_peer_t *peer,
                                   turbo_dc_state_t old_state,
                                   turbo_dc_state_t new_state, void *opaque) {
  state_reentry_probe *probe = (state_reentry_probe *)opaque;
  (void)old_state;
  assert(probe != NULL && probe->peer == peer);
  assert(dc_peer_is_on_transport_owner(peer));
  if (new_state == TURBO_DC_STATE_FAILED) ++probe->failed;
  if (new_state == TURBO_DC_STATE_DISCONNECTING) ++probe->disconnecting;
  if (new_state == TURBO_DC_STATE_CLOSED) ++probe->closed;
  if (new_state == TURBO_DC_STATE_CONNECTED) ++probe->connected;
}

static void close_from_terminal_error(turbo_dc_peer_t *peer, int code,
                                      const char *message, void *opaque) {
  state_reentry_probe *probe = (state_reentry_probe *)opaque;
  assert(probe != NULL && probe->peer == peer);
  assert(code == TURBO_DC_ERROR_DTLS_HANDSHAKE);
  assert(message != NULL);
  assert(dc_peer_is_on_transport_owner(peer));
  assert(peer->state == TURBO_DC_STATE_FAILED);
  ++probe->errors;
  if (probe->close_from_error) turbo_dc_peer_close(peer);
}

static void fail_then_publish_connected(void *arg1, void *arg2) {
  state_reentry_probe *probe = (state_reentry_probe *)arg1;
  (void)arg2;
  assert(probe != NULL && probe->peer != NULL);
  assert(dc_peer_is_on_transport_owner(probe->peer));
  dc_fail_peer(probe->peer, TURBO_DC_ERROR_DTLS_HANDSHAKE,
               "terminal state publication regression");
  dc_notify_state(probe->peer, TURBO_DC_STATE_CONNECTED);
}

static void test_terminal_state_publication(int close_from_error) {
  turbo_dc_config_t config = {0};
  state_reentry_probe probe = {0};
  turbo_dc_context_t *ctx;
  config.transport = TURBO_DC_TRANSPORT_ICE;
  config.disable_sctp = 1;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  probe.close_from_error = close_from_error;
  turbo_dc_peer_on_state(probe.peer, observe_terminal_state);
  turbo_dc_peer_on_error(probe.peer, close_from_terminal_error);
  assert(dc_peer_acquire(probe.peer) == 0);
  assert(dc_peer_post_owner_sync(probe.peer, fail_then_publish_connected,
                                 &probe, NULL) == SALTS_OK);
  dc_peer_release(probe.peer);

  assert(probe.errors == 1u);
  assert(probe.connected == 0u);
  if (close_from_error) {
    assert(probe.failed == 0u);
    assert(probe.disconnecting == 1u);
    assert(probe.closed == 1u);
    assert(turbo_dc_peer_get_state(probe.peer) == TURBO_DC_STATE_CLOSED);
  } else {
    assert(probe.failed == 1u);
    assert(probe.disconnecting == 0u);
    assert(probe.closed == 0u);
    assert(turbo_dc_peer_get_state(probe.peer) == TURBO_DC_STATE_FAILED);
  }

  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
}

static void retransmit_while_owner_busy(void *arg1, void *arg2) {
  handshake_owner_probe *probe = (handshake_owner_probe *)arg1;
  turbo_dc_peer_t *peer = probe->peer;
  unsigned sends;
  (void)arg2;
  assert(dc_peer_is_on_transport_owner(peer));
  assert(turbo_dc_peer_connect(peer) == 0);
  sends = probe->sends;
  assert(sends > 0u && peer->dtls.retransmit_timer != NULL);

  /* Hold the actual owner while the actual native timer expires. The timer
   * must publish and return without waiting for this owner task. */
  cmeta_mutex_lock(&peer->operation_mutex);
  while (!peer->dtls_retransmit_pending)
    assert(cmeta_cond_timedwait(&peer->operation_cond, &peer->operation_mutex,
                               UINT64_C(5000000000)) == 0);
  cmeta_mutex_unlock(&peer->operation_mutex);
  dtls_poll_retransmit(peer); /* Real timeout retransmission and timer rearm. */
  assert(probe->sends > sends);
  cmeta_mutex_lock(&peer->operation_mutex);
  while (!peer->dtls_retransmit_pending)
    assert(cmeta_cond_timedwait(&peer->operation_cond, &peer->operation_mutex,
                               UINT64_C(5000000000)) == 0);
  cmeta_mutex_unlock(&peer->operation_mutex);
  peer->dtls.handshake_done = 1;
  dtls_poll_retransmit(peer); /* Completion stops the native timer on owner. */
  assert(!peer->dtls_retransmit_pending);
  dtls_session_cleanup(peer);
  assert(peer->dtls.retransmit_timer == NULL);
}

static void test_native_timer_does_not_wait_for_owner(void) {
  turbo_dc_config_t config = {0};
  handshake_owner_probe probe = {0};
  turbo_dc_context_t *ctx;
  config.transport = TURBO_DC_TRANSPORT_ICE;
  config.disable_sctp = 1;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  assert(turbo_dc_peer_set_dtls_role(probe.peer, 0) == 0);
  assert(turbo_dc_peer_set_external_transport(
      probe.peer, &probe, observe_owner_send) == 0);
  assert(dc_peer_acquire(probe.peer) == 0);
  assert(dc_peer_post_owner_sync(probe.peer, retransmit_while_owner_busy,
                                 &probe, NULL) == SALTS_OK);
  dc_peer_release(probe.peer);
  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
}

#ifdef TURBO_MEDIA_TEST_CHANNEL_SENDV
static unsigned g_channel_send_calls;
static uint16_t g_channel_send_sid;
static uint32_t g_channel_send_ppid;
static size_t g_channel_send_length;
static uint8_t g_channel_send_first_byte;

ssize_t turbo_media_test_channel_sendv(
    struct socket *socket, const void *data, size_t length,
    struct sockaddr *address, int address_count, void *info,
    socklen_t info_length, unsigned int info_type, int flags) {
  const struct sctp_sendv_spa *spa = (const struct sctp_sendv_spa *)info;
  assert(socket != NULL && data != NULL && length > 0u);
  assert(address == NULL && address_count == 0);
  assert(spa != NULL && info_length == sizeof(*spa));
  assert(info_type == SCTP_SENDV_SPA && flags == 0);
  assert((spa->sendv_flags & SCTP_SEND_SNDINFO_VALID) != 0);
  ++g_channel_send_calls;
  g_channel_send_sid = spa->sendv_sndinfo.snd_sid;
  g_channel_send_ppid = ntohl(spa->sendv_sndinfo.snd_ppid);
  g_channel_send_length = length;
  g_channel_send_first_byte = ((const uint8_t *)data)[0];
  return (ssize_t)length;
}
#endif

#ifdef TURBO_MEDIA_TEST_DCEP_SENDV
static unsigned g_dcep_ack_admissions;

ssize_t turbo_media_test_dcep_sendv(
    struct socket *socket, const void *data, size_t length,
    struct sockaddr *address, int address_count, void *info,
    socklen_t info_length, unsigned int info_type, int flags) {
  const struct sctp_sendv_spa *spa = (const struct sctp_sendv_spa *)info;
  assert(socket != NULL);
  assert(data != NULL && length == 1u);
  assert(((const uint8_t *)data)[0] == DCEP_DATA_CHANNEL_ACK);
  assert(address == NULL && address_count == 0);
  assert(spa != NULL && info_length == sizeof(*spa));
  assert(info_type == SCTP_SENDV_SPA && flags == 0);
  assert((spa->sendv_flags & SCTP_SEND_SNDINFO_VALID) != 0);
  assert(spa->sendv_sndinfo.snd_sid == 0u);
  assert(ntohl(spa->sendv_sndinfo.snd_ppid) == SCTP_PPID_DCEP);
  ++g_dcep_ack_admissions;
  return (ssize_t)length;
}
#endif

typedef struct channel_lease_probe {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  turbo_dc_peer_t *peer;
  turbo_dc_channel_t *channel;
  uint16_t sid;
  unsigned channels;
  unsigned opens;
  unsigned closes;
  unsigned messages;
  size_t last_length;
  int last_binary;
  uint8_t last_byte;
  int entered;
  int release;
  int read_after_close;
} channel_lease_probe;

static void receive_test_packet(channel_lease_probe *probe, uint32_t ppid,
                                 const uint8_t *bytes, size_t length) {
  union sctp_sockstore address;
  struct sctp_rcvinfo info;
  void *packet = malloc(length);
  assert(packet != NULL);
  memcpy(packet, bytes, length);
  memset(&address, 0, sizeof(address));
  memset(&info, 0, sizeof(info));
  info.rcv_sid = probe->sid;
  info.rcv_ppid = htonl(ppid);
  assert(sctp_inbound_packet_cb(NULL, address, packet, length, info,
                                0, probe->peer) == 1);
}

static void channel_open_probe(turbo_dc_channel_t *channel, void *opaque) {
  channel_lease_probe *probe = (channel_lease_probe *)opaque;
  (void)channel;
  ++probe->opens;
}

static void channel_close_probe(turbo_dc_channel_t *channel, void *opaque) {
  channel_lease_probe *probe = (channel_lease_probe *)opaque;
  ++probe->closes;
  assert(strcmp(turbo_dc_channel_get_label(channel), "x") == 0);
  /* Closing from on_close must not issue another reset/callback/free. */
  turbo_dc_channel_close(channel);
}

static void close_during_dcep(turbo_dc_peer_t *peer,
                               turbo_dc_channel_t *channel, void *opaque) {
  channel_lease_probe *probe = (channel_lease_probe *)opaque;
  ++probe->channels;
  turbo_dc_channel_set_user_data(channel, probe);
  turbo_dc_channel_on_open(channel, channel_open_probe);
  turbo_dc_channel_on_close(channel, channel_close_probe);
  turbo_dc_channel_close(channel);
  assert(strcmp(channel->label, "x") == 0);
  cmeta_mutex_lock(&peer->operation_mutex);
  assert(peer_get_channel(peer, channel->id) == NULL);
  assert(roaring_bitmap_contains(peer->channel_ids, channel->id));
  cmeta_mutex_unlock(&peer->operation_mutex);
}

static void dcep_close_owner_task(void *arg1, void *arg2) {
  static const uint8_t open[] = {
    DCEP_DATA_CHANNEL_OPEN, DCEP_CHANNEL_RELIABLE_ORDERED,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 'x'
  };
  (void)arg2;
  receive_test_packet((channel_lease_probe *)arg1, SCTP_PPID_DCEP,
                       open, sizeof(open));
}

static void test_dcep_reentrant_close_keeps_channel_until_return(void) {
  turbo_dc_config_t config = {0};
  channel_lease_probe probe = {0};
  turbo_dc_context_t *ctx;
  config.is_server = 1;
  config.transport = TURBO_DC_TRANSPORT_ICE;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  turbo_dc_peer_on_channel(probe.peer, close_during_dcep);
  assert(dc_peer_acquire(probe.peer) == 0);
#ifdef TURBO_MEDIA_TEST_DCEP_SENDV
  g_dcep_ack_admissions = 0u;
#endif
  assert(dc_peer_post_owner_sync(probe.peer, dcep_close_owner_task,
                                 &probe, NULL) == SALTS_OK);
#ifdef TURBO_MEDIA_TEST_DCEP_SENDV
  assert(g_dcep_ack_admissions == 1u);
#endif
  assert(probe.channels == 1u && probe.opens == 0u && probe.closes == 1u);
  cmeta_mutex_lock(&probe.peer->operation_mutex);
  assert(peer_get_channel(probe.peer, probe.sid) == NULL);
  assert(!roaring_bitmap_contains(probe.peer->channel_ids, probe.sid));
  cmeta_mutex_unlock(&probe.peer->operation_mutex);
  dc_peer_release(probe.peer);
  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
}

static void open_channel_on_owner(void *arg1, void *arg2);

typedef struct webrtc_receive_command {
  channel_lease_probe *probe;
  uint32_t ppid;
  const uint8_t *bytes;
  size_t length;
} webrtc_receive_command;

static void observe_webrtc_message(turbo_dc_channel_t *channel,
                                   const void *data, size_t length,
                                   int binary, void *opaque) {
  channel_lease_probe *probe = (channel_lease_probe *)opaque;
  assert(channel == probe->channel);
  ++probe->messages;
  probe->last_length = length;
  probe->last_binary = binary;
  probe->last_byte = length ? ((const uint8_t *)data)[0] : 0u;
}

static void receive_webrtc_message_on_owner(void *arg1, void *arg2) {
  webrtc_receive_command *command = (webrtc_receive_command *)arg1;
  (void)arg2;
  receive_test_packet(command->probe, command->ppid,
                      command->bytes, command->length);
}

static void deliver_webrtc_message(channel_lease_probe *probe, uint32_t ppid,
                                   const uint8_t *bytes, size_t length) {
  webrtc_receive_command command = {probe, ppid, bytes, length};
  assert(dc_peer_acquire(probe->peer) == 0);
  assert(dc_peer_post_owner_sync(probe->peer,
                                 receive_webrtc_message_on_owner,
                                 &command, NULL) == SALTS_OK);
  dc_peer_release(probe->peer);
}

static void test_webrtc_empty_ppid_framing(void) {
  static const uint8_t zero[] = {0u};
  static const uint8_t malformed[] = {1u};
  static const uint8_t text[] = {'x'};
  turbo_dc_config_t config = {0};
  channel_lease_probe probe = {0};
  turbo_dc_context_t *ctx;
  config.transport = TURBO_DC_TRANSPORT_ICE;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  probe.channel = turbo_dc_channel_create(probe.peer, "webrtc-ppid", NULL);
  assert(probe.channel != NULL);
  probe.sid = turbo_dc_channel_get_id(probe.channel);
  turbo_dc_channel_set_user_data(probe.channel, &probe);
  turbo_dc_channel_on_message(probe.channel, observe_webrtc_message);
  assert(dc_peer_acquire(probe.peer) == 0);
  assert(dc_peer_post_owner_sync(probe.peer, open_channel_on_owner,
                                 &probe, NULL) == SALTS_OK);
  dc_peer_release(probe.peer);
  assert(turbo_dc_channel_is_open(probe.channel));

#ifdef TURBO_MEDIA_TEST_CHANNEL_SENDV
  g_channel_send_calls = 0u;
  assert(turbo_dc_channel_send(probe.channel, zero, 0u, 0) == 0);
  assert(g_channel_send_calls == 1u);
  assert(g_channel_send_sid == probe.sid);
  assert(g_channel_send_ppid == SCTP_PPID_STRING_EMPTY);
  assert(g_channel_send_length == 1u && g_channel_send_first_byte == 0u);

  assert(turbo_dc_channel_send(probe.channel, zero, 0u, 1) == 0);
  assert(g_channel_send_calls == 2u);
  assert(g_channel_send_ppid == SCTP_PPID_BINARY_EMPTY);
  assert(g_channel_send_length == 1u && g_channel_send_first_byte == 0u);

  assert(turbo_dc_channel_send(probe.channel, text, sizeof(text), 0) == 0);
  assert(g_channel_send_calls == 3u);
  assert(g_channel_send_ppid == SCTP_PPID_STRING);
  assert(g_channel_send_length == 1u && g_channel_send_first_byte == 'x');
#endif

  deliver_webrtc_message(&probe, SCTP_PPID_STRING_EMPTY, zero, sizeof(zero));
  assert(probe.messages == 1u && probe.last_length == 0u &&
         probe.last_binary == 0);
  deliver_webrtc_message(&probe, SCTP_PPID_BINARY_EMPTY, zero, sizeof(zero));
  assert(probe.messages == 2u && probe.last_length == 0u &&
         probe.last_binary == 1);
  deliver_webrtc_message(&probe, SCTP_PPID_STRING, text, sizeof(text));
  assert(probe.messages == 3u && probe.last_length == 1u &&
         probe.last_binary == 0 && probe.last_byte == 'x');

  deliver_webrtc_message(&probe, SCTP_PPID_STRING_EMPTY,
                          malformed, sizeof(malformed));
  assert(probe.messages == 3u);
  assert(turbo_dc_peer_get_error(probe.peer).code ==
         TURBO_DC_ERROR_SCTP_RECEIVE);
  deliver_webrtc_message(&probe, 52u, zero, sizeof(zero));
  assert(probe.messages == 3u);
  assert(turbo_dc_peer_get_error(probe.peer).code ==
         TURBO_DC_ERROR_SCTP_RECEIVE);

  turbo_dc_channel_close(probe.channel);
  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
}

static void held_channel_message(turbo_dc_channel_t *channel,
                                  const void *data, size_t length,
                                  int binary, void *opaque) {
  channel_lease_probe *probe = (channel_lease_probe *)opaque;
  assert(!binary && length == 3u && memcmp(data, "abc", 3u) == 0);
  cmeta_mutex_lock(&probe->mutex);
  ++probe->messages;
  probe->entered = 1;
  cmeta_cond_broadcast(&probe->changed);
  while (!probe->release)
    cmeta_cond_wait(&probe->changed, &probe->mutex);
  /* The real SCTP callback still owns the unlinked channel allocation. */
  assert(strcmp(channel->label, "borrowed") == 0);
  probe->read_after_close = 1;
  cmeta_mutex_unlock(&probe->mutex);
}

static void channel_receive_worker(void *opaque) {
  static const uint8_t bytes[] = {'a', 'b', 'c'};
  receive_test_packet((channel_lease_probe *)opaque, SCTP_PPID_STRING,
                       bytes, sizeof(bytes));
}

static void open_channel_on_owner(void *arg1, void *arg2) {
  channel_lease_probe *probe = (channel_lease_probe *)arg1;
  static const uint8_t ack[] = {DCEP_DATA_CHANNEL_ACK};
  (void)arg2;
  receive_test_packet(probe, SCTP_PPID_DCEP, ack, sizeof(ack));
}

static void test_sctp_callback_retains_closed_channel(void) {
  turbo_dc_config_t config = {0};
  channel_lease_probe probe = {0};
  cmeta_thread_t receiver = NULL;
  turbo_dc_context_t *ctx;
  config.transport = TURBO_DC_TRANSPORT_ICE;
  cmeta_mutex_init(&probe.mutex);
  cmeta_cond_init(&probe.changed);
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, &probe);
  assert(probe.peer != NULL);
  probe.channel = turbo_dc_channel_create(probe.peer, "borrowed", NULL);
  assert(probe.channel != NULL);
  probe.sid = turbo_dc_channel_get_id(probe.channel);
  turbo_dc_channel_set_user_data(probe.channel, &probe);
  turbo_dc_channel_on_message(probe.channel, held_channel_message);
  assert(dc_peer_acquire(probe.peer) == 0);
  assert(dc_peer_post_owner_sync(probe.peer, open_channel_on_owner,
                                 &probe, NULL) == SALTS_OK);
  dc_peer_release(probe.peer);
  assert(cmeta_thread_create(&receiver, channel_receive_worker, &probe) == 0);
  cmeta_mutex_lock(&probe.mutex);
  while (!probe.entered)
    cmeta_cond_wait(&probe.changed, &probe.mutex);
  cmeta_mutex_unlock(&probe.mutex);

  /* Close must return while the native receive worker is held in user code. */
  turbo_dc_channel_close(probe.channel);
  cmeta_mutex_lock(&probe.peer->operation_mutex);
  assert(peer_get_channel(probe.peer, probe.sid) == NULL);
  assert(probe.channel->closing && probe.channel->callback_refs == 1u);
  assert(roaring_bitmap_contains(probe.peer->channel_ids, probe.sid));
  cmeta_mutex_unlock(&probe.peer->operation_mutex);
  channel_receive_worker(&probe); /* No new callback after unpublish. */
  cmeta_mutex_lock(&probe.mutex);
  assert(probe.messages == 1u);
  probe.release = 1;
  cmeta_cond_broadcast(&probe.changed);
  cmeta_mutex_unlock(&probe.mutex);
  assert(cmeta_thread_join(&receiver) == 0);
  assert(probe.read_after_close);
  cmeta_mutex_lock(&probe.peer->operation_mutex);
  assert(!roaring_bitmap_contains(probe.peer->channel_ids, probe.sid));
  cmeta_mutex_unlock(&probe.peer->operation_mutex);
  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
  cmeta_cond_destroy(&probe.changed);
  cmeta_mutex_destroy(&probe.mutex);
}

int main(void) {
  /* Match the Peer bridge fixture: the installed Salts logger DSO is not
   * TSan-instrumented, so its async queue's synchronization is not visible
   * to this source-instrumented Peer test. Exclude routine handshake traffic
   * before starting workers; ERROR/FATAL diagnostics and every lifecycle
   * assertion remain enabled. This does not qualify the external logger. */
  tlog_set_level(tlog_get_default(), SALTS_LOG_LEVEL_ERROR);
  test_peer_destroy_waits_for_inflight_transport_callback();
  test_external_dtls_handshake_runs_on_owner();
  test_channel_callback_metadata_is_owner_dispatched();
  test_destroy_from_owner_callback(0u);
  test_destroy_from_owner_callback(1u);
  test_destroy_from_owner_callback(2u);
  test_terminal_state_publication(0);
  test_terminal_state_publication(1);
  test_native_timer_does_not_wait_for_owner();
  test_dcep_reentrant_close_keeps_channel_until_return();
  test_webrtc_empty_ppid_framing();
  test_sctp_callback_retains_closed_channel();
  puts("Production DataChannel Peer owner/drain contract PASS");
  return 0;
}
