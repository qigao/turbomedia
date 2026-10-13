/**
 * turbo_datachannel_internal.h - Internal definitions shared across modules
 *
 * NOT A PUBLIC API - for internal use only
 */

#ifndef TURBO_DATACHANNEL_INTERNAL_H
#define TURBO_DATACHANNEL_INTERNAL_H

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include <cnet/cnet.h>
#include <platform.h>
#include <tstr.h>
#include <salts/thread.h>
#include <cstl/hash_map.h>
#include "gmssl_dtls.h"
#include <usrsctp.h>
#include <roaring/roaring.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_LABEL_LEN 256
#define MAX_PROTOCOL_LEN 256
#define SCTP_MTU_DEFAULT 1188
#define SCTP_MTU_MIN 512 /* usrsctp SCTP_SMALLEST_PMTU, excluding common header */
#define DTLS_MTU_DEFAULT 1280
#define MAX_CHANNELS 65536u
#define SCTP_ASSOCIATION_PORT 5000
#define SCTP_MAX_STREAMS 65535u
#define DCEP_MAX_PACKET_SIZE 512
#define SCTP_RECEIVE_SLOTS 8u /* At most 2 MiB of retained application payload. */

/* SCTP PPIDs */
#define SCTP_PPID_DCEP 50
#define SCTP_PPID_STRING 51
#define SCTP_PPID_BINARY 53
#define SCTP_PPID_STRING_EMPTY 56
#define SCTP_PPID_BINARY_EMPTY 57

/* DCEP message types */
#define DCEP_DATA_CHANNEL_OPEN 0x03
#define DCEP_DATA_CHANNEL_ACK  0x02

/* DCEP channel types */
#define DCEP_CHANNEL_RELIABLE_ORDERED   0x00
#define DCEP_CHANNEL_RELIABLE_UNORDERED 0x80
#define DCEP_CHANNEL_REXMIT             0x01
#define DCEP_CHANNEL_TIMED              0x02

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/* DTLS session state */
typedef struct {
    turbo_gdtls *engine;
    int handshake_done;
    int application_ready;
    int stopped;
    /* Admission is protected by operation_mutex; engine state by busy. */
    int busy;
    int handshake_started;
    int timer_active;
} dtls_session_t;

/* SCTP session state */
typedef struct {
    tstr body;
    uint32_t ppid;
    sctp_assoc_t association;
    uint16_t stream;
    uint16_t sequence;
    uint16_t unordered;
    int active;
    int discard;
} sctp_receive_message_t;

typedef struct {
    struct socket *socket;
    int connected;
    /* operation_mutex protects partial messages and buffer admission. Completed
     * buffers remain charged until their callback returns, outside the lock. */
    sctp_receive_message_t receive[SCTP_RECEIVE_SLOTS];
    unsigned receive_buffers;
    int receive_stopped;

    /* One bounded ownership protocol for every native usrsctp operation.
     * Under peer->operation_mutex; callbacks always run outside that mutex.
     * Thread tokens let reentrant close defer physical release until its
     * own admitted native operation unwinds. */
    unsigned native_borrows;
    int native_close_requested;
    int native_shutdown_requested;
    int native_closing;
    int native_closed;
    const void *native_closer_token;
    struct {
        const void *thread;
        unsigned depth;
    } native_owners[16];
} sctp_session_t;

/* Data channel context - internal */
struct turbo_dc_peer_s;

typedef void (*dc_transport_task_fn)(void *arg1, void *arg2);

struct turbo_dc_context_s {
    turbo_gdtls_context *dtls_context;
    int is_server;
    int initialized;
    int disable_sctp;
    turbo_dc_transport_t transport;   /* Transport type */
    uint16_t sctp_mtu;                /* SCTP path MTU */
    uint16_t dtls_mtu;                /* DTLS MTU */
    turbo_dc_error_t last_error;      /* Only for errors before peer exists */
    tstr local_fingerprint;         /* SHA-256 hex fingerprint */
    tstr local_fingerprint_hash;    /* "sha-256" */
    cmeta_thread_t transport_thread;  /* CNet progress and DTLS deadline owner */
    int transport_thread_started;
    cmeta_mutex_t transport_mutex;
    cmeta_cond_t transport_cond;
    int transport_sync_initialized;
    int transport_stop_requested;
    int transport_command_pending;
    int transport_command_done;
    uint32_t active_dtls_timers;     /* Protected by transport_mutex */
    dc_transport_task_fn transport_command;
    void *transport_command_arg1;
    void *transport_command_arg2;
    cmeta_mutex_t peer_mutex;
    int peer_mutex_initialized;
    int destroying;
    struct turbo_dc_peer_s *peers_head;
};

