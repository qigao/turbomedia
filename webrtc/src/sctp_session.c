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
static cmeta_once_t g_sctp_once = SALTS_ONCE_INIT;
static atomic_int g_sctp_initialized = 0;
/* In no-thread mode the native timer queue belongs to the process, not to a
 * peer. Serialize timer dispatch across callers; a synchronous user callback
 * can reenter the public pump without recursively acquiring this mutex. */
static cmeta_mutex_t g_sctp_timer_mutex;
static uint64_t g_sctp_timer_last_ms;
static SALTS_THREAD_LOCAL int g_sctp_timer_dispatching = 0;
/* A foreign destroy uses the same existing process-global timer mutex to
 * retire native callouts before freeing application peer userdata. Track
 * ownership on this thread so callbacks synchronously reentering the public
 * timer pump during usrsctp_close cannot deadlock on their own lock. */
static SALTS_THREAD_LOCAL unsigned g_sctp_timer_retirement_depth = 0;

static void sctp_global_init_once(void) {
    cmeta_mutex_init(&g_sctp_timer_mutex);
    g_sctp_timer_last_ms = cmeta_monotonic_ms();
    usrsctp_init_nothreads(0, sctp_outbound_packet_cb, NULL);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    usrsctp_sysctl_set_sctp_nr_outgoing_streams_default(SCTP_MAX_STREAMS);
    atomic_store_explicit(&g_sctp_initialized, 1, memory_order_release);
}

/* The public no-argument timer pump supplies real elapsed milliseconds to
 * usrsctp_handle_timers(). Passing zero leaves the native tick counter
 * unchanged, so SCTP retransmission and shutdown timers never expire.
 *
 * Each elapsed slice is accounted for exactly once under a process-global
 * mutex. A callback reentering this same function returns immediately; the
 * outer dispatch owns the clock until it finishes. No new thread/scheduler,
 * fallback transport or automatic replay is introduced. */
void sctp_global_poll_timers(void) {
    if (g_sctp_timer_dispatching || g_sctp_timer_retirement_depth != 0 ||
        atomic_load_explicit(&g_sctp_initialized, memory_order_acquire) == 0)
        return;

    cmeta_mutex_lock(&g_sctp_timer_mutex);
    g_sctp_timer_dispatching = 1;
    uint64_t now = cmeta_monotonic_ms();
    if (now > g_sctp_timer_last_ms) {
        uint64_t remaining = now - g_sctp_timer_last_ms;
        while (remaining != 0) {
            uint32_t step = remaining > UINT32_MAX
                ? UINT32_MAX : (uint32_t)remaining;
            g_sctp_timer_last_ms += step;
            remaining -= step;
            usrsctp_handle_timers(step);
        }
    }
    g_sctp_timer_dispatching = 0;
    cmeta_mutex_unlock(&g_sctp_timer_mutex);
}

/* Source-level callback retirement fence for usrsctp's no-thread timer
 * pump. The native 0.9.5.0 callout queue lock is dropped before an expired
 * callback executes. Peer destruction therefore must not reclaim the raw
 * application address while ANY globally dispatched timer callback might
 * still enter its first dc_peer_acquire().
 *
 * The caller has already published peer->destroying, drained admitted peer
 * operations and native Socket borrows BEFORE taking this mutex. It then
 * closes/deregisters SCTP under the same mutex that owns timer dispatch, and
 * releases it before asynchronous transport/DTLS teardown. This is not a
 * second scheduler, lease registry, or retry path. */
int sctp_global_timer_retirement_begin(void) {
    if (atomic_load_explicit(&g_sctp_initialized, memory_order_acquire) == 0)
        return 0;
    if (g_sctp_timer_dispatching || g_sctp_timer_retirement_depth != 0) {
        /* This thread already owns the global timer dispatch lock. */
        ++g_sctp_timer_retirement_depth;
        return 0;
    }
    cmeta_mutex_lock(&g_sctp_timer_mutex);
    g_sctp_timer_retirement_depth = 1;
    return 1;
}

