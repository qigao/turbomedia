/**
 * sctp_session.c - SCTP session management wrapper
 *
 * Wraps usrsctp for WebRTC DataChannel usage.
 * Handles SCTP initialization, socket configuration, and association management.
 */

#include "turbo_datachannel_internal.h"
#include <platform.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include "tlog.h"

/* ============================================================================
 * Global SCTP State (thread-safe)
 * ============================================================================ */

static atomic_int g_context_count = 0;
static turbo_once_t g_sctp_once = TURBO_ONCE_INIT;
static int g_sctp_init_result = -1;

static void sctp_global_init_once(void) {
    usrsctp_init_nothreads(0, sctp_outbound_packet_cb, NULL);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    usrsctp_sysctl_set_sctp_nr_outgoing_streams_default(SCTP_MAX_STREAMS);
    g_sctp_init_result = 0;
}

static const char *sctp_assoc_state_name(uint16_t state) {
    switch (state) {
        case SCTP_COMM_UP:
            return "COMM_UP";
        case SCTP_COMM_LOST:
            return "COMM_LOST";
        case SCTP_RESTART:
            return "RESTART";
        case SCTP_SHUTDOWN_COMP:
            return "SHUTDOWN_COMP";
        case SCTP_CANT_STR_ASSOC:
            return "CANT_START_ASSOC";
        default:
            return "UNKNOWN";
    }
}

void sctp_poll_status(turbo_dc_peer_t *peer, const char *reason) {
    struct sctp_status status;
    socklen_t status_len = sizeof(status);

    if (!peer || !peer->sctp.socket || !peer->dtls.handshake_done) {
        return;
    }

    memset(&status, 0, sizeof(status));
    if (usrsctp_getsockopt(peer->sctp.socket, IPPROTO_SCTP, SCTP_STATUS,
                           &status, &status_len) != 0) {
        TLOG_WARN("SCTP status poll failed reason='{}' errno={}",
                  reason ? reason : "", errno);
        return;
    }

    /* Some usrsctp builds keep SCTP_STATUS at COOKIE_ECHOED while the stream
     * negotiation is already complete and the socket is usable for DCEP/data.
     * Promote the peer once the association is established or the socket has
     * negotiated both stream directions. */
    if (!peer->sctp.connected &&
        (status.sstat_state == SCTP_ESTABLISHED ||
         (status.sstat_state == SCTP_COOKIE_ECHOED &&
          status.sstat_outstrms > 0 &&
          status.sstat_instrms > 0))) {
        peer->sctp.connected = 1;
        dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
    }
}

int sctp_global_init(void) {
    atomic_fetch_add_explicit(&g_context_count, 1, memory_order_relaxed);
    turbo_once(&g_sctp_once, sctp_global_init_once);
    if (g_sctp_init_result != 0) {
        atomic_fetch_sub_explicit(&g_context_count, 1, memory_order_relaxed);
        return -1;
    }
    return 0;
}

void sctp_global_cleanup(void) {
    atomic_fetch_sub_explicit(&g_context_count, 1, memory_order_relaxed);
    /*
     * Note: We intentionally do NOT call usrsctp_finish() here.
     *
     * usrsctp is process-global and calling finish() causes issues:
     * 1. Race condition: another thread may be initializing simultaneously
     * 2. Re-initialization issues: usrsctp may not work after finish/re-init
     * 3. Pending callbacks: finish() blocks until all callbacks complete
     *
     * Most WebRTC implementations leave usrsctp running for process lifetime.
     * Memory is reclaimed on process exit.
     */
}

/* ============================================================================
 * SCTP Callbacks (called by usrsctp)
 * ============================================================================ */

int sctp_outbound_packet_cb(void *addr, void *data, size_t length, uint8_t tos, uint8_t set_df) {
    (void)tos;
    (void)set_df;

    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)addr;
    if (!peer || dc_peer_acquire(peer) != 0) {
        return -1;
    }
    if (!peer->dtls.handshake_done) {
        TLOG_WARN("SCTP outbound dropped before DTLS handshake: bytes={}",
                  length);
        dc_peer_release(peer);
        return -1;
    }

    int written = SSL_write(peer->dtls.ssl, data, (int)length);
    if (written <= 0) {
        TLOG_ERROR("SCTP outbound SSL_write failed bytes={} ret={} errno={}",
                   length, written, errno);
        dc_peer_release(peer);
        return -1;
    }

    dtls_send_output(peer);
    dc_peer_release(peer);
    return 0;
}

int sctp_inbound_packet_cb(struct socket *sock, union sctp_sockstore addr,
                           void *data, size_t datalen,
                           struct sctp_rcvinfo rcv, int flags, void *ulp_info) {
    (void)sock;
    (void)addr;

    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)ulp_info;
    if (!peer || !data || dc_peer_acquire(peer) != 0) {
        if (data) free(data);
        return 1;
    }

    if (flags & MSG_NOTIFICATION) {
        union sctp_notification notif_storage;
        union sctp_notification *notif;

        memset(&notif_storage, 0, sizeof(notif_storage));
        memcpy(&notif_storage, data,
               datalen < sizeof(notif_storage) ? datalen : sizeof(notif_storage));
        notif = &notif_storage;
        if (notif->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
            const struct sctp_assoc_change *assoc = &notif->sn_assoc_change;
            TLOG_INFO("SCTP assoc change state={} out={} in={} error={}",
                      sctp_assoc_state_name(assoc->sac_state),
                      assoc->sac_outbound_streams,
                      assoc->sac_inbound_streams,
                      assoc->sac_error);
            if (assoc->sac_state == SCTP_COMM_UP) {
                peer->sctp.connected = 1;
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
            }
        }

        free(data);
        dc_peer_release(peer);
        return 1;
    }

    uint16_t stream_id = rcv.rcv_sid;
    uint32_t ppid = ntohl(rcv.rcv_ppid);
    if (ppid == SCTP_PPID_DCEP) {
        dcep_handle_message(peer, stream_id, (const uint8_t *)data, datalen);
        free(data);
        dc_peer_release(peer);
        return 1;
    }

    turbo_dc_channel_t *channel = NULL;
    channel = peer_get_channel(peer, stream_id);

    if (channel && channel->on_message && channel->is_open) {
        int is_binary = (ppid == SCTP_PPID_BINARY || ppid == SCTP_PPID_BINARY_EMPTY);
        channel->on_message(channel, data, datalen, is_binary, channel->user_data);
    }

    free(data);
    dc_peer_release(peer);
    return 1;
}

