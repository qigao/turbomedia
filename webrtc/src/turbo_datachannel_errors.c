/**
 * turbo_datachannel_errors.c - Error code implementation
 */

#include "turbo_datachannel_errors.h"
#include "turbo_datachannel_internal.h"

/* ============================================================================
 * Error Strings
 * ============================================================================ */

const char *turbo_dc_error_string(turbo_dc_error_code_t code) {
    switch (code) {
        case TURBO_DC_ERROR_NONE:
            return "No error";

        /* Context errors */
        case TURBO_DC_ERROR_INVALID_CONFIG:
            return "Invalid configuration";
        case TURBO_DC_ERROR_NULL_LOOP:
            return "Event loop is required";
        case TURBO_DC_ERROR_ALLOC_CONTEXT:
            return "Failed to allocate context";
        case TURBO_DC_ERROR_SSL_CONTEXT:
            return "Failed to create SSL context";
        case TURBO_DC_ERROR_CERT_LOAD:
            return "Failed to load certificate/key";
        case TURBO_DC_ERROR_SCTP_INIT:
            return "Failed to initialize SCTP";

        /* Peer errors */
        case TURBO_DC_ERROR_NULL_CONTEXT:
            return "Context is NULL";
        case TURBO_DC_ERROR_ALLOC_PEER:
            return "Failed to allocate peer";
        case TURBO_DC_ERROR_NULL_PEER:
            return "Peer is NULL";
        case TURBO_DC_ERROR_CREATE_TRANSPORT:
            return "Failed to create transport";
        case TURBO_DC_ERROR_LISTEN_FAILED:
            return "Failed to listen on port";
        case TURBO_DC_ERROR_CONNECT_FAILED:
            return "Failed to connect to remote";

        /* DTLS errors */
        case TURBO_DC_ERROR_SSL_CREATE:
            return "Failed to create SSL object";
        case TURBO_DC_ERROR_DTLS_HANDSHAKE:
            return "DTLS handshake failed";

        /* SCTP errors */
        case TURBO_DC_ERROR_SCTP_SOCKET:
            return "Failed to create SCTP socket";
        case TURBO_DC_ERROR_SCTP_CONFIG:
            return "Failed to configure SCTP socket";
        case TURBO_DC_ERROR_SCTP_BIND:
            return "SCTP bind failed";
        case TURBO_DC_ERROR_SCTP_CONNECT:
            return "SCTP connect failed";
        case TURBO_DC_ERROR_SCTP_SEND:
            return "SCTP send failed";

        /* Channel errors */
        case TURBO_DC_ERROR_NULL_LABEL:
            return "Channel label is NULL";
        case TURBO_DC_ERROR_ALLOC_CHANNEL:
            return "Failed to allocate channel";
        case TURBO_DC_ERROR_INVALID_CHANNEL:
            return "Invalid channel";
        case TURBO_DC_ERROR_PEER_NOT_CONNECTED:
            return "Peer not connected";
        case TURBO_DC_ERROR_CHANNEL_NOT_OPEN:
            return "Channel not open";
        case TURBO_DC_ERROR_NULL_DATA:
            return "Data is NULL";
        case TURBO_DC_ERROR_INVALID_PEER_STATE:
            return "Invalid peer state";

        /* DCEP errors */
        case TURBO_DC_ERROR_DCEP_SEND_OPEN:
            return "Failed to send DCEP OPEN";
        case TURBO_DC_ERROR_DCEP_SEND_ACK:
            return "Failed to send DCEP ACK";

        default:
            return "Unknown error";
    }
}

/* ============================================================================
 * Context Error API
 * ============================================================================ */

turbo_dc_error_t turbo_dc_peer_get_error(turbo_dc_peer_t *peer) {
    turbo_dc_error_t empty = { TURBO_DC_ERROR_NONE, NULL, NULL };
    return peer ? peer->last_error : empty;
}

/* For errors during peer creation (before peer exists) */
turbo_dc_error_t turbo_dc_context_get_error(turbo_dc_context_t *ctx) {
    turbo_dc_error_t empty = { TURBO_DC_ERROR_NONE, NULL, NULL };
    return ctx ? ctx->last_error : empty;
}

/* ============================================================================
 * Internal Error Setting
 * ============================================================================ */

void dc_set_peer_error(turbo_dc_peer_t *peer, turbo_dc_error_code_t code, const char *detail) {
    if (peer) {
        peer->last_error.code = code;
        peer->last_error.message = turbo_dc_error_string(code);
        peer->last_error.detail = detail;
    }
}

/* For errors during context/peer creation (before peer exists) */
void dc_set_context_error(turbo_dc_context_t *ctx, turbo_dc_error_code_t code, const char *detail) {
    if (ctx) {
        ctx->last_error.code = code;
        ctx->last_error.message = turbo_dc_error_string(code);
        ctx->last_error.detail = detail;
    }
}