/* Forward declaration */
struct salts_ice_agent_s;

/* Transport operations vtable */
typedef struct {
    void (*send)(struct turbo_dc_peer_s *peer, const void *data, size_t len);
    void (*close)(struct turbo_dc_peer_s *peer);
    void (*destroy)(struct turbo_dc_peer_s *peer);
} dc_transport_ops_t;

/* Peer connection - internal */
struct turbo_dc_peer_s {
    turbo_dc_context_t *ctx;
    struct turbo_dc_peer_s *next_in_context;
    void *user_data;
    int is_dtls_server;

    /* Lifetime admission/drain, distinct from DTLS operation serialization. */
    cmeta_mutex_t operation_mutex;
    cmeta_cond_t operation_cond;
    int operation_sync_initialized;
    int destroying;
    uint32_t active_operations;

    /* Transport (unified via ops) */
    cnet_client stream_client;
    cnet_listener listener;
    cnet_connection stream_connection;
    cnet_datagram datagram;
    cnet_datagram_peer remote_datagram_peer;
    int stream_client_initialized;
    int listener_initialized;
    int datagram_initialized;
    const dc_transport_ops_t *transport_ops;

    /* Externally owned datagram transport, typically an ICE agent. */
    void *external_transport;
    turbo_dc_transport_send_cb external_transport_send;

    /* DTLS */
    dtls_session_t dtls;

    /* SCTP */
    sctp_session_t sctp;
    int sctp_address_registered;

    /* Data channels */
    hash_map_t channels;  /* uint16_t stream id -> turbo_dc_channel_t * */
    int channels_initialized;
    roaring_bitmap_t *channel_ids;  /* Bitmap for ID allocation */

    /* Error state (per-peer to avoid race conditions) */
    turbo_dc_error_t last_error;

    /* State */
    turbo_dc_state_t state;

    /* Raw RTP/RTCP transport data callback used by media engines. */
    void (*on_transport_data)(void *user_data, const uint8_t *data, size_t len);
    void *transport_data_user_data;
    uint32_t transport_data_callbacks;

    /* Callbacks */
    turbo_dc_state_cb on_state;
    turbo_dc_channel_cb on_channel;
    turbo_dc_error_cb on_error;

    /* Remote endpoint */
    tstr remote_host;
    uint16_t remote_port;
    int has_remote_datagram_peer;

    /* Security: expected remote fingerprint from SDP */
    tstr remote_fingerprint;
    tstr remote_fingerprint_hash;

};

/* Data channel - internal */
struct turbo_dc_channel_s {
    turbo_dc_peer_t *peer;
    void *user_data;

    uint16_t id;
    tstr label;
    tstr protocol;

    turbo_dc_channel_config_t config;

    int is_open;
    int dcep_sent;
    size_t buffered_amount;

    /* Per-channel callback admission; protected by peer->operation_mutex.
     * An admitted application callback retains the object even if another
     * thread or the callback itself closes/unpublishes this channel. */
    unsigned callback_refs;
    int closing;

    /* Callbacks */
    turbo_dc_open_cb on_open;
    turbo_dc_message_cb on_message;
    turbo_dc_close_cb on_close;
};

/* ============================================================================
 * Error Handling (internal)
 * ============================================================================ */

void dc_set_peer_error(turbo_dc_peer_t *peer, turbo_dc_error_code_t code, const char *detail);
void dc_notify_state(turbo_dc_peer_t *peer, turbo_dc_state_t new_state);
void dc_fail_peer(turbo_dc_peer_t *peer, turbo_dc_error_code_t code, const char *detail);
int dc_peer_acquire(turbo_dc_peer_t *peer);
void dc_peer_release(turbo_dc_peer_t *peer);

/* Internal bounded channel lifetime lease: never invoke application code
 * under operation_mutex. Release the final close+callback lease to reclaim. */
