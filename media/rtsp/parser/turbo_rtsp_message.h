#ifndef TURBO_RTSP_MESSAGE_H
#define TURBO_RTSP_MESSAGE_H

#include "turbo_rtsp.h"
#include <turbo_export.h>

#ifdef TURBO_MEDIA_HAS_RTSP

typedef struct {
    int status_code;
    const char *reason;
    size_t reason_len;
    turbo_rtsp_header_view_t headers[TURBO_RTSP_MAX_MESSAGE_HEADERS];
    size_t header_count;
    const char *body;
    size_t body_len;
} turbo_rtsp_response_view_t;

CXX_C_API int turbo_rtsp_parse_message(
    const char *buffer,
    size_t buffer_len,
    size_t *consumed,
    turbo_rtsp_message_t *message);

CXX_C_API const turbo_rtsp_header_view_t *turbo_rtsp_message_find_header(
    const turbo_rtsp_message_t *message,
    const char *name);

CXX_C_API const turbo_rtsp_header_view_t *turbo_rtsp_response_find_header(
    const turbo_rtsp_response_view_t *response,
    const char *name);

#endif /* TURBO_MEDIA_HAS_RTSP */

#endif /* TURBO_RTSP_MESSAGE_H */
