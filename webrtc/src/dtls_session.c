/* Salts DTLS-SRTP on the existing DataChannel transport owner. */
#include "turbo_datachannel_internal.h"
#include <core/clock.h>
#include <crypto/status.h>
#include <stdatomic.h>
#include <string.h>

enum { DTLS_QUEUE_CAPACITY = 64, DTLS_RECORD_HEADER = 13 };

struct dc_dtls_packet_s {
    dc_dtls_packet_t *next;
    cnet_datagram_peer address;
    size_t size;
    uint8_t bytes[];
};

struct dc_dtls_flow_s {
    crypto_dtls_stream *stream;
    crypto_dtls_listener *listener;
    atomic_uint references;
    atomic_int terminal;
    int sending, stream_send;
    uint8_t peer_sha256[CRYPTO_DTLS_FINGERPRINT_BYTES];
    uint8_t application[CRYPTO_DTLS_MAX_DATAGRAM_BYTES];
    size_t application_size;
};

static int enqueue(dc_dtls_packet_t **head, dc_dtls_packet_t **tail,
                   size_t *count, const void *bytes, size_t size,
                   const cnet_datagram_peer *address) {
    if (*count == DTLS_QUEUE_CAPACITY) return -1;
    dc_dtls_packet_t *packet = malloc(sizeof(*packet) + size);
    if (!packet) return -1;
    packet->next = NULL;
    packet->address = address ? *address : (cnet_datagram_peer){0};
    packet->size = size;
    memcpy(packet->bytes, bytes, size);
    if (*tail) (*tail)->next = packet;
    else *head = packet;
    *tail = packet;
    ++*count;
    return 0;
}

static void pop(dc_dtls_packet_t **head, dc_dtls_packet_t **tail, size_t *count) {
    dc_dtls_packet_t *packet = *head;
    *head = packet->next;
    if (!*head) *tail = NULL;
    --*count;
    free(packet);
}

static void flow_release(dc_dtls_flow_t *flow) {
    if (atomic_fetch_sub_explicit(&flow->references, 1, memory_order_acq_rel) != 1)
        return;
    if (flow->stream) {
        (void)crypto_dtls_stream_abort(flow->stream);
        if (flow->sending && flow->stream_send) {
            (void)crypto_dtls_stream_sent(flow->stream, monotonic_ms(),
                                         atomic_load(&flow->terminal) > 0);
        }
        (void)crypto_dtls_stream_destroy(flow->stream);
    }
    (void)crypto_dtls_listener_destroy(flow->listener);
    dc_secure_clear(flow->application, sizeof(flow->application));
    free(flow);
}

void dtls_transport_done(void *operation, int status) {
    dc_dtls_flow_t *flow = operation;
    atomic_store_explicit(&flow->terminal, status == 0 ? 1 : -1, memory_order_release);
    flow_release(flow);
}

static void submit(turbo_dc_peer_t *peer, const uint8_t *wire, size_t size,
                   int stream_send) {
    dc_dtls_flow_t *flow = peer->dtls.flow;
    flow->sending = 1;
    flow->stream_send = stream_send;
    atomic_store(&flow->terminal, 0);
    atomic_fetch_add(&flow->references, 1);
    if (dc_submit_dtls(peer, wire, size, dtls_transport_done, flow) != 0)
        dtls_transport_done(flow, -1);
}

static int app_data(void *user, crypto_dtls_stream *stream,
                    const uint8_t *data, size_t size) {
    dc_dtls_flow_t *flow = user;
    (void)stream;
    if (flow->application_size) return CRYPTO_DTLS_BUSY;
    if (size > sizeof(flow->application)) return SALTS_CRYPTO_EINVAL;
    memcpy(flow->application, data, size);
    flow->application_size = size;
    return 0;
}

