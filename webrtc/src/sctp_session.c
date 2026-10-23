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
#include <limits.h>
#include <stdlib.h>
#include "tlog.h"

/* ============================================================================
 * Global SCTP State (thread-safe)
 * ============================================================================ */

static atomic_int g_context_count = 0;
static cmeta_once_t g_sctp_once = SALTS_ONCE_INIT;
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
        TLOG_WARNF("SCTP status poll failed reason='{}' errno={}",
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
    cmeta_once(&g_sctp_once, sctp_global_init_once);
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

typedef struct sctp_outbound_owner_command_s {
    turbo_dc_peer_t *peer;
    uint8_t *packet;
    size_t length;
    int result;
} sctp_outbound_owner_command_t;

static void sctp_outbound_owner_task(void *arg1, void *arg2) {
    sctp_outbound_owner_command_t *command =
        (sctp_outbound_owner_command_t *)arg1;
    turbo_dc_peer_t *peer;
    int written;
    (void)arg2;

    if (!command || !command->peer || !command->packet ||
        command->length == 0 || command->length > INT_MAX) {
        return;
    }
    peer = command->peer;
    command->result = -1;
    if (!peer->dtls.ssl || !peer->dtls.handshake_done) {
        return;
    }

    written = SSL_write(peer->dtls.ssl, command->packet, (int)command->length);
    if (written != (int)command->length) {
        TLOG_ERRORF("SCTP outbound SSL_write failed bytes={} ret={} errno={}",
                    command->length, written, errno);
        return;
    }

    dtls_send_output(peer);
    command->result = 0;
}

int sctp_outbound_packet_cb(void *addr, void *data, size_t length,
                            uint8_t tos, uint8_t set_df) {
    (void)tos;
    (void)set_df;

    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)addr;
    sctp_outbound_owner_command_t command;
    int dispatch_status;

    if (!peer || !data || length == 0 || length > UINT16_MAX ||
        dc_peer_acquire(peer) != 0) {
        return -1;
    }

    /* usrsctp may emit packets from its timer pump as well as sendv(). Copy
     * callback-owned bytes before synchronously dispatching SSL/BIO work to
     * the Peer owner. The packet is bounded to one SCTP wire packet. */
    command.packet = (uint8_t *)malloc(length);
    if (!command.packet) {
        dc_peer_release(peer);
        return -1;
    }
    memcpy(command.packet, data, length);
    command.peer = peer;
    command.length = length;
    command.result = -1;

    dispatch_status = dc_peer_post_owner_sync(
        peer, sctp_outbound_owner_task, &command, NULL);
    free(command.packet);
    dc_peer_release(peer);
    return dispatch_status == SALTS_OK ? command.result : -1;
}

typedef enum sctp_reassembly_result_e {
    SCTP_REASSEMBLY_PENDING = 0,
    SCTP_REASSEMBLY_READY = 1,
    SCTP_REASSEMBLY_REJECTED = -1
} sctp_reassembly_result_t;

static void sctp_reassembly_clear_slot(sctp_reassembly_slot_t *slot) {
    if (!slot) return;
    free(slot->data);
    memset(slot, 0, sizeof(*slot));
}

static void sctp_reassembly_clear_all_locked(turbo_dc_peer_t *peer) {
    size_t i;
    for (i = 0; i < SCTP_REASSEMBLY_SLOTS; ++i)
        sctp_reassembly_clear_slot(&peer->sctp.reassembly[i]);
    peer->sctp.reassembly_blocked = 0;
}

