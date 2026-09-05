#ifndef TURBO_DC_MSG_H
#define TURBO_DC_MSG_H

/**
 * @file turbo_dc_msg.h
 * @brief LTV message framing for WebRTC DataChannel
 *
 * Provides self-describing message format over DataChannel:
 *   +----------+------+----------+
 *   | Length   | Type | Value    |
 *   | (varint) | (1B) | (N bytes)|
 *   +----------+------+----------+
 *
 * Benefits:
 * - Type discrimination without app-level protocol negotiation
 * - Compact varint length encoding
 * - Zero-copy parsing on receive
 * - Compatible with browser WebRTC via simple JS decoder
 */

#include "turbo_datachannel.h"
#include <ltv_parser.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Send API
 * ============================================================================ */

/**
 * @brief Send LTV-encoded message on data channel
 * @param channel  DataChannel handle
 * @param type     Message type (application-defined)
 * @param data     Message payload
 * @param len      Payload length
 * @return 0 on success, negative on error
 *
 * Encodes message as LTV and sends as binary.
 */
TURBO_MEDIA_API int turbo_dc_send_ltv(turbo_dc_channel_t *channel, uint8_t type,
                                 const void *data, size_t len);

/**
 * @brief Send raw bytes (no LTV encoding)
 * @param channel  DataChannel handle
 * @param data     Raw data
 * @param len      Data length
 * @param is_binary TRUE for binary, FALSE for text
 * @return 0 on success, negative on error
 *
 * Bypasses LTV encoding for legacy/text messages.
 */
static inline int turbo_dc_send_raw(turbo_dc_channel_t *channel,
                                     const void *data, size_t len,
                                     int is_binary) {
    return turbo_dc_channel_send(channel, data, len, is_binary);
}

/* ============================================================================
 * Receive API
 * ============================================================================ */

/**
 * @brief Parse received data as LTV message
 * @param data     Received data from on_message callback
 * @param len      Data length
 * @param out      Output: pointer to LTV message handle (caller must free)
 * @return 0 on success, -1 if not valid LTV
 *
 * The parsed value borrows @p data. The caller must keep the input buffer
 * alive and unchanged until it releases the message handle.
 *
 * Usage in on_message callback:
 *   void on_message(channel, data, len, is_binary, user_data) {
 *       ltv_message_t *msg = NULL;
 *       if (turbo_dc_parse_ltv(data, len, &msg) == 0) {
 *           uint8_t type = msg->type;
 *           // handle type...
 *           turbo_dc_ltv_free(&msg);
 *       }
 *   }
 */
TURBO_MEDIA_API int turbo_dc_parse_ltv(const void *data, size_t len, ltv_message_t **out);

/**
 * @brief Release an LTV message allocated by TurboMedia and clear the handle.
 */
TURBO_MEDIA_API void turbo_dc_ltv_free(ltv_message_t **message);

/**
 * @brief Check if data looks like LTV message
 * @param data     Received data
 * @param len      Data length
 * @return 1 if valid LTV, 0 otherwise
 */
TURBO_MEDIA_API int turbo_dc_is_ltv(const void *data, size_t len);

/* ============================================================================
 * Stream Parser (for fragmented messages)
 * ============================================================================ */

typedef struct turbo_dc_msg_stream_s turbo_dc_msg_stream_t;

/**
 * @brief Create stream parser for handling message fragments
 * @param buffer_size  Internal buffer size (0 = default 64KB)
 * @return Stream parser or NULL on error
 */
TURBO_MEDIA_API turbo_dc_msg_stream_t *turbo_dc_msg_stream_create(size_t buffer_size);

/**
 * @brief Feed data to stream parser
 * @param stream  Stream parser
 * @param data    Received data
 * @param len     Data length
 * @param out     Output: pointer to LTV message handle on completion
 * @return 1 if message complete, 0 if need more data, -1 on error
 *
 * Release the returned handle with turbo_dc_ltv_free(). Its value borrows the
 * stream reassembly buffer and is valid only until the next feed, reset, or
 * destroy call on @p stream.
 */
TURBO_MEDIA_API int turbo_dc_msg_stream_feed(turbo_dc_msg_stream_t *stream,
                                        const void *data, size_t len,
                                        ltv_message_t **out);

/**
 * @brief Reset stream parser state
 */
TURBO_MEDIA_API void turbo_dc_msg_stream_reset(turbo_dc_msg_stream_t *stream);

/**
 * @brief Destroy stream parser
 */
TURBO_MEDIA_API void turbo_dc_msg_stream_destroy(turbo_dc_msg_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DC_MSG_H */
