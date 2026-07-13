/**
 * turbo_datachannel_internal.h - Internal definitions shared across modules
 *
 * NOT A PUBLIC API - for internal use only
 */

#ifndef TURBO_DATACHANNEL_INTERNAL_H
#define TURBO_DATACHANNEL_INTERNAL_H

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include <platform.h>
#include <turbo_str.h>
#include <turbo_coro_context.h>
#include <turbo_datagram.h>
#include <turbo_kcp.h>
#include <turbo_stream.h>
#include <turbo_thread.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <usrsctp.h>
#include <roaring/roaring.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_LABEL_LEN 256
#define MAX_PROTOCOL_LEN 256
#define SCTP_MTU_DEFAULT 1188
#define DTLS_MTU_DEFAULT 1280
#define MAX_CHANNELS 256
#define SCTP_ASSOCIATION_PORT 5000
#define SCTP_MAX_STREAMS 300
#define DCEP_MAX_PACKET_SIZE 512

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
    SSL *ssl;
    BIO *read_bio;
    BIO *write_bio;
    int handshake_done;
    turbo_timer_t *retransmit_timer;  /* DTLS retransmission timer (NULL if no loop) */
} dtls_session_t;

/* SCTP session state */
typedef struct {
    struct socket *socket;
    int connected;
} sctp_session_t;

/* Data channel context - internal */
struct turbo_dc_context_s {
    SSL_CTX *ssl_ctx;
    int is_server;
    int initialized;
    int disable_sctp;
    turbo_dc_transport_t transport;   /* Transport type */
    uint16_t sctp_mtu;                /* SCTP path MTU */
    uint16_t dtls_mtu;                /* DTLS MTU */
    turbo_dc_error_t last_error;      /* Only for errors before peer exists */
    tstr_t local_fingerprint;         /* SHA-256 hex fingerprint */
    tstr_t local_fingerprint_hash;    /* "sha-256" */
    coro_context_t *transport_ctx;    /* Private CoroNet loop context */
    turbo_thread_t transport_thread;  /* Dedicated transport loop thread */
    int transport_thread_started;
};

/* Forward declaration */
struct turbo_dc_peer_s;
struct turbo_ice_agent_s;

/* Transport operations vtable */
typedef struct {
    void (*send)(struct turbo_dc_peer_s *peer, const void *data, size_t len);
    void (*close)(struct turbo_dc_peer_s *peer);
    void (*destroy)(struct turbo_dc_peer_s *peer);
} dc_transport_ops_t;

/* Peer connection - internal */
struct turbo_dc_peer_s {
    turbo_dc_context_t *ctx;
    void *user_data;
    int is_dtls_server;

    /* Transport (unified via ops) */
    void *transport;
    turbo_stream_listener_t *listener;  /* Only for TCP server mode */
    turbo_stream_t *server_stream;      /* Accepted TCP server connection */
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
    turbo_dc_channel_t *channels[MAX_CHANNELS];
    roaring_bitmap_t *channel_ids;  /* Bitmap for ID allocation */

    /* Error state (per-peer to avoid race conditions) */
    turbo_dc_error_t last_error;

    /* State */
    turbo_dc_state_t state;

    /* Raw RTP/RTCP transport data callback used by media engines. */
    void (*on_transport_data)(void *user_data, const uint8_t *data, size_t len);
    void *transport_data_user_data;

    /* Callbacks */
    turbo_dc_state_cb on_state;
    turbo_dc_channel_cb on_channel;
    turbo_dc_error_cb on_error;

    /* Remote endpoint */
    tstr_t remote_host;
    uint16_t remote_port;
    struct sockaddr_storage remote_addr;
    int has_remote_addr;

    /* Security: expected remote fingerprint from SDP */
    tstr_t remote_fingerprint;
    tstr_t remote_fingerprint_hash;

};

/* Data channel - internal */
struct turbo_dc_channel_s {
    turbo_dc_peer_t *peer;
    void *user_data;

    uint16_t id;
    tstr_t label;
    tstr_t protocol;

    turbo_dc_channel_config_t config;

    int is_open;
    int dcep_sent;
    size_t buffered_amount;

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

/* Legacy - for context-level errors during creation (before peer exists) */
void dc_set_context_error(turbo_dc_context_t *ctx, turbo_dc_error_code_t code, const char *detail);

/* ============================================================================
 * DTLS Session API (internal)
 * ============================================================================ */

int dtls_session_init(turbo_dc_peer_t *peer);
int dtls_session_init_timer(turbo_dc_peer_t *peer);

void dtls_session_cleanup(turbo_dc_peer_t *peer);
void dtls_send_output(turbo_dc_peer_t *peer);
void dtls_process_handshake(turbo_dc_peer_t *peer);
void dtls_handle_incoming(turbo_dc_peer_t *peer, const void *data, size_t len);

/* ============================================================================
 * SCTP Session API (internal)
 * ============================================================================ */

int sctp_global_init(void);
void sctp_global_cleanup(void);

int sctp_session_init(turbo_dc_peer_t *peer);
int sctp_start_association(turbo_dc_peer_t *peer);
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

static inline void peer_free_channel_id(turbo_dc_peer_t *peer, uint16_t id) {
    roaring_bitmap_remove(peer->channel_ids, id);
}

static inline void peer_mark_channel_id(turbo_dc_peer_t *peer, uint16_t id) {
    roaring_bitmap_add(peer->channel_ids, id);
}

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DATACHANNEL_INTERNAL_H */