static void sctp_reassembly_clear_all(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) return;
    cmeta_mutex_lock(&peer->operation_mutex);
    sctp_reassembly_clear_all_locked(peer);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static void sctp_reassembly_clear_identity(turbo_dc_peer_t *peer,
                                           sctp_assoc_t association_id,
                                           uint16_t stream_id,
                                           uint16_t stream_sequence) {
    size_t i;
    cmeta_mutex_lock(&peer->operation_mutex);
    for (i = 0; i < SCTP_REASSEMBLY_SLOTS; ++i) {
        sctp_reassembly_slot_t *slot = &peer->sctp.reassembly[i];
        if (slot->used && slot->association_id == association_id &&
            slot->stream_id == stream_id &&
            slot->stream_sequence == stream_sequence)
            sctp_reassembly_clear_slot(slot);
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static void sctp_reassembly_clear_stream(turbo_dc_peer_t *peer,
                                         sctp_assoc_t association_id,
                                         uint16_t stream_id) {
    size_t i;
    cmeta_mutex_lock(&peer->operation_mutex);
    for (i = 0; i < SCTP_REASSEMBLY_SLOTS; ++i) {
        sctp_reassembly_slot_t *slot = &peer->sctp.reassembly[i];
        if (slot->used && slot->association_id == association_id &&
            slot->stream_id == stream_id)
            sctp_reassembly_clear_slot(slot);
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static sctp_reassembly_result_t sctp_reassemble_message(
    turbo_dc_peer_t *peer, const struct sctp_rcvinfo *rcv, int flags,
    const uint8_t *bytes, size_t length, uint8_t **assembled,
    size_t *assembled_length, const char **detail) {
    sctp_reassembly_slot_t *slot = NULL;
    sctp_reassembly_slot_t *free_slot = NULL;
    const uint32_t ppid = ntohl(rcv->rcv_ppid);
    const uint8_t unordered =
        (rcv->rcv_flags & SCTP_UNORDERED) != 0u ? 1u : 0u;
    size_t i;

    *assembled = NULL;
    *assembled_length = 0u;
    *detail = NULL;
    cmeta_mutex_lock(&peer->operation_mutex);
    if (peer->sctp.reassembly_blocked) {
        cmeta_mutex_unlock(&peer->operation_mutex);
        *detail = "SCTP receive reassembly is blocked until association reset";
        return SCTP_REASSEMBLY_REJECTED;
    }

    for (i = 0; i < SCTP_REASSEMBLY_SLOTS; ++i) {
        sctp_reassembly_slot_t *candidate = &peer->sctp.reassembly[i];
        if (!candidate->used) {
            if (!free_slot) free_slot = candidate;
            continue;
        }
        if (candidate->association_id == rcv->rcv_assoc_id &&
            candidate->stream_id == rcv->rcv_sid &&
            candidate->stream_sequence == rcv->rcv_ssn &&
            candidate->unordered == unordered) {
            slot = candidate;
            break;
        }
    }

    if (!bytes || length == 0u || length > SCTP_REASSEMBLY_MESSAGE_MAX) {
        if (slot) {
            free(slot->data);
            slot->data = NULL;
            slot->length = 0u;
            slot->discarding = 1u;
            if (flags & MSG_EOR) sctp_reassembly_clear_slot(slot);
        } else if ((flags & MSG_EOR) == 0) {
            if (!free_slot) {
                sctp_reassembly_clear_all_locked(peer);
                peer->sctp.reassembly_blocked = 1;
            } else {
                free_slot->stream_id = rcv->rcv_sid;
                free_slot->stream_sequence = rcv->rcv_ssn;
                free_slot->ppid = ppid;
                free_slot->association_id = rcv->rcv_assoc_id;
                free_slot->unordered = unordered;
                free_slot->discarding = 1u;
                free_slot->used = 1u;
            }
        }
        cmeta_mutex_unlock(&peer->operation_mutex);
        *detail = "invalid or oversized SCTP receive fragment";
        return SCTP_REASSEMBLY_REJECTED;
    }

    if (slot && slot->ppid != ppid) {
        sctp_reassembly_clear_slot(slot);
        if ((flags & MSG_EOR) == 0)
            peer->sctp.reassembly_blocked = 1;
        cmeta_mutex_unlock(&peer->operation_mutex);
        *detail = "SCTP fragment metadata changed before EOR";
        return SCTP_REASSEMBLY_REJECTED;
    }
    if (slot && slot->discarding) {
        if (flags & MSG_EOR) sctp_reassembly_clear_slot(slot);
        cmeta_mutex_unlock(&peer->operation_mutex);
        *detail = "discarding rejected SCTP partial delivery until EOR";
        return SCTP_REASSEMBLY_REJECTED;
    }
    if (!slot && (flags & MSG_EOR)) {
        cmeta_mutex_unlock(&peer->operation_mutex);
        *assembled_length = length;
        return SCTP_REASSEMBLY_READY;
    }
    if (!slot) {
        if (!free_slot) {
            sctp_reassembly_clear_all_locked(peer);
            peer->sctp.reassembly_blocked = 1;
            cmeta_mutex_unlock(&peer->operation_mutex);
            *detail = "too many simultaneous SCTP partial deliveries";
            return SCTP_REASSEMBLY_REJECTED;
        }
        free_slot->data = (uint8_t *)malloc(SCTP_REASSEMBLY_MESSAGE_MAX);
        free_slot->length = 0u;
        free_slot->stream_id = rcv->rcv_sid;
        free_slot->stream_sequence = rcv->rcv_ssn;
        free_slot->ppid = ppid;
        free_slot->association_id = rcv->rcv_assoc_id;
        free_slot->unordered = unordered;
        free_slot->used = 1u;
        slot = free_slot;
        if (!slot->data) {
            slot->discarding = 1u;
            cmeta_mutex_unlock(&peer->operation_mutex);
            *detail = "SCTP receive reassembly allocation failed";
            return SCTP_REASSEMBLY_REJECTED;
        }
    }
    if (length > SCTP_REASSEMBLY_MESSAGE_MAX - slot->length) {
        free(slot->data);
        slot->data = NULL;
        slot->length = 0u;
        slot->discarding = 1u;
        if (flags & MSG_EOR) sctp_reassembly_clear_slot(slot);
        cmeta_mutex_unlock(&peer->operation_mutex);
        *detail = "SCTP message exceeds the 24 KiB receive limit";
        return SCTP_REASSEMBLY_REJECTED;
    }

    memcpy(slot->data + slot->length, bytes, length);
    slot->length += length;
    if ((flags & MSG_EOR) == 0) {
        cmeta_mutex_unlock(&peer->operation_mutex);
        return SCTP_REASSEMBLY_PENDING;
    }

    *assembled = slot->data;
    *assembled_length = slot->length;
    slot->data = NULL;
    memset(slot, 0, sizeof(*slot));
    cmeta_mutex_unlock(&peer->operation_mutex);
    return SCTP_REASSEMBLY_READY;
}

static void sctp_handle_partial_delivery_event(
    turbo_dc_peer_t *peer, const void *data, size_t datalen) {
    struct sctp_pdapi_event event;
    if (datalen < sizeof(event)) return;
    memcpy(&event, data, sizeof(event));
    if (event.pdapi_length < sizeof(event) ||
        event.pdapi_length > datalen ||
        event.pdapi_indication != SCTP_PARTIAL_DELIVERY_ABORTED)
        return;
    sctp_reassembly_clear_identity(
        peer, event.pdapi_assoc_id, (uint16_t)event.pdapi_stream,
        (uint16_t)event.pdapi_seq);
    dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_RECEIVE,
                      "SCTP partial delivery aborted");
}

static void sctp_handle_stream_reset_event(
    turbo_dc_peer_t *peer, const void *data, size_t datalen) {
    struct sctp_stream_reset_event event;
    size_t count;
    size_t i;
    if (datalen < sizeof(event)) return;
    memcpy(&event, data, sizeof(event));
    if (event.strreset_length < sizeof(event) ||
        event.strreset_length > datalen ||
        (event.strreset_length - sizeof(event)) % sizeof(uint16_t) != 0u ||
        (event.strreset_flags &
         (SCTP_STREAM_RESET_DENIED | SCTP_STREAM_RESET_FAILED)) != 0u ||
        (event.strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) == 0u)
        return;
    count = (event.strreset_length - sizeof(event)) / sizeof(uint16_t);
    if (count == 0u) {
        /* RFC 6525 section 6.1.1: an empty list identifies every stream. */
        sctp_reassembly_clear_all(peer);
        dc_channel_handle_all_streams_reset(
            peer,
            (event.strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN) == 0u);
        return;
    }
    for (i = 0; i < count; ++i) {
        uint16_t stream_id;
        memcpy(&stream_id,
               (const uint8_t *)data + sizeof(event) + i * sizeof(stream_id),
               sizeof(stream_id));
        sctp_reassembly_clear_stream(peer, event.strreset_assoc_id, stream_id);
        /* RFC 8831 section 6.7: answer a peer's incoming reset with an
         * outgoing reset. If both directions already completed, only retire
         * the application channel. */
        dc_channel_handle_stream_reset(
            peer, stream_id,
            (event.strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN) == 0u);
    }
}

int sctp_inbound_packet_cb(struct socket *sock, union sctp_sockstore addr,
                           void *data, size_t datalen,
                           struct sctp_rcvinfo rcv, int flags, void *ulp_info) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)ulp_info;
    uint8_t *assembled = NULL;
    const uint8_t *message_data;
    size_t message_length;
    size_t logical_length;
    const char *reassembly_detail = NULL;
    uint16_t stream_id;
    uint32_t ppid;
    int is_binary = 0;
    sctp_reassembly_result_t reassembly;

    (void)sock;
    (void)addr;
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
        if (notif->sn_header.sn_type == SCTP_ASSOC_CHANGE &&
            datalen >= sizeof(struct sctp_assoc_change)) {
            const struct sctp_assoc_change *assoc = &notif->sn_assoc_change;
            TLOG_INFOF("SCTP assoc change state={} out={} in={} error={}",
                      sctp_assoc_state_name(assoc->sac_state),
                      assoc->sac_outbound_streams,
                      assoc->sac_inbound_streams,
                      assoc->sac_error);
            if (assoc->sac_state == SCTP_COMM_UP) {
                peer->sctp.connected = 1;
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
            } else if (assoc->sac_state == SCTP_RESTART) {
                sctp_reassembly_clear_all(peer);
            } else if (assoc->sac_state == SCTP_COMM_LOST ||
                       assoc->sac_state == SCTP_SHUTDOWN_COMP ||
                       assoc->sac_state == SCTP_CANT_STR_ASSOC) {
                sctp_reassembly_clear_all(peer);
                peer->sctp.connected = 0;
            }
        } else if (notif->sn_header.sn_type ==
                   SCTP_PARTIAL_DELIVERY_EVENT) {
            sctp_handle_partial_delivery_event(peer, data, datalen);
        } else if (notif->sn_header.sn_type == SCTP_STREAM_RESET_EVENT) {
            sctp_handle_stream_reset_event(peer, data, datalen);
        }

        free(data);
        dc_peer_release(peer);
        return 1;
    }

    reassembly = sctp_reassemble_message(
        peer, &rcv, flags, (const uint8_t *)data, datalen,
        &assembled, &message_length, &reassembly_detail);
    if (reassembly == SCTP_REASSEMBLY_PENDING) {
        free(data);
        dc_peer_release(peer);
        return 1;
    }
    if (reassembly == SCTP_REASSEMBLY_REJECTED) {
        int blocked;
        cmeta_mutex_lock(&peer->operation_mutex);
        blocked = peer->sctp.reassembly_blocked;
        cmeta_mutex_unlock(&peer->operation_mutex);
        if (blocked)
            dc_fail_peer(peer, TURBO_DC_ERROR_SCTP_RECEIVE,
                         reassembly_detail);
        else
            dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_RECEIVE,
                              reassembly_detail);
        free(data);
        dc_peer_release(peer);
        return 1;
    }

    message_data = assembled ? assembled : (const uint8_t *)data;
    stream_id = rcv.rcv_sid;
    ppid = ntohl(rcv.rcv_ppid);
    logical_length = message_length;
    if (ppid == SCTP_PPID_DCEP) {
        dcep_handle_message(peer, stream_id, message_data, message_length);
        free(assembled);
        free(data);
        dc_peer_release(peer);
        return 1;
    }

    if (ppid == SCTP_PPID_STRING || ppid == SCTP_PPID_BINARY) {
        if (message_length == 0u) {
            dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_RECEIVE,
                              "nonempty PPID carried zero octets");
            free(assembled);
            free(data);
            dc_peer_release(peer);
            return 1;
        }
        is_binary = ppid == SCTP_PPID_BINARY;
    } else if (ppid == SCTP_PPID_STRING_EMPTY ||
               ppid == SCTP_PPID_BINARY_EMPTY) {
        if (message_length != 1u || message_data[0] != 0u) {
            dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_RECEIVE,
                              "empty PPID requires one zero octet");
            free(assembled);
            free(data);
            dc_peer_release(peer);
            return 1;
        }
        logical_length = 0u;
        is_binary = ppid == SCTP_PPID_BINARY_EMPTY;
    } else {
        /* PPID 52/54 fragmentation and unknown PPIDs are not WebRTC user
         * messages. SCTP DATA reassembly owns fragmentation below this layer. */
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_RECEIVE,
                          "unsupported WebRTC PPID");
        free(assembled);
        free(data);
        dc_peer_release(peer);
        return 1;
    }

    {
        turbo_dc_channel_t *channel = dc_channel_admit(peer, stream_id);
        if (channel) {
            turbo_dc_message_cb callback = NULL;
            void *callback_data;
            cmeta_mutex_lock(&peer->operation_mutex);
            if (!channel->closing && channel->is_open && !peer->destroying)
                callback = channel->on_message;
            callback_data = channel->user_data;
            cmeta_mutex_unlock(&peer->operation_mutex);
            if (callback)
                callback(channel, message_data, logical_length, is_binary,
                         callback_data);
            dc_channel_release(channel);
        }
    }

    free(assembled);
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
        SCTP_PARTIAL_DELIVERY_EVENT,
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

void sctp_session_cleanup(turbo_dc_peer_t *peer) {
    if (!peer) return;
    sctp_reassembly_clear_all(peer);
    peer->sctp.connected = 0;
}

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
        TLOG_ERRORF("SCTP bind failed errno={}", errno);
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_BIND, NULL);
        return -1;
    }

    int ret = usrsctp_connect(peer->sctp.socket, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        TLOG_ERRORF("SCTP connect failed ret={} errno={}", ret, errno);
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_CONNECT, NULL);
        return -1;
    }

    peer->sctp.connected = 0;
    /* Some usrsctp builds are unstable if SCTP_STATUS is queried immediately
     * after connect() while the association is still being instantiated.
     * Let notifications or later explicit polls observe readiness instead. */
    return 0;
}
