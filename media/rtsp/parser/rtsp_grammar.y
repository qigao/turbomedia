%name TurboRtspParse
%token_prefix RTSP_TOKEN_
%token_type {turbo_rtsp_token_t}
%default_type {turbo_rtsp_token_t}
%stack_size 128

%extra_argument {turbo_rtsp_parse_ctx_t *ctx}

%include {
#include "rtsp_parser_internal.h"
#include <string.h>
}

%token OPTIONS DESCRIBE SETUP PLAY TEARDOWN PAUSE ANNOUNCE RECORD GET_PARAMETER SET_PARAMETER REDIRECT TOKEN URI VERSION CSEQ SESSION TRANSPORT CONTENT_LENGTH COLON HEADER_VALUE CRLF END_HEADERS.

%start_symbol message

message ::= request_line headers END_HEADERS. {
    ctx->complete = 1;
}

request_line ::= method(M) URI(U) VERSION CRLF. {
    ctx->request->method = M.method;
    if (turbo_rtsp_token_copy(ctx->request->uri, sizeof(ctx->request->uri), U) != 0) {
        ctx->error = 1;
    }
}

method(A) ::= OPTIONS(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_OPTIONS;
}
method(A) ::= DESCRIBE(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_DESCRIBE;
}
method(A) ::= SETUP(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_SETUP;
}
method(A) ::= PLAY(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_PLAY;
}
method(A) ::= TEARDOWN(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_TEARDOWN;
}
method(A) ::= PAUSE(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_PAUSE;
}
method(A) ::= ANNOUNCE(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_ANNOUNCE;
}
method(A) ::= RECORD(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_RECORD;
}
method(A) ::= GET_PARAMETER(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_GET_PARAMETER;
}
method(A) ::= SET_PARAMETER(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_SET_PARAMETER;
}
method(A) ::= REDIRECT(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_REDIRECT;
}
method(A) ::= TOKEN(T). {
    A = T;
    A.method = TURBO_RTSP_METHOD_UNKNOWN;
}

headers ::= .
headers ::= headers header.

header ::= CSEQ(N) COLON HEADER_VALUE(V) CRLF. {
    if (turbo_rtsp_parse_ctx_add_header(ctx, N, V) != 0) {
        ctx->error = 1;
    }
    if (turbo_rtsp_token_to_u32(V, &ctx->request->cseq) != 0) {
        ctx->error = 1;
    }
}
header ::= SESSION(N) COLON HEADER_VALUE(V) CRLF. {
    if (turbo_rtsp_parse_ctx_add_header(ctx, N, V) != 0) {
        ctx->error = 1;
    }
    if (turbo_rtsp_apply_session(ctx->request, V) != 0) {
        ctx->error = 1;
    }
}
header ::= TRANSPORT(N) COLON HEADER_VALUE(V) CRLF. {
    if (turbo_rtsp_parse_ctx_add_header(ctx, N, V) != 0) {
        ctx->error = 1;
    }
    if (turbo_rtsp_apply_transport(ctx->request, V) != 0) {
        ctx->error = 1;
    }
}
header ::= CONTENT_LENGTH(N) COLON HEADER_VALUE(V) CRLF. {
    size_t content_length = 0;
    if (turbo_rtsp_parse_ctx_add_header(ctx, N, V) != 0) {
        ctx->error = 1;
    }
    if (turbo_rtsp_token_to_size(V, &content_length) != 0) {
        ctx->error = 1;
    } else if (ctx->has_content_length && ctx->content_length != content_length) {
        ctx->error = 1;
    } else {
        ctx->content_length = content_length;
        ctx->has_content_length = 1;
    }
}
header ::= TOKEN(N) COLON HEADER_VALUE(V) CRLF. {
    if (turbo_rtsp_parse_ctx_add_header(ctx, N, V) != 0) {
        ctx->error = 1;
    }
    if (turbo_rtsp_apply_header(ctx->request, N, V) != 0) {
        ctx->error = 1;
    }
}

%syntax_error {
    ctx->error = 1;
}

%parse_failure {
    ctx->error = 1;
}

%stack_overflow {
    ctx->error = 1;
}