static inline turbo_dc_channel_t *dc_channel_admit(turbo_dc_peer_t *peer, uint16_t id);
static inline void dc_channel_release(turbo_dc_channel_t *channel);

/* Legacy - for context-level errors during creation (before peer exists) */
void dc_set_context_error(turbo_dc_context_t *ctx, turbo_dc_error_code_t code, const char *detail);

/* ============================================================================
 * DTLS Session API (internal)
 * ============================================================================ */

int dtls_session_init(turbo_dc_peer_t *peer);
void dtls_session_poll_timeout(turbo_dc_peer_t *peer);
void dtls_send_output(turbo_dc_peer_t *peer);
void dtls_process_handshake(turbo_dc_peer_t *peer);
void dtls_handle_incoming(turbo_dc_peer_t *peer, const void *data, size_t len);
/* Caller holds a peer operation lease (or exclusive create/destroy ownership).
 * Never call application callbacks, SCTP or transport code while admitted. */
void dtls_session_lock(turbo_dc_peer_t *peer);
void dtls_session_unlock(turbo_dc_peer_t *peer);
void dtls_session_shutdown(turbo_dc_peer_t *peer);
int dtls_session_is_ready(turbo_dc_peer_t *peer);
int dtls_write_application_data(turbo_dc_peer_t *peer, const void *data, size_t len);

/* ============================================================================
 * SCTP Session API (internal)
 * ============================================================================ */

int sctp_global_init(void);
void sctp_global_cleanup(void);
/* Advance no-thread usrsctp timers by monotonic elapsed time once per
 * process, serialized across callers and safe from synchronous reentry. */
void sctp_global_poll_timers(void);
/* Foreign peer destruction serializes its native Socket close and AF_CONN
 * deregistration with the global timer pump. begin() returns whether it
 * acquired that mutex; pass the same result to end(). Call only after all
 * previously admitted per-peer operations/Socket borrows have settled. */
int sctp_global_timer_retirement_begin(void);
void sctp_global_timer_retirement_end(int acquired);

int sctp_session_init(turbo_dc_peer_t *peer);
int sctp_start_association(turbo_dc_peer_t *peer);
/* Native Stream Reset outcome: 0 submitted; +1 skipped because Socket
 * teardown is terminal; -1 native call or live bounded-owner admission
 * failed. The caller must not treat terminal refusal as a send error. */
int sctp_reset_channel_stream(turbo_dc_peer_t *peer, uint16_t stream_id);
void sctp_receive_stop(turbo_dc_peer_t *peer);
void sctp_receive_discard_stream(turbo_dc_peer_t *peer, uint16_t stream_id);
void sctp_poll_status(turbo_dc_peer_t *peer, const char *reason);

/* SCTP callbacks (called by usrsctp) */
int sctp_outbound_packet_cb(void *addr, void *data, size_t length, uint8_t tos, uint8_t set_df);
int sctp_inbound_packet_cb(struct socket *sock, union sctp_sockstore addr,
                            void *data, size_t datalen,
                            struct sctp_rcvinfo rcv, int flags, void *ulp_info);

/* ============================================================================
 * DCEP Protocol API (internal)
 * ============================================================================ */

int dcep_send_open(turbo_dc_channel_t *channel);
int dcep_send_ack(turbo_dc_peer_t *peer, uint16_t stream_id);
void dcep_handle_message(turbo_dc_peer_t *peer, uint16_t stream_id,
                         const uint8_t *data, size_t len);

/* ============================================================================
 * Transport Helper (internal)
 * ============================================================================ */

void dc_send_transport_data(turbo_dc_peer_t *peer, const void *data, size_t len);

/* ============================================================================
 * Utility Helpers
 * ============================================================================ */

static inline void put_be16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