static int deliver(turbo_dc_peer_t *peer) {
    dc_dtls_flow_t *flow = peer->dtls.flow;
    if (!flow->application_size) return 0;
#ifdef TURBO_MEDIA_TEST_C11_SCTP_BRIDGE
    if (!peer->test_dtls_application_handler) {
        dc_fail_peer(peer, TURBO_DC_ERROR_SCTP_INIT, "private C11 DTLS bridge handler is not installed");
    } else {
        peer->test_dtls_application_handler(peer, flow->application, flow->application_size,
                                            peer->test_dtls_application_user_data);
    }
#else
    if (!peer->ctx->disable_sctp) {
        usrsctp_conninput(peer, flow->application, flow->application_size, 0);
        sctp_poll_status(peer, "post-conninput");
    }
#endif
    dc_secure_clear(flow->application, flow->application_size);
    flow->application_size = 0;
    return 1;
}

static int fingerprint(const tstr text, uint8_t output[CRYPTO_DTLS_FINGERPRINT_BYTES]) {
    static const char hex[] = "0123456789ABCDEF";
    if (!text || tstr_len(text) != CRYPTO_DTLS_FINGERPRINT_BYTES * 3u - 1u) return -1;
    for (size_t i = 0; i < CRYPTO_DTLS_FINGERPRINT_BYTES; ++i) {
        const char *hi = strchr(hex, text[3 * i]);
        const char *lo = strchr(hex, text[3 * i + 1]);
        if (!hi || !lo || (i && text[3 * i - 1] != ':')) return -1;
        output[i] = (uint8_t)((hi - hex) * 16 + (lo - hex));
    }
    return 0;
}

static void on_retransmit(native_timer_t *timer) {
    turbo_dc_peer_t *peer = native_timer_get_data(timer);
    if (!peer) return;
    sync_mutex_lock(&peer->operation_mutex);
    if (!peer->destroying) peer->dtls_retransmit_pending = 1;
    sync_cond_broadcast(&peer->operation_cond);
    sync_mutex_unlock(&peer->operation_mutex);
}

static int start(turbo_dc_peer_t *peer) {
    dc_dtls_flow_t *flow = peer->dtls.flow;
    if (fingerprint(peer->remote_fingerprint, flow->peer_sha256) != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, "remote fingerprint is required");
        return -1;
    }
    const crypto_tls_credentials credentials = {
        .size = sizeof(credentials), .object = peer->ctx->dtls_identity
    };
    int status;
    if (peer->is_dtls_server)
        status = crypto_dtls_listener_create_srtp(&credentials, CRYPTO_DTLS_SRTP_PROFILES,
                                                 10000u, &flow->listener);
    else {
        status = crypto_dtls_stream_create_srtp(&credentials, flow->peer_sha256, CRYPTO_DTLS_SRTP_PROFILES,
                                              app_data, flow, &flow->stream);
        if (!status) status = crypto_dtls_stream_set_mtu(flow->stream, peer->ctx->dtls_mtu);
    }
    if (status) {
        dc_fail_peer(peer, TURBO_DC_ERROR_SSL_CREATE, "Salts DTLS initialization failed");
        return -1;
    }
    peer->dtls.started = 1;
    return 0;
}

int dtls_session_init(turbo_dc_peer_t *peer) {
    dc_dtls_flow_t *flow = calloc(1, sizeof(*flow));
    if (!flow) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_SSL_CREATE, NULL);
        return -1;
    }
    atomic_init(&flow->references, 1);
    atomic_init(&flow->terminal, 0);
    peer->dtls.flow = flow;
    return 0;
}

int dtls_session_init_timer(turbo_dc_peer_t *peer) {
    if (!peer) return -1;
    if (peer->dtls.retransmit_timer) return 0;
    peer->dtls.retransmit_timer = native_timer_create(NULL);
    if (!peer->dtls.retransmit_timer) return -1;
    native_timer_set_data(peer->dtls.retransmit_timer, peer);
    return 0;
}

