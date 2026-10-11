/** DTLS session owner: serialized GmSSL engine, callbacks outside admission. */
#include "turbo_datachannel_internal.h"
#include <cmeta_crypto.h>
#include <salts/clock.h>
#include <string.h>
#include "tlog.h"

static void dtls_set_timer_active(turbo_dc_peer_t *peer, int active) {
    if (peer->dtls.timer_active == active) return;
    cmeta_mutex_lock(&peer->ctx->transport_mutex);
    peer->dtls.timer_active = active;
    if (active) ++peer->ctx->active_dtls_timers;
    else --peer->ctx->active_dtls_timers;
    cmeta_cond_broadcast(&peer->ctx->transport_cond);
    cmeta_mutex_unlock(&peer->ctx->transport_mutex);
}

void dtls_session_lock(turbo_dc_peer_t *peer) {
    cmeta_mutex_lock(&peer->operation_mutex);
    while (peer->dtls.busy) {
        cmeta_cond_wait(&peer->operation_cond, &peer->operation_mutex);
    }
    peer->dtls.busy = 1;
    cmeta_mutex_unlock(&peer->operation_mutex);
}

void dtls_session_unlock(turbo_dc_peer_t *peer) {
    cmeta_mutex_lock(&peer->operation_mutex);
    peer->dtls.busy = 0;
    cmeta_cond_broadcast(&peer->operation_cond);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

/* Under admission: commit terminal status and publish timer demand together. */
static void dtls_commit_result(turbo_dc_peer_t *peer, int result) {
    if (result) peer->dtls.stopped = 1;
    dtls_set_timer_active(peer, !peer->dtls.stopped &&
        turbo_gdtls_deadline(peer->dtls.engine) != 0);
}

static void dtls_report_error(turbo_dc_peer_t *peer, int result) {
    if (!result) return;
    if (result == TURBO_GDTLS_CLOSED) {
        dc_notify_state(peer, TURBO_DC_STATE_CLOSED);
        return;
    }
    const char *detail = "DTLS protocol failure";
    turbo_dc_error_code_t code = TURBO_DC_ERROR_DTLS_HANDSHAKE;
    switch (result) {
        case TURBO_GDTLS_AUTH:
            code = TURBO_DC_ERROR_DTLS_FINGERPRINT;
            detail = "DTLS identity or handshake authentication failed";
            break;
        case TURBO_GDTLS_NOMEM: detail = "DTLS allocation failed"; break;
        case TURBO_GDTLS_CAPACITY: detail = "DTLS capacity exceeded"; break;
        case TURBO_GDTLS_TIMEOUT: detail = "DTLS handshake deadline expired"; break;
        case TURBO_GDTLS_INVALID: detail = "DTLS input or session configuration invalid"; break;
        default: break;
    }
    dc_fail_peer(peer, code, detail);
}

/* The public setter validates and canonicalizes the colon-separated pin.
 * Create lazily: signaling can select the role/pin after peer allocation. */
static int dtls_start_locked(turbo_dc_peer_t *peer) {
    if (peer->dtls.handshake_started) return 0;
    peer->dtls.handshake_started = 1;
    if (!peer->remote_fingerprint || tstr_len(peer->remote_fingerprint) != 95)
        return TURBO_GDTLS_AUTH;
    uint8_t expected[32];
    for (size_t i = 0; i < sizeof(expected); ++i) {
        unsigned char high = (unsigned char)peer->remote_fingerprint[i * 3];
        unsigned char low = (unsigned char)peer->remote_fingerprint[i * 3 + 1];
        high = high <= '9' ? high - '0' : high - 'A' + 10;
        low = low <= '9' ? low - '0' : low - 'A' + 10;
        expected[i] = (uint8_t)((high << 4) | low);
    }
    static const uint16_t profiles[] = {1, 2, 7, 8};
    peer->dtls.engine = turbo_gdtls_create(peer->ctx->dtls_context,
        peer->is_dtls_server, peer->ctx->dtls_mtu, expected, profiles,
        sizeof(profiles) / sizeof(profiles[0]));
    if (!peer->dtls.engine) return TURBO_GDTLS_NOMEM;
    return turbo_gdtls_start(peer->dtls.engine, cmeta_monotonic_ms());
}

int dtls_session_init(turbo_dc_peer_t *peer) {
    /* The context is already validated; the session awaits the signaling pin. */
    return peer->ctx->dtls_context ? 0 : -1;
}

void dtls_session_shutdown(turbo_dc_peer_t *peer) {
    dtls_session_lock(peer);
    peer->dtls.stopped = 1;
    dtls_set_timer_active(peer, 0);
    /* Local shutdown has historically suppressed network callbacks. Destroy
     * here also wipes incomplete-handshake secrets immediately. */
    turbo_gdtls_destroy(peer->dtls.engine);
    peer->dtls.engine = NULL;
    dtls_session_unlock(peer);
}

int dtls_session_is_ready(turbo_dc_peer_t *peer) {
    dtls_session_lock(peer);
    int ready = !peer->dtls.stopped && turbo_gdtls_ready(peer->dtls.engine);
    dtls_session_unlock(peer);
    return ready;
}

static void dtls_send_queued(turbo_dc_peer_t *peer, int closing) {
    for (;;) {
        dtls_session_lock(peer);
        tstr packet = peer->dtls.stopped && !closing
            ? NULL : turbo_gdtls_take_datagram(peer->dtls.engine);
        dtls_session_unlock(peer);
        if (!packet) return;
        dc_send_transport_data(peer, packet, tstr_len(packet));
        tstr_free(packet);
    }
}

void dtls_send_output(turbo_dc_peer_t *peer) {
    dtls_send_queued(peer, 0);
}

static void dtls_drain_application_data(turbo_dc_peer_t *peer) {
    for (;;) {
        dtls_session_lock(peer);
        /* Clean peer close may leave authenticated tail records. Engine
         * failure and explicit local shutdown leave no drainable payload. */
        tstr plaintext = peer->dtls.application_ready
            ? turbo_gdtls_take_plaintext(peer->dtls.engine) : NULL;
        dtls_session_unlock(peer);
        if (!plaintext) return;
        if (!peer->ctx->disable_sctp) {
            usrsctp_conninput(peer, plaintext, tstr_len(plaintext), 0);
            sctp_poll_status(peer, "post-conninput");
        }
        cmeta_crypto_clear(plaintext, tstr_len(plaintext));
        tstr_free(plaintext);
    }
}

/* Exactly one transaction elects the completion owner under admission. */
static int dtls_elect_completion(turbo_dc_peer_t *peer) {
    if (peer->dtls.handshake_done || peer->dtls.stopped ||
        !turbo_gdtls_ready(peer->dtls.engine)) return 0;
    peer->dtls.handshake_done = 1;
    return 1;
}

static void dtls_complete(turbo_dc_peer_t *peer) {
    TLOG_INFO("DTLS handshake completed");
    if (!peer->ctx->disable_sctp && sctp_start_association(peer) != 0) {
        dtls_session_shutdown(peer);
        dc_fail_peer(peer, TURBO_DC_ERROR_SCTP_CONNECT, "SCTP association failed");
        return;
    }
    dtls_session_lock(peer);
    int active = !peer->dtls.stopped;
    if (active) peer->dtls.application_ready = 1;
    dtls_session_unlock(peer);
    if (active && peer->ctx->disable_sctp) dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
}

void dtls_session_poll_timeout(turbo_dc_peer_t *peer) {
    int result = 0;
    dtls_session_lock(peer);
    if (peer->dtls.timer_active && !peer->dtls.stopped) {
        uint64_t now = cmeta_monotonic_ms();
        uint64_t deadline = turbo_gdtls_deadline(peer->dtls.engine);
        if (deadline && now >= deadline) result = turbo_gdtls_poll(peer->dtls.engine, now);
        dtls_commit_result(peer, result);
    }
    dtls_session_unlock(peer);
    dtls_report_error(peer, result);
    dtls_send_output(peer);
}

int dtls_write_application_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    if (!data || !len || len > 16384) return -1;
    int result = TURBO_GDTLS_CLOSED;
    int attempted = 0;
    dtls_session_lock(peer);
    if (!peer->dtls.stopped && turbo_gdtls_ready(peer->dtls.engine)) {
        attempted = 1;
        result = turbo_gdtls_write(peer->dtls.engine, data, len);
        dtls_commit_result(peer, result);
    }
    dtls_session_unlock(peer);
    if (attempted) dtls_report_error(peer, result);
    if (!result) dtls_send_output(peer);
    return result ? -1 : (int)len;
}

