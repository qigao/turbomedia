/**
 * @file turbo_dc_msg.c
 * @brief LTV message framing for WebRTC DataChannel
 */

#include "turbo_dc_msg.h"
#include <stdlib.h>


/* ============================================================================
 * Send API
 * ============================================================================ */

int turbo_dc_send_ltv(turbo_dc_channel_t *channel, uint8_t type,
                      const void *data, size_t len) {
    if (!channel) return -1;

    size_t wire_size = turbo_ltv_wire_size(len);
    if (wire_size == 0) return -1;

    /* Stack buffer for small messages, heap for large */
    uint8_t stack_buf[256];
    uint8_t *buf = stack_buf;
    int heap_alloc = 0;

    if (wire_size > sizeof(stack_buf)) {
        buf = (uint8_t *)malloc(wire_size);
        if (!buf) return -1;
        heap_alloc = 1;
    }

    size_t written = turbo_ltv_build(type, (const uint8_t *)data, len, buf, wire_size);
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
    if (!data || !out || len == 0) return -1;

    int rc = turbo_parse_ltv((const uint8_t *)data, len, (void**)out);
    return (rc == 0) ? 0 : -1;
}

int turbo_dc_is_ltv(const void *data, size_t len) {
    if (!data || len < 2) return 0;

    uint32_t length;
    size_t header_size;
    
    int rc = turbo_ltv_peek_size((const uint8_t *)data, len,
                                 &length, &header_size);
    /* LtvParseResult: OK=0 assumed from wrapper if mapped correctly? 
       Wrapper: return (int)ltv_peek_size(...)
       ltv_parser.h: LTV_PARSE_OK = 0.
    */
    if (rc != 0) return 0;

    /* ltv_total_size logic was: header_size + length */
    size_t total = header_size + length;
    return (total <= len) ? 1 : 0;
}

/* ============================================================================
 * Stream Parser
 * ============================================================================ */

struct turbo_dc_msg_stream_s {
    turbo_ltv_stream_t *ltv_stream;
};

turbo_dc_msg_stream_t *turbo_dc_msg_stream_create(size_t buffer_size) {
    turbo_dc_msg_stream_t *stream = (turbo_dc_msg_stream_t *)malloc(sizeof(*stream));
    if (!stream) return NULL;

    stream->ltv_stream = turbo_ltv_stream_create(buffer_size);
    if (!stream->ltv_stream) {
        free(stream);
        return NULL;
    }

    return stream;
}

int turbo_dc_msg_stream_feed(turbo_dc_msg_stream_t *stream,
                              const void *data, size_t len,
                              ltv_message_t **out) {
    if (!stream || !data || !out) return -1;

    int rc = turbo_ltv_stream_feed(stream->ltv_stream,
                                   (const uint8_t *)data, len, (void**)out);
    
    /* rc: 0=Complete, 1=Need more, -1=Error */
    if (rc == 0) return 1; /* Complete */
    if (rc == 1) return 0; /* Need more */
    return -1;
}

void turbo_dc_msg_stream_reset(turbo_dc_msg_stream_t *stream) {
    if (stream && stream->ltv_stream) {
        turbo_ltv_stream_reset(stream->ltv_stream);
    }
}

void turbo_dc_msg_stream_destroy(turbo_dc_msg_stream_t *stream) {
    if (!stream) return;
    if (stream->ltv_stream) {
        turbo_ltv_stream_destroy(stream->ltv_stream);
    }
    free(stream);
}