void dtls_session_cleanup(turbo_dc_peer_t *peer) {
    dtls_session_t *dtls = &peer->dtls;
    if (dtls->retransmit_timer) native_timer_destroy(dtls->retransmit_timer);
    if (dtls->flow) flow_release(dtls->flow);
    while (dtls->input_head) pop(&dtls->input_head, &dtls->input_tail, &dtls->input_count);
    while (dtls->output_head) pop(&dtls->output_head, &dtls->output_tail, &dtls->output_count);
    free(dtls->tcp_record);
    *dtls = (dtls_session_t){0};
}

void dtls_session_stop(turbo_dc_peer_t *peer) {
    if (peer->dtls.retransmit_timer) native_timer_stop(peer->dtls.retransmit_timer);
    crypto_dtls_stream *stream = dtls_session_stream(peer);
    if (stream) (void)crypto_dtls_stream_abort(stream);
}

crypto_dtls_stream *dtls_session_stream(const turbo_dc_peer_t *peer) {
    return peer->dtls.flow ? peer->dtls.flow->stream : NULL;
}

static int accept_client(turbo_dc_peer_t *peer, const dc_dtls_packet_t *packet, uint64_t now) {
    dc_dtls_flow_t *flow = peer->dtls.flow;
    uint8_t address[23] = {0};
    size_t address_size = 1;
    if (peer->datagram_initialized) {
        /* Encode explicit fields, including the IPv6 scope, for cookie binding. */
        const cnet_datagram_peer *remote = &packet->address;
        address[0] = (uint8_t)remote->family;
        address[1] = (uint8_t)(remote->port >> 8);
        address[2] = (uint8_t)remote->port;
        for (size_t i = 0; i < 4u; ++i)
            address[3 + i] = (uint8_t)(remote->scope_id >> (24u - 8u * i));
        memcpy(address + 7, remote->address, sizeof(remote->address));
        address_size = sizeof(address);
        peer->remote_datagram_peer = *remote;
        peer->has_remote_datagram_peer = 1;
    }
    /* A listener is exclusive to a Peer. TCP and the selected ICE pair
     * already have connection-local transport identity. */
    uint8_t reply[CRYPTO_DTLS_COOKIE_REPLY_BYTES];
    size_t n;
    bool verified;
    int status = crypto_dtls_listener_gate(flow->listener, address, address_size,
        packet->bytes, packet->size, now, reply, sizeof(reply), &n, &verified);
    if (status) return status;
    if (!verified) {
        if (n) submit(peer, reply, n, 0);
        return 0;
    }
    status = crypto_dtls_stream_accept_srtp(flow->listener, address, address_size,
        packet->bytes, packet->size, now, flow->peer_sha256, app_data, flow, &flow->stream);
    return status ? status : crypto_dtls_stream_set_mtu(flow->stream, peer->ctx->dtls_mtu);
}

static void update_state(turbo_dc_peer_t *peer) {
    crypto_dtls_stream_info info;
    crypto_dtls_stream *stream = dtls_session_stream(peer);
    if (!stream || crypto_dtls_stream_get_info(stream, &info) != 0) return;
    if (info.state == CRYPTO_DTLS_READY && !peer->dtls.handshake_done) {
        peer->dtls.handshake_done = 1;
        if (peer->dtls.retransmit_timer) native_timer_stop(peer->dtls.retransmit_timer);
        if (peer->ctx->disable_sctp) dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
        else if (sctp_start_association(peer) != 0) {
            turbo_dc_error_t error = turbo_dc_peer_get_error(peer);
            dc_fail_peer(peer, error.code == TURBO_DC_ERROR_NONE ? TURBO_DC_ERROR_SCTP_CONNECT :
                         error.code, error.detail);
        }
    } else if (info.state == CRYPTO_DTLS_FAILED) {
        dc_fail_peer(peer, info.local_alert == 46 ? TURBO_DC_ERROR_DTLS_FINGERPRINT :
                     TURBO_DC_ERROR_DTLS_HANDSHAKE, "Salts DTLS authentication or handshake failed");
    } else if (info.state == CRYPTO_DTLS_CLOSED) {
        dc_notify_state(peer, TURBO_DC_STATE_CLOSED);
    }
}

