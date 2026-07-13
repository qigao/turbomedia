#ifndef RTSP_PARSER_INTERNAL_H
#define RTSP_PARSER_INTERNAL_H

#include "turbo_rtsp_parser.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *value;
    size_t length;
    turbo_rtsp_method_t method;
} turbo_rtsp_token_t;

typedef enum {
    TURBO_RTSP_LEX_METHOD = 0,
    TURBO_RTSP_LEX_URI,
    TURBO_RTSP_LEX_VERSION,
    TURBO_RTSP_LEX_REQUEST_CRLF,
    TURBO_RTSP_LEX_HEADER_NAME,
    TURBO_RTSP_LEX_HEADER_COLON,
    TURBO_RTSP_LEX_HEADER_VALUE,
    TURBO_RTSP_LEX_HEADER_CRLF
} turbo_rtsp_lexer_state_t;

typedef struct {
    const char *input;
    const char *cursor;
    const char *limit;
    turbo_rtsp_lexer_state_t state;
} turbo_rtsp_lexer_t;

typedef struct {
    turbo_rtsp_request_t *request;
    turbo_rtsp_message_t *message;
    size_t content_length;
    int has_content_length;
    int complete;
    int error;
} turbo_rtsp_parse_ctx_t;

void turbo_rtsp_lexer_init(
    turbo_rtsp_lexer_t *lexer,
    const char *input,
    size_t length);

int turbo_rtsp_lexer_next(
    turbo_rtsp_lexer_t *lexer,
    turbo_rtsp_token_t *token);

int turbo_rtsp_token_copy(char *dst, size_t dst_size, turbo_rtsp_token_t token);
int turbo_rtsp_token_to_u32(turbo_rtsp_token_t token, uint32_t *value);
int turbo_rtsp_token_to_size(turbo_rtsp_token_t token, size_t *value);
int turbo_rtsp_apply_session(turbo_rtsp_request_t *request, turbo_rtsp_token_t token);
int turbo_rtsp_apply_transport(turbo_rtsp_request_t *request, turbo_rtsp_token_t token);
int turbo_rtsp_apply_header(turbo_rtsp_request_t *request, turbo_rtsp_token_t name, turbo_rtsp_token_t value);
int turbo_rtsp_parse_ctx_add_header(
    turbo_rtsp_parse_ctx_t *ctx,
    turbo_rtsp_token_t name,
    turbo_rtsp_token_t value);

#endif /* TURBO_MEDIA_HAS_RTSP */

#endif /* RTSP_PARSER_INTERNAL_H */
