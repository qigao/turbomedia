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
} handshake_owner_probe;

static void observe_owner_send(void *opaque, const void *data, size_t len) {
  handshake_owner_probe *probe = (handshake_owner_probe *)opaque;
  assert(probe != NULL && probe->peer != NULL);
  assert(data != NULL && len > 0u);
  assert(dc_peer_is_on_transport_owner(probe->peer));
  ++probe->sends;
}

static void test_external_dtls_handshake_runs_on_owner(void) {
  turbo_dc_config_t config = {0};
  handshake_owner_probe probe = {0};
  turbo_dc_context_t *ctx;

  config.transport = TURBO_DC_TRANSPORT_ICE;
  config.disable_sctp = 1;
  ctx = turbo_dc_context_create(&config);
  assert(ctx != NULL);
  probe.peer = turbo_dc_peer_create(ctx, NULL, 0u, NULL);
  assert(probe.peer != NULL);
  assert(turbo_dc_peer_set_dtls_role(probe.peer, 0) == 0);
  assert(turbo_dc_peer_is_dtls_server(probe.peer) == 0);
  assert(turbo_dc_peer_set_external_transport(
      probe.peer, &probe, observe_owner_send) == 0);

  /* The public caller is not the DataChannel owner. ClientHello generation
   * and the external send callback must nevertheless execute on the owner. */
  assert(turbo_dc_peer_connect(probe.peer) == 0);
  assert(probe.sends > 0u);

  turbo_dc_peer_destroy(probe.peer);
  turbo_dc_context_destroy(ctx);
}

int main(void) {
  test_peer_destroy_waits_for_inflight_transport_callback();
  test_external_dtls_handshake_runs_on_owner();
  puts("Production DataChannel Peer owner/drain contract PASS");
  return 0;
}