static bool same_address(const cnet_datagram_peer *a, const cnet_datagram_peer *b) {
    return a->family == b->family && a->port == b->port && a->scope_id == b->scope_id &&
           memcmp(a->address, b->address, sizeof(a->address)) == 0;
}

void dtls_send_output(turbo_dc_peer_t *peer) {
    dtls_session_t *dtls = &peer->dtls;
    dc_dtls_flow_t *flow = dtls->flow;
    if (!flow || !dtls->started || dtls->pumping) return;
    dtls->pumping = 1;
    /* Bound work, including synchronous legacy transport callbacks. One
     * prepared provider record is retained until its actual send terminal. */
    for (size_t work = 0; work < DTLS_QUEUE_CAPACITY; ++work) {
        const uint64_t now = monotonic_ms();
        if (flow->sending) {
            const int terminal = atomic_load_explicit(&flow->terminal, memory_order_acquire);
            if (!terminal) break;
            flow->sending = 0;
            if (flow->stream_send) (void)crypto_dtls_stream_sent(flow->stream, now, terminal > 0);
            else if (terminal < 0) {
                dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, "DTLS cookie reply send failed");
                break;
            }
        }
        if (peer->state == TURBO_DC_STATE_CLOSED || peer->state == TURBO_DC_STATE_FAILED) break;
        if (flow->stream) {
            const uint8_t *wire;
            size_t n;
            int status = crypto_dtls_stream_next(flow->stream, now, &wire, &n);
            update_state(peer);
            const int delivered = deliver(peer);
            if (status == 0) {
                if (peer->state == TURBO_DC_STATE_DISCONNECTING || peer->state == TURBO_DC_STATE_CLOSED) {
                    (void)crypto_dtls_stream_sent(flow->stream, monotonic_ms(), false);
                    break;
                }
                submit(peer, wire, n, 1);
                continue;
            }
            if (status < 0 || peer->state == TURBO_DC_STATE_FAILED) break;
            if (delivered) continue;
            uint64_t deadline;
            if (!dtls->handshake_done && dtls->retransmit_timer &&
                crypto_dtls_stream_retransmit_deadline(flow->stream, &deadline) == 0) {
                (void)native_timer_start(dtls->retransmit_timer, on_retransmit,
                                         deadline > now ? deadline - now : 1u, 0);
            }
        }

        if (dtls->input_head) {
            dc_dtls_packet_t *packet = dtls->input_head;
            if (flow->stream && peer->datagram_initialized &&
                !same_address(&packet->address, &peer->remote_datagram_peer)) {
                pop(&dtls->input_head, &dtls->input_tail, &dtls->input_count);
                continue;
            }
            int status = flow->stream ?
                crypto_dtls_stream_receive(flow->stream, now, packet->bytes, packet->size) :
                accept_client(peer, packet, now);
            if (status == CRYPTO_DTLS_BUSY) break;
            pop(&dtls->input_head, &dtls->input_tail, &dtls->input_count);
            deliver(peer);
            if (flow->sending) continue;
            if (status < 0 && !flow->stream) {
                dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, "Salts DTLS admission failed");
                break;
            }
            continue;
        }
        if (dtls->output_head && dtls->handshake_done) {
            dc_dtls_packet_t *packet = dtls->output_head;
            int status = crypto_dtls_stream_write(flow->stream, packet->bytes, packet->size);
            if (status == CRYPTO_DTLS_BUSY) break;
            pop(&dtls->output_head, &dtls->output_tail, &dtls->output_count);
            if (status < 0) {
                dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, "Salts DTLS application write failed");
                break;
            }
            continue;
        }
        break;
    }
    dtls->pumping = 0;
}

