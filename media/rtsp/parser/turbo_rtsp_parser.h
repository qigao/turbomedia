#ifndef TURBO_RTSP_PARSER_H
#define TURBO_RTSP_PARSER_H

#include "turbo_rtsp.h"
#include "turbo_rtsp_message.h"

#ifdef TURBO_MEDIA_HAS_RTSP

typedef enum {
    TURBO_RTSP_PARSE_ERROR = -1,
    TURBO_RTSP_PARSE_OK = 0,
    TURBO_RTSP_PARSE_PARTIAL = 1
} turbo_rtsp_parse_result_t;

TURBO_MEDIA_C_API int turbo_rtsp_parse_request(
    const char *buffer,
    size_t buffer_len,
    size_t *consumed,
    turbo_rtsp_request_t *request);

TURBO_MEDIA_C_API int turbo_rtsp_parse_response(
    const char *buffer,
    size_t buffer_len,
    size_t *consumed,
    turbo_rtsp_response_view_t *response);

TURBO_MEDIA_C_API int turbo_rtsp_parse_transport(
    const char *transport,
    size_t transport_len,
    turbo_rtsp_transport_spec_t *spec);

TURBO_MEDIA_C_API int turbo_rtsp_parse_rtp_info(
    const char *rtp_info,
    size_t rtp_info_len,
    turbo_rtsp_rtp_info_t *infos,
    size_t info_capacity,
    size_t *info_count);

TURBO_MEDIA_C_API int turbo_rtsp_format_request(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_request_message_t *request);

TURBO_MEDIA_C_API int turbo_rtsp_format_response(
    char *buffer,
    size_t buffer_size,
    uint32_t cseq,
    const char *server_name,
    const turbo_rtsp_response_t *response);

#endif /* TURBO_MEDIA_HAS_RTSP */

#endif /* TURBO_RTSP_PARSER_H */
