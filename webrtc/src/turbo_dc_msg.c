/**
 * @file turbo_dc_msg.c
 * @brief LTV message framing for WebRTC DataChannel
 */

#include "turbo_dc_msg.h"
#include <stdlib.h>

enum { TURBO_DC_STACK_BUFFER_SIZE = 256 };
/* ============================================================================
 * Send API
 * ============================================================================ */

int turbo_dc_send_ltv(turbo_dc_channel_t *channel, uint8_t type,
                      const void *data, size_t len) {
    if (!channel) return -1;

    size_t wire_size = ltv_wire_size(len);
    if (wire_size == 0) return -1;

    /* Stack buffer for small messages, heap for large */
    uint8_t stack_buf[TURBO_DC_STACK_BUFFER_SIZE];
    uint8_t *buf = stack_buf;
    int heap_alloc = 0;

    if (wire_size > sizeof(stack_buf)) {
        buf = (uint8_t *)malloc(wire_size);
        if (!buf) return -1;
        heap_alloc = 1;
    }

    size_t written = ltv_build(type, (const uint8_t *)data, len, buf, wire_size);
    if (written == 0) {
        if (heap_alloc) free(buf);
        return -1;
    }

    int rc = turbo_dc_channel_send(channel, buf, written, 1 /* binary */);

    if (heap_alloc) free(buf);
    return rc;
}

/* ============================================================================
 * Receive API
 * ============================================================================ */

int turbo_dc_parse_ltv(const void *data, size_t len, ltv_message_t **out) {
    ltv_message_t *message;

    if (!out) return -1;
    *out = NULL;
    if (!data || len == 0) return -1;
    message = (ltv_message_t *)malloc(sizeof(*message));
    if (!message) return -1;
    if (ltv_parse((const uint8_t *)data, len, message) != LTV_PARSE_OK) {
        free(message);
        return -1;
    }
    *out = message;
    return 0;
}

void turbo_dc_ltv_free(ltv_message_t **message) {
    if (!message || !*message) return;
    free(*message);
    *message = NULL;
}

int turbo_dc_is_ltv(const void *data, size_t len) {
    if (!data || len < 2) return 0;

    uint32_t length;
    size_t header_size;
    
    LtvParseResult rc = ltv_peek_size((const uint8_t *)data, len,
                                      &length, &header_size);
    if (rc != LTV_PARSE_OK) return 0;

    size_t total = ltv_total_size(length, header_size);
    return (total <= len) ? 1 : 0;
}

/* ============================================================================
 * Stream Parser
 * ============================================================================ */

struct turbo_dc_msg_stream_s {
    ltv_stream_t *ltv_stream;
};

turbo_dc_msg_stream_t *turbo_dc_msg_stream_create(size_t buffer_size) {
    turbo_dc_msg_stream_t *stream = (turbo_dc_msg_stream_t *)malloc(sizeof(*stream));
    if (!stream) return NULL;

    stream->ltv_stream = ltv_stream_create(buffer_size);
    if (!stream->ltv_stream) {
        free(stream);
        return NULL;
    }

    return stream;
}

int turbo_dc_msg_stream_feed(turbo_dc_msg_stream_t *stream,
                              const void *data, size_t len,
                              ltv_message_t **out) {
    ltv_message_t *message;
    LtvParseResult rc;

    if (!out) return -1;
    *out = NULL;
    if (!stream || !data) return -1;
    message = (ltv_message_t *)malloc(sizeof(*message));
    if (!message) return -1;

    rc = ltv_stream_feed(stream->ltv_stream, (const uint8_t *)data, len,
                         message);
    if (rc == LTV_PARSE_OK) {
        *out = message;
        return 1;
    }
    free(message);
    if (rc == LTV_PARSE_NEED_MORE) return 0;
    return -1;
}

void turbo_dc_msg_stream_reset(turbo_dc_msg_stream_t *stream) {
    if (stream && stream->ltv_stream) {
        ltv_stream_reset(stream->ltv_stream);
    }
}

void turbo_dc_msg_stream_destroy(turbo_dc_msg_stream_t *stream) {
    if (!stream) return;
    if (stream->ltv_stream) {
        ltv_stream_destroy(stream->ltv_stream);
    }
    free(stream);
}