void sctp_global_timer_retirement_end(int acquired) {
    if (g_sctp_timer_retirement_depth != 0)
        --g_sctp_timer_retirement_depth;
    if (acquired)
        cmeta_mutex_unlock(&g_sctp_timer_mutex);
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
    if (!peer || !dtls_session_is_ready(peer)) return;

    struct socket *socket = sctp_native_borrow(peer);
    if (!socket) return;
    memset(&status, 0, sizeof(status));
    int result = usrsctp_getsockopt(socket, IPPROTO_SCTP, SCTP_STATUS,
                                    &status, &status_len);
    if (result != 0) {
        sctp_native_return(peer);
        TLOG_WARNF("SCTP status poll failed reason='{}' errno={}",
                   reason ? reason : "", errno);
        return;
    }

    /* The native borrow also serializes the CONNECTED notification against
     * foreign close: close must not publish DISCONNECTING until every old
     * native-status callback has completed. Reentrant close from on_state
     * defers physical teardown until this scope exits. */
    int promote = 0;
    cmeta_mutex_lock(&peer->operation_mutex);
    if (!peer->sctp.native_close_requested && !peer->sctp.connected &&
        (status.sstat_state == SCTP_ESTABLISHED ||
         (status.sstat_state == SCTP_COOKIE_ECHOED &&
          status.sstat_outstrms > 0 && status.sstat_instrms > 0))) {
        peer->sctp.connected = 1;
        promote = 1;
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    if (promote)
        dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
    sctp_native_return(peer);
}

int sctp_global_init(void) {
    atomic_fetch_add_explicit(&g_context_count, 1, memory_order_relaxed);
    cmeta_once(&g_sctp_once, sctp_global_init_once);
    if (atomic_load_explicit(&g_sctp_initialized, memory_order_acquire) == 0) {
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

/* This hook exists only in the private production-source concurrency
 * executable, never in the installed DataChannel DLL. It holds the exact
 * timer callback interval BEFORE the first dc_peer_acquire. Returning nonzero
 * avoids dereferencing a potentially retired peer in a RED control. */
#ifdef TURBO_MEDIA_TEST_SCTP_PRE_ADMISSION
extern int turbo_media_test_sctp_before_peer_admission(void *peer_address);
#endif
int sctp_outbound_packet_cb(void *addr, void *data, size_t length, uint8_t tos, uint8_t set_df) {
    (void)tos;
    (void)set_df;

#ifdef TURBO_MEDIA_TEST_SCTP_PRE_ADMISSION
    if (turbo_media_test_sctp_before_peer_admission(addr))
        return -1;
#endif
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)addr;
    if (!peer || dc_peer_acquire(peer) != 0) {
        return -1;
    }
    if (!dtls_session_is_ready(peer)) {
        TLOG_WARNF("SCTP outbound dropped before DTLS handshake: bytes={}",
                  length);
        dc_peer_release(peer);
        return -1;
    }

    int written = dtls_write_application_data(peer, data, length);
    if (written <= 0) {
        TLOG_ERRORF("SCTP outbound DTLS write failed bytes={} ret={} errno={}",
                   length, written, errno);
        dc_peer_release(peer);
        return -1;
    }

    dc_peer_release(peer);
    return 0;
}

/* Partial deliveries are borrowed from usrsctp only for this callback. Keep
 * bounded, peer-owned copies; DCEP uses the same message boundary as user data. */
static void sctp_receive_reset_locked(turbo_dc_peer_t *peer, sctp_receive_message_t *message) {
    if (message->body) {
        tstr_free(message->body);
        --peer->sctp.receive_buffers;
    }
    memset(message, 0, sizeof(*message));
}

void sctp_receive_stop(turbo_dc_peer_t *peer) {
    cmeta_mutex_lock(&peer->operation_mutex);
    peer->sctp.receive_stopped = 1;
    for (size_t i = 0; i < SCTP_RECEIVE_SLOTS; ++i)
        sctp_receive_reset_locked(peer, &peer->sctp.receive[i]);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

void sctp_receive_discard_stream(turbo_dc_peer_t *peer, uint16_t stream) {
    cmeta_mutex_lock(&peer->operation_mutex);
    for (size_t i = 0; i < SCTP_RECEIVE_SLOTS; ++i) {
        sctp_receive_message_t *message = &peer->sctp.receive[i];
        if (message->active && message->stream == stream) {
            if (message->body) {
                tstr_free(message->body);
                message->body = NULL;
                --peer->sctp.receive_buffers;
            }
            /* Consume the old message through EOR before permitting SID reuse. */
            message->discard = 1;
        }
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static void sctp_receive_abort(turbo_dc_peer_t *peer, uint32_t stream) {
    cmeta_mutex_lock(&peer->operation_mutex);
    for (size_t i = 0; i < SCTP_RECEIVE_SLOTS; ++i) {
        sctp_receive_message_t *message = &peer->sctp.receive[i];
        if (message->active && (stream == UINT32_MAX || message->stream == stream))
            sctp_receive_reset_locked(peer, message);
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
}

/* Return 1 only for a complete message; *owned transfers an assembled buffer
 * to the callback caller. Zero means pending/discarded or a reported failure. */
static int sctp_receive_fragment(turbo_dc_peer_t *peer, const void *data, size_t size,
                                 struct sctp_rcvinfo rcv, int flags, tstr *owned) {
    sctp_receive_message_t *message = NULL, *available = NULL;
    const char *error = NULL;
    uint32_t ppid = ntohl(rcv.rcv_ppid);
    size_t limit = ppid == SCTP_PPID_DCEP ? DCEP_MAX_PACKET_SIZE : TURBO_DC_MAX_MESSAGE_SIZE;
    int complete = 0;
    *owned = NULL;
    cmeta_mutex_lock(&peer->operation_mutex);
    if (peer->sctp.receive_stopped) goto done;
    for (size_t i = 0; i < SCTP_RECEIVE_SLOTS; ++i) {
        sctp_receive_message_t *candidate = &peer->sctp.receive[i];
        if (candidate->active && candidate->stream == rcv.rcv_sid) message = candidate;
        if (!candidate->active && !available) available = candidate;
    }
    if (message && message->discard) {
        if (flags & MSG_EOR) sctp_receive_reset_locked(peer, message);
        goto done;
    }
    if (size > limit) {
        error = "SCTP message exceeds local receive limit";
        goto done;
    }
    if (!message && (flags & MSG_EOR)) {
        complete = 1;
        goto done;
    }
    if (!message) {
        if (!available || peer->sctp.receive_buffers >= SCTP_RECEIVE_SLOTS) {
            error = "SCTP partial-message capacity exhausted";
            goto done;
        }
        /* One bounded allocation, with no geometric growth or retained spare
         * buffers. Maximum charged storage is eight times 256 KiB. */
        tstr body = tstr_new_len(NULL, limit);
        if (!body) {
            error = "SCTP message allocation failed";
            goto done;
        }
        tstr_set_len(body, 0);
        message = available;
        message->body = body;
        message->active = 1;
        message->stream = rcv.rcv_sid;
        message->sequence = rcv.rcv_ssn;
        message->association = rcv.rcv_assoc_id;
        message->unordered = rcv.rcv_flags & SCTP_UNORDERED;
        message->ppid = ppid;
        ++peer->sctp.receive_buffers;
    }
    if (message->sequence != rcv.rcv_ssn || message->association != rcv.rcv_assoc_id ||
        message->unordered != (rcv.rcv_flags & SCTP_UNORDERED) || message->ppid != ppid) {
        error = "SCTP partial-message identity changed";
        goto done;
    }
    size_t used = tstr_len(message->body);
    if (size > limit - used) {
        error = "SCTP message exceeds local receive limit";
        goto done;
    }
    memcpy(message->body + used, data, size);
    tstr_set_len(message->body, used + size);
    if (flags & MSG_EOR) {
        *owned = message->body;
        /* Keep the allocation charged while the external callback borrows it. */
        memset(message, 0, sizeof(*message));
        complete = 1;
    }
done:
    cmeta_mutex_unlock(&peer->operation_mutex);
    if (error) dc_fail_peer(peer, TURBO_DC_ERROR_SCTP_RECEIVE, error);
    return complete;
}

int sctp_inbound_packet_cb(struct socket *sock, union sctp_sockstore addr,
                           void *data, size_t datalen,
                           struct sctp_rcvinfo rcv, int flags, void *ulp_info) {
    (void)sock;
    (void)addr;

    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)ulp_info;
    if (!peer || dc_peer_acquire(peer) != 0) {
        if (data) free(data);
        return 1;
    }
    if (!data) {
        sctp_receive_stop(peer);
        dc_peer_release(peer);
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
            TLOG_INFOF("SCTP assoc change state={} out={} in={} error={}",
                      sctp_assoc_state_name(assoc->sac_state),
                      assoc->sac_outbound_streams,
                      assoc->sac_inbound_streams,
                      assoc->sac_error);
            if (assoc->sac_state == SCTP_COMM_UP) {
                /* A native timer or conninput may deliver a delayed COMM_UP.
                 * Admit notification delivery under the same bounded Socket
                 * owner as STATUS, sendv and stream reset. A foreign close
                 * cannot publish DISCONNECTING while this borrow remains,
                 * and an already sealed admission gate rejects promotion. */
                struct socket *native = sctp_native_borrow(peer);
                if (native) {
                    int promote = 0;
                    cmeta_mutex_lock(&peer->operation_mutex);
                    if (!peer->sctp.native_close_requested &&
                        !peer->sctp.connected) {
                        peer->sctp.connected = 1;
                        promote = 1;
                    }
                    cmeta_mutex_unlock(&peer->operation_mutex);
                    /* Application callbacks can close this peer reentrantly;
                     * never invoke them while holding operation_mutex. */
                    if (promote)
                        dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
                    sctp_native_return(peer);
                }
            } else if (assoc->sac_state == SCTP_RESTART || assoc->sac_state == SCTP_COMM_LOST ||
                       assoc->sac_state == SCTP_SHUTDOWN_COMP) {
                sctp_receive_abort(peer, UINT32_MAX);
            }
        } else if (notif->sn_header.sn_type == SCTP_PARTIAL_DELIVERY_EVENT &&
                   datalen >= sizeof(struct sctp_pdapi_event) &&
                   notif->sn_pdapi_event.pdapi_indication == SCTP_PARTIAL_DELIVERY_ABORTED) {
            sctp_receive_abort(peer, notif->sn_pdapi_event.pdapi_stream);
        } else if (notif->sn_header.sn_type == SCTP_STREAM_RESET_EVENT &&
                   datalen >= sizeof(struct sctp_stream_reset_event) &&
                   !(notif->sn_strreset_event.strreset_flags &
                     (SCTP_STREAM_RESET_DENIED | SCTP_STREAM_RESET_FAILED)) &&
                   (notif->sn_strreset_event.strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN)) {
            /* The variable-length stream list is not contained in the local
             * notification header copy. Read aligned values via memcpy. */
            size_t offset = sizeof(struct sctp_stream_reset_event);
            if (offset == datalen) {
                sctp_receive_abort(peer, UINT32_MAX);
            } else {
                while (datalen - offset >= sizeof(uint16_t)) {
                    uint16_t stream;
                    memcpy(&stream, (const uint8_t *)data + offset, sizeof(stream));
                    sctp_receive_abort(peer, stream);
                    offset += sizeof(stream);
                }
            }
        }

        free(data);
        dc_peer_release(peer);
        return 1;
    }

    tstr assembled = NULL;
    if (!sctp_receive_fragment(peer, data, datalen, rcv, flags, &assembled)) {
        free(data);
        dc_peer_release(peer);
        return 1;
    }
    const void *payload = assembled ? assembled : data;
    if (assembled) datalen = tstr_len(assembled);
    uint16_t stream_id = rcv.rcv_sid;
    uint32_t ppid = ntohl(rcv.rcv_ppid);
    if (ppid == SCTP_PPID_DCEP) {
        dcep_handle_message(peer, stream_id, (const uint8_t *)payload, datalen);
    } else {
        /* The peer operation lease does not own individual channel storage.
         * Hold a separate bounded channel lease across the application
         * callback so a concurrent channel_close cannot unmap and free it. */
        turbo_dc_channel_t *channel = dc_channel_admit(peer, stream_id);
        if (channel) {
            cmeta_mutex_lock(&peer->operation_mutex);
            turbo_dc_message_cb callback = !channel->closing && channel->is_open
                ? channel->on_message : NULL;
            void *callback_data = channel->user_data;
            cmeta_mutex_unlock(&peer->operation_mutex);
            if (callback) {
                int is_binary = (ppid == SCTP_PPID_BINARY || ppid == SCTP_PPID_BINARY_EMPTY);
                /* Empty-message PPIDs describe the application length independently
                 * of the SCTP placeholder payload (RFC 8831 section 6.6). */
                if (ppid == SCTP_PPID_STRING_EMPTY || ppid == SCTP_PPID_BINARY_EMPTY)
                    datalen = 0;
                callback(channel, payload, datalen, is_binary, callback_data);
            }
            dc_channel_release(channel);
        }
    }

    if (assembled) {
        tstr_free(assembled);
        cmeta_mutex_lock(&peer->operation_mutex);
        --peer->sctp.receive_buffers;
        cmeta_mutex_unlock(&peer->operation_mutex);
    }
    free(data);
    dc_peer_release(peer);
    return 1;
}

/* ============================================================================
 * SCTP Socket Configuration
 * ============================================================================ */

static int configure_sctp_socket(struct socket *sock, uint16_t mtu) {
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
        SCTP_STREAM_RESET_EVENT,
        SCTP_PARTIAL_DELIVERY_EVENT
    };

    for (size_t i = 0; i < sizeof(event_types) / sizeof(event_types[0]); i++) {
        event.se_type = event_types[i];
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) != 0)
            return -1;
    }

    {
        struct sctp_paddrparams params;
        memset(&params, 0, sizeof(params));
        params.spp_assoc_id = 0;
        params.spp_flags = SPP_PMTUD_DISABLE;
        params.spp_pathmtu = mtu;
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS,
                               &params, sizeof(params)) != 0) {
            return -1;
        }
    }

    return 0;
}