void dtls_process_handshake(turbo_dc_peer_t *peer) {
    dtls_session_lock(peer);
    if (peer->dtls.stopped || peer->dtls.handshake_started) {
        dtls_session_unlock(peer);
        return;
    }
    int result = dtls_start_locked(peer);
    dtls_commit_result(peer, result);
    dtls_session_unlock(peer);
    dtls_report_error(peer, result);
    dtls_send_output(peer);
}

void dtls_handle_incoming(turbo_dc_peer_t *peer, const void *data, size_t len) {
    dtls_session_lock(peer);
    if (peer->dtls.stopped) {
        dtls_session_unlock(peer);
        return;
    }
    int result = dtls_start_locked(peer);
    if (!result) result = turbo_gdtls_receive(peer->dtls.engine, data, len, cmeta_monotonic_ms());
    dtls_commit_result(peer, result);
    int complete = dtls_elect_completion(peer);
    dtls_session_unlock(peer);
    if (complete) dtls_complete(peer);
    dtls_send_output(peer);
    dtls_drain_application_data(peer);
    /* Clean close retains the closing alert behind any admitted output.
     * Drain the complete queue before CLOSED; failures suppress all output. */
    if (result == TURBO_GDTLS_CLOSED) dtls_send_queued(peer, 1);
    dtls_report_error(peer, result);
}