static inline uint16_t get_be16(const uint8_t *buf) {
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

/* Native operations borrow the usrsctp socket under one Owner-admission
 * protocol. At most 16 distinct threads and 256 concurrent/nested native
 * borrows per peer are admitted. A full registry fails closed, never bypasses
 * the socket ownership barrier. */
static inline struct socket *sctp_native_borrow(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) return NULL;
    const void *token = cmeta_thread_current_token();
    if (!token) return NULL;
    cmeta_mutex_lock(&peer->operation_mutex);
    struct socket *sock = NULL;
    if (!peer->destroying && !peer->sctp.native_close_requested &&
        peer->sctp.socket && peer->sctp.native_borrows < 256u) {
        int idx = -1;
        int available = -1;
        for (size_t i = 0; i < 16u; ++i) {
            if (peer->sctp.native_owners[i].thread == token) {
                idx = (int)i;
                break;
            }
            if (!peer->sctp.native_owners[i].thread && available < 0)
                available = (int)i;
        }
        if (idx < 0) idx = available;
        if (idx >= 0) {
            peer->sctp.native_owners[idx].thread = token;
            ++peer->sctp.native_owners[idx].depth;
            ++peer->sctp.native_borrows;
            sock = peer->sctp.socket;
        }
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    return sock;
}

/* Owner-close is permitted to reenter from within conninput/on_message. The
 * native handle is detached and released exactly once, after the *last*
 * admitted native operation finishes; no background thread is introduced. */
static inline void sctp_native_finish_close(turbo_dc_peer_t *peer,
                                            struct socket *socket,
                                            int shutdown) {
    if (shutdown) (void)usrsctp_shutdown(socket, SHUT_RDWR);
    (void)usrsctp_close(socket);
    cmeta_mutex_lock(&peer->operation_mutex);
    peer->sctp.native_closing = 0;
    peer->sctp.native_closed = 1;
    cmeta_cond_broadcast(&peer->operation_cond);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static inline void sctp_native_return(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) return;
    const void *token = cmeta_thread_current_token();
    struct socket *last_socket = NULL;
    int last_shutdown = 0;
    cmeta_mutex_lock(&peer->operation_mutex);
    for (size_t i = 0; i < 16u; ++i) {
        if (peer->sctp.native_owners[i].thread == token &&
            peer->sctp.native_owners[i].depth > 0) {
            if (--peer->sctp.native_owners[i].depth == 0)
                peer->sctp.native_owners[i].thread = NULL;
            if (peer->sctp.native_borrows > 0)
                --peer->sctp.native_borrows;
            break;
        }
    }
    if (peer->sctp.native_borrows == 0 &&
        peer->sctp.native_close_requested &&
        !peer->sctp.native_closing && peer->sctp.socket) {
        peer->sctp.native_closing = 1;
        peer->sctp.native_closer_token = token;
        last_socket = peer->sctp.socket;
        last_shutdown = peer->sctp.native_shutdown_requested;
        peer->sctp.socket = NULL;
    }
    cmeta_cond_broadcast(&peer->operation_cond);
    cmeta_mutex_unlock(&peer->operation_mutex);
    if (last_socket)
        sctp_native_finish_close(peer, last_socket, last_shutdown);
}

/* Called only after peer->destroying has sealed both peer and Socket
 * admissions. Drain all earlier native borrows / in-progress physical close
 * BEFORE acquiring the process-global timer retirement fence, so no
 * borrowed native operation can wait on the global timer mutex while the
 * destroying owner is waiting for that borrow to return. */
static inline void sctp_native_wait_quiescent(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) return;
    cmeta_mutex_lock(&peer->operation_mutex);
    while (peer->sctp.native_borrows != 0 || peer->sctp.native_closing)
        cmeta_cond_wait(&peer->operation_cond, &peer->operation_mutex);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static inline void sctp_native_request_close(turbo_dc_peer_t *peer, int shutdown) {
    if (!peer || !peer->operation_sync_initialized) return;
    cmeta_mutex_lock(&peer->operation_mutex);
    peer->sctp.native_close_requested = 1;
    if (shutdown) peer->sctp.native_shutdown_requested = 1;
    cmeta_cond_broadcast(&peer->operation_cond);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static inline void sctp_native_close_socket(turbo_dc_peer_t *peer, int shutdown) {
    if (!peer || !peer->operation_sync_initialized) return;
    const void *token = cmeta_thread_current_token();
    struct socket *socket = NULL;
    int perform_shutdown = 0;
    cmeta_mutex_lock(&peer->operation_mutex);
    peer->sctp.native_close_requested = 1;
    if (shutdown) peer->sctp.native_shutdown_requested = 1;
    /* A callback nested inside its own conninput/sendv must not deadlock
     * waiting for itself. The last native return will finalize close. */
    for (size_t i = 0; i < 16u; ++i) {
        if (peer->sctp.native_owners[i].thread == token &&
            peer->sctp.native_owners[i].depth > 0) {
            cmeta_mutex_unlock(&peer->operation_mutex);
            return;
        }
    }
    /* Likewise, an outbound callback from usrsctp_close on this very
     * thread cannot recursively wait on the closing owner. */
    if (peer->sctp.native_closing &&
        peer->sctp.native_closer_token == token) {
        cmeta_mutex_unlock(&peer->operation_mutex);
        return;
    }
    while (peer->sctp.native_borrows != 0 || peer->sctp.native_closing)
        cmeta_cond_wait(&peer->operation_cond, &peer->operation_mutex);
    if (peer->sctp.socket) {
        peer->sctp.native_closing = 1;
        peer->sctp.native_closer_token = token;
        socket = peer->sctp.socket;
        perform_shutdown = peer->sctp.native_shutdown_requested;
        peer->sctp.socket = NULL;
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    if (socket)
        sctp_native_finish_close(peer, socket, perform_shutdown);
}

/* ============================================================================
 * Channel ID Allocation (roaring bitmap)
 * ============================================================================ */

static inline int peer_alloc_channel_id(turbo_dc_peer_t *peer, uint16_t *out_id) {
    uint32_t start_id = (peer && peer->is_dtls_server) ? 1u : 0u;

    /* WebRTC DataChannel reserves even stream IDs for the DTLS client and odd
     * stream IDs for the DTLS server. */
    for (uint32_t id = start_id; id < MAX_CHANNELS; id += 2) {
        if (!roaring_bitmap_contains(peer->channel_ids, id)) {
            roaring_bitmap_add(peer->channel_ids, id);
            *out_id = (uint16_t)id;
            return 0;
        }
    }
    return -1;  /* No free IDs */
}

/* Channel-map and ID-bitmap helpers require peer->operation_mutex on
 * every path that may race with SCTP receive or channel close. */
static inline turbo_dc_channel_t *peer_get_channel(turbo_dc_peer_t *peer,
                                                    uint16_t id) {
    turbo_dc_channel_t **channel;

    if (!peer) return NULL;
    channel = (turbo_dc_channel_t **)hash_map_get(&peer->channels, &id);
    return channel ? *channel : NULL;
}

static inline int peer_set_channel(turbo_dc_peer_t *peer,
                                   uint16_t id,
                                   turbo_dc_channel_t *channel) {
    if (!peer || !channel) return -1;
    return hash_map_put(&peer->channels, &id, &channel) == STL_OK ? 0 : -1;
}

static inline void peer_remove_channel(turbo_dc_peer_t *peer, uint16_t id) {
    if (peer) {
        hash_map_remove(&peer->channels, &id, NULL);
    }
}

static inline void peer_free_channel_id(turbo_dc_peer_t *peer, uint16_t id) {
    roaring_bitmap_remove(peer->channel_ids, id);
}

static inline void peer_mark_channel_id(turbo_dc_peer_t *peer, uint16_t id) {
    roaring_bitmap_add(peer->channel_ids, id);
}

/* Inline internal helpers are shared by the production DCEP/SCTP modules and
 * the standalone conformance parser, without exporting new DLL symbols. */
static inline turbo_dc_channel_t *dc_channel_admit(turbo_dc_peer_t *peer, uint16_t id) {
    if (!peer || !peer->operation_sync_initialized) return NULL;
    cmeta_mutex_lock(&peer->operation_mutex);
    turbo_dc_channel_t *channel = !peer->destroying && peer->channels_initialized
        ? peer_get_channel(peer, id) : NULL;
    if (channel && !channel->closing) {
        ++channel->callback_refs;
    } else {
        channel = NULL;
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    return channel;
}

static inline void dc_channel_release(turbo_dc_channel_t *channel) {
    if (!channel || !channel->peer) return;
    turbo_dc_peer_t *peer = channel->peer;
    int reclaim = 0;
    cmeta_mutex_lock(&peer->operation_mutex);
    if (channel->callback_refs > 0)
        --channel->callback_refs;
    if (channel->closing && channel->callback_refs == 0) {
        peer_free_channel_id(peer, channel->id);
        reclaim = 1;
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    if (reclaim) {
        tstr_free(channel->label);
        tstr_free(channel->protocol);
        free(channel);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DATACHANNEL_INTERNAL_H */