/* ============================================================================
 * SCTP Socket Configuration
 * ============================================================================ */

static int configure_sctp_socket(struct socket *sock) {
    struct linger linger_opt = { .l_onoff = 1, .l_linger = 0 };
    if (usrsctp_setsockopt(sock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt)) != 0) {
        return -1;
    }

    {
        struct sctp_assoc_value stream_rst;
        memset(&stream_rst, 0, sizeof(stream_rst));
        stream_rst.assoc_id = SCTP_ALL_ASSOC;
        stream_rst.assoc_value = 1;
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET,
                               &stream_rst, sizeof(stream_rst)) != 0) {
            return -1;
        }
    }

    uint32_t nodelay = 1;
    if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
        return -1;
    }

    uint32_t recvrcvinfo = 1;
    if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &recvrcvinfo,
                           sizeof(recvrcvinfo)) != 0) {
        return -1;
    }

    struct sctp_event event;
    memset(&event, 0, sizeof(event));
    event.se_assoc_id = SCTP_FUTURE_ASSOC;
    event.se_on = 1;

    uint16_t event_types[] = {
        SCTP_ASSOC_CHANGE,
        SCTP_PEER_ADDR_CHANGE,
        SCTP_SEND_FAILED_EVENT,
        SCTP_SENDER_DRY_EVENT,
        SCTP_STREAM_RESET_EVENT
    };

    for (size_t i = 0; i < sizeof(event_types) / sizeof(event_types[0]); i++) {
        event.se_type = event_types[i];
        usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event));
    }

    {
        struct sctp_paddrparams params;
        memset(&params, 0, sizeof(params));
        params.spp_assoc_id = 0;
        params.spp_flags = SPP_PMTUD_DISABLE;
        params.spp_pathmtu = SCTP_MTU_DEFAULT;
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS,
                               &params, sizeof(params)) != 0) {
            return -1;
        }
    }

    return 0;
}

int sctp_reset_channel_stream(turbo_dc_peer_t *peer, uint16_t stream_id) {
    uint8_t reset_buffer[sizeof(struct sctp_reset_streams) + sizeof(uint16_t)];
    struct sctp_reset_streams *reset_request =
        (struct sctp_reset_streams *)reset_buffer;

    if (!peer || !peer->sctp.socket) {
        return -1;
    }

    memset(reset_buffer, 0, sizeof(reset_buffer));
    reset_request->srs_assoc_id = SCTP_ALL_ASSOC;
    reset_request->srs_flags = SCTP_STREAM_RESET_OUTGOING;
    reset_request->srs_number_streams = 1;
    reset_request->srs_stream_list[0] = stream_id;

    return usrsctp_setsockopt(peer->sctp.socket, IPPROTO_SCTP, SCTP_RESET_STREAMS,
                              reset_buffer, sizeof(reset_buffer));
}

/* ============================================================================
 * SCTP Session API
 * ============================================================================ */

int sctp_session_init(turbo_dc_peer_t *peer) {
    struct socket *sock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP,
                                         sctp_inbound_packet_cb, NULL, 0, peer);
    if (!sock) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_SOCKET, NULL);
        return -1;
    }

    usrsctp_set_non_blocking(sock, 1);
    usrsctp_register_address(peer);
    peer->sctp_address_registered = 1;

    if (configure_sctp_socket(sock) != 0) {
        if (peer->sctp_address_registered) {
            usrsctp_deregister_address(peer);
            peer->sctp_address_registered = 0;
        }
        usrsctp_close(sock);
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_CONFIG, NULL);
        return -1;
    }

    peer->sctp.socket = sock;
    return 0;
}

int sctp_start_association(turbo_dc_peer_t *peer) {
    struct sockaddr_conn local_addr, remote_addr;

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sconn_family = AF_CONN;
    local_addr.sconn_port = htons(SCTP_ASSOCIATION_PORT);
    local_addr.sconn_addr = peer;
#ifdef HAVE_SCONN_LEN
    local_addr.sconn_len = sizeof(local_addr);
#endif

    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sconn_family = AF_CONN;
    remote_addr.sconn_port = htons(SCTP_ASSOCIATION_PORT);
    remote_addr.sconn_addr = peer;
#ifdef HAVE_SCONN_LEN
    remote_addr.sconn_len = sizeof(remote_addr);
#endif

    if (usrsctp_bind(peer->sctp.socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
        TLOG_ERROR("SCTP bind failed errno={}", errno);
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_BIND, NULL);
        return -1;
    }

    int ret = usrsctp_connect(peer->sctp.socket, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        TLOG_ERROR("SCTP connect failed ret={} errno={}", ret, errno);
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_CONNECT, NULL);
        return -1;
    }

    peer->sctp.connected = 0;
    /* Some usrsctp builds are unstable if SCTP_STATUS is queried immediately
     * after connect() while the association is still being instantiated.
     * Let notifications or later explicit polls observe readiness instead. */
    return 0;
}