int sctp_reset_channel_stream(turbo_dc_peer_t *peer, uint16_t stream_id) {
    if (!peer || !peer->operation_sync_initialized) return -1;
    struct socket *socket = sctp_native_borrow(peer);
    if (!socket) {
        /* No raw Socket read outside the native Owner mutex. A terminal
         * admission gate makes channel reset unnecessary, not a send error.
         * A live owner-capacity/token rejection remains a real failure and
         * must not be silently accepted as a successful reset. */
        cmeta_mutex_lock(&peer->operation_mutex);
        int terminal = peer->destroying || peer->sctp.native_close_requested ||
                       peer->sctp.socket == NULL;
        cmeta_mutex_unlock(&peer->operation_mutex);
        return terminal ? 1 : -1;
    }
    /* Stream-list payload is variable-length, but the struct header needs
     * its required alignment on C11 and across ARM/MSVC/Clang targets. */
    _Alignas(struct sctp_reset_streams)
    uint8_t reset_buffer[sizeof(struct sctp_reset_streams) + sizeof(uint16_t)];
    struct sctp_reset_streams *reset_request =
        (struct sctp_reset_streams *)reset_buffer;
    memset(reset_buffer, 0, sizeof(reset_buffer));
    reset_request->srs_assoc_id = SCTP_ALL_ASSOC;
    reset_request->srs_flags = SCTP_STREAM_RESET_OUTGOING;
    reset_request->srs_number_streams = 1;
    reset_request->srs_stream_list[0] = stream_id;
    int result = usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_RESET_STREAMS,
                                    reset_buffer, sizeof(reset_buffer));
    sctp_native_return(peer);
    return result;
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

    if (configure_sctp_socket(sock, peer->ctx->sctp_mtu) != 0) {
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
    if (!peer) return -1;
    struct socket *socket = sctp_native_borrow(peer);
    if (!socket) return -1;

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

    int rc = 0;
    if (usrsctp_bind(socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
        TLOG_ERRORF("SCTP bind failed errno={}", errno);
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_BIND, NULL);
        rc = -1;
    } else {
        int ret = usrsctp_connect(socket, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
        if (ret < 0 && errno != EINPROGRESS) {
            TLOG_ERRORF("SCTP connect failed ret={} errno={}", ret, errno);
            dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_CONNECT, NULL);
            rc = -1;
        }
    }
    if (!rc) {
        /* Only the native owner can publish this state; close owns the same
         * terminal gate, so late connect cannot rewrite it after close. */
        cmeta_mutex_lock(&peer->operation_mutex);
        if (!peer->sctp.native_close_requested) peer->sctp.connected = 0;
        cmeta_mutex_unlock(&peer->operation_mutex);
    }
    sctp_native_return(peer);
    return rc;
}