void dtls_poll_retransmit(turbo_dc_peer_t *peer) {
    sync_mutex_lock(&peer->operation_mutex);
    int retry = peer->dtls_retransmit_pending;
    peer->dtls_retransmit_pending = 0;
    sync_mutex_unlock(&peer->operation_mutex);
    if (retry && peer->dtls.handshake_done) {
        if (peer->dtls.retransmit_timer) native_timer_stop(peer->dtls.retransmit_timer);
        retry = 0;
    }
    dc_dtls_flow_t *flow = peer->dtls.flow;
    if (flow && (retry || (flow->sending && atomic_load(&flow->terminal)) ||
                 (!flow->sending && (peer->dtls.input_head || peer->dtls.output_head)))) {
        dtls_send_output(peer);
    }
}

int dtls_write_application_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    dtls_session_t *dtls = &peer->dtls;
    if (!data || !len || len > peer->ctx->dtls_mtu - DTLS_RECORD_OVERHEAD ||
        !dtls->handshake_done || peer->state == TURBO_DC_STATE_FAILED) return -1;
    if (enqueue(&dtls->output_head, &dtls->output_tail, &dtls->output_count, data, len, NULL) != 0)
        return -1;
    dtls_send_output(peer);
    return 0;
}

#ifdef TURBO_MEDIA_TEST_C11_SCTP_BRIDGE
int dtls_test_write_application_data(turbo_dc_peer_t *peer,
                                     const void *data, size_t len) {
    if (!peer || !dc_peer_is_on_transport_owner(peer)) return -1;
    return dtls_write_application_data(peer, data, len);
}
#endif

void dtls_process_handshake(turbo_dc_peer_t *peer) {
    if (!peer->dtls.started && start(peer) != 0) return;
    dtls_send_output(peer);
}

void dtls_handle_incoming(turbo_dc_peer_t *peer, const void *data, size_t len) {
    dtls_session_t *dtls = &peer->dtls;
    if (!dtls->flow || len > CRYPTO_DTLS_MAX_DATAGRAM_BYTES ||
        peer->state == TURBO_DC_STATE_CLOSED || peer->state == TURBO_DC_STATE_FAILED) return;
    if (!dtls->started && start(peer) != 0) return;
    if (enqueue(&dtls->input_head, &dtls->input_tail, &dtls->input_count,
                data, len, &peer->remote_datagram_peer) != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, "DTLS receive queue exhausted");
        return;
    }
    dtls_send_output(peer);
}

void dtls_handle_tcp(turbo_dc_peer_t *peer, const void *data, size_t size) {
    dtls_session_t *dtls = &peer->dtls;
    const uint8_t *bytes = data;
    if (!dtls->tcp_record) dtls->tcp_record = malloc(CRYPTO_DTLS_MAX_DATAGRAM_BYTES);
    if (!dtls->tcp_record) {
        dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, "DTLS TCP record allocation failed");
        return;
    }
    while (size) {
        size_t target = DTLS_RECORD_HEADER;
        if (dtls->tcp_used >= DTLS_RECORD_HEADER) {
            target += (size_t)dtls->tcp_record[11] * 256u + dtls->tcp_record[12];
            if (target > CRYPTO_DTLS_MAX_DATAGRAM_BYTES) {
                dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, "oversized DTLS TCP record");
                return;
            }
        }
        const size_t missing = target - dtls->tcp_used;
        const size_t take = size < missing ? size : missing;
        memcpy(dtls->tcp_record + dtls->tcp_used, bytes, take);
        dtls->tcp_used += take;
        bytes += take;
        size -= take;
        if (dtls->tcp_used >= DTLS_RECORD_HEADER &&
            dtls->tcp_used == DTLS_RECORD_HEADER + (size_t)dtls->tcp_record[11] * 256u + dtls->tcp_record[12]) {
            dtls_handle_incoming(peer, dtls->tcp_record, dtls->tcp_used);
            dtls->tcp_used = 0;
        }
    }
}
