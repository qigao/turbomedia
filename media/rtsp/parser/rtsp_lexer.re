#include "rtsp_parser_internal.h"
#include "turbo_rtsp_grammar_gen.h"

#include <string.h>

static const char *rtsp_skip_ows(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) {
        ++p;
    }
    return p;
}

static void rtsp_token_set(
    turbo_rtsp_token_t *token,
    int type,
    const char *value,
    size_t length) {
    token->value = value;
    token->length = length;
    token->method = TURBO_RTSP_METHOD_UNKNOWN;
    (void)type;
}

void turbo_rtsp_lexer_init(
    turbo_rtsp_lexer_t *lexer,
    const char *input,
    size_t length) {
    if (!lexer) {
        return;
    }
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + length;
    lexer->state = TURBO_RTSP_LEX_METHOD;
}

int turbo_rtsp_lexer_next(
    turbo_rtsp_lexer_t *lexer,
    turbo_rtsp_token_t *token) {
    const char *YYCURSOR;
    const char *YYMARKER;
    const char *YYLIMIT;
    const char *token_start;
    const char *line_end;

    if (!lexer || !token) {
        return -1;
    }

    memset(token, 0, sizeof(*token));

    YYCURSOR = lexer->cursor;
    YYMARKER = YYCURSOR;
    YYLIMIT = lexer->limit;
    if (YYCURSOR >= YYLIMIT) {
        lexer->cursor = YYCURSOR;
        return 0;
    }

    switch (lexer->state) {
    case TURBO_RTSP_LEX_METHOD:
        token_start = YYCURSOR;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;
            re2c:eof = 0;

            token = [A-Za-z0-9_$.-]+;

            "OPTIONS" {
                rtsp_token_set(token, RTSP_TOKEN_OPTIONS, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_OPTIONS;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_OPTIONS;
            }
            "DESCRIBE" {
                rtsp_token_set(token, RTSP_TOKEN_DESCRIBE, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_DESCRIBE;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_DESCRIBE;
            }
            "SETUP" {
                rtsp_token_set(token, RTSP_TOKEN_SETUP, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_SETUP;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_SETUP;
            }
            "PLAY" {
                rtsp_token_set(token, RTSP_TOKEN_PLAY, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_PLAY;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_PLAY;
            }
            "TEARDOWN" {
                rtsp_token_set(token, RTSP_TOKEN_TEARDOWN, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_TEARDOWN;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_TEARDOWN;
            }
            "PAUSE" {
                rtsp_token_set(token, RTSP_TOKEN_PAUSE, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_PAUSE;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_PAUSE;
            }
            "ANNOUNCE" {
                rtsp_token_set(token, RTSP_TOKEN_ANNOUNCE, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_ANNOUNCE;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_ANNOUNCE;
            }
            "RECORD" {
                rtsp_token_set(token, RTSP_TOKEN_RECORD, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_RECORD;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_RECORD;
            }
            "GET_PARAMETER" {
                rtsp_token_set(token, RTSP_TOKEN_GET_PARAMETER, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_GET_PARAMETER;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_GET_PARAMETER;
            }
            "SET_PARAMETER" {
                rtsp_token_set(token, RTSP_TOKEN_SET_PARAMETER, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_SET_PARAMETER;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_SET_PARAMETER;
            }
            "REDIRECT" {
                rtsp_token_set(token, RTSP_TOKEN_REDIRECT, token_start, (size_t)(YYCURSOR - token_start));
                token->method = TURBO_RTSP_METHOD_REDIRECT;
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_REDIRECT;
            }
            token {
                rtsp_token_set(token, RTSP_TOKEN_TOKEN, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_URI;
                return RTSP_TOKEN_TOKEN;
            }
            $ { return 0; }
            * { return -1; }
        */

    case TURBO_RTSP_LEX_URI:
        YYCURSOR = rtsp_skip_ows(YYCURSOR, YYLIMIT);
        token_start = YYCURSOR;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;
            re2c:eof = 0;

            uri = [^ \t\r\n]+;

            uri {
                rtsp_token_set(token, RTSP_TOKEN_URI, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_VERSION;
                return RTSP_TOKEN_URI;
            }
            $ { return 0; }
            * { return -1; }
        */

    case TURBO_RTSP_LEX_VERSION:
        YYCURSOR = rtsp_skip_ows(YYCURSOR, YYLIMIT);
        token_start = YYCURSOR;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;
            re2c:eof = 0;

            "RTSP/1.0" {
                rtsp_token_set(token, RTSP_TOKEN_VERSION, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_REQUEST_CRLF;
                return RTSP_TOKEN_VERSION;
            }
            $ { return 0; }
            * { return -1; }
        */

    case TURBO_RTSP_LEX_REQUEST_CRLF:
        token_start = YYCURSOR;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;
            re2c:eof = 0;

            "\r\n" {
                rtsp_token_set(token, RTSP_TOKEN_CRLF, token_start, 2);
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_NAME;
                return RTSP_TOKEN_CRLF;
            }
            $ { return 0; }
            * { return -1; }
        */

    case TURBO_RTSP_LEX_HEADER_NAME:
        token_start = YYCURSOR;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;
            re2c:eof = 0;

            hchar = [A-Za-z0-9!#$%&'*+.^_`|~-];
            hname = hchar+;

            "\r\n" {
                rtsp_token_set(token, RTSP_TOKEN_END_HEADERS, token_start, 2);
                lexer->cursor = YYCURSOR;
                return RTSP_TOKEN_END_HEADERS;
            }
            [Cc][Ss][Ee][Qq] {
                rtsp_token_set(token, RTSP_TOKEN_CSEQ, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_COLON;
                return RTSP_TOKEN_CSEQ;
            }
            [Ss][Ee][Ss][Ss][Ii][Oo][Nn] {
                rtsp_token_set(token, RTSP_TOKEN_SESSION, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_COLON;
                return RTSP_TOKEN_SESSION;
            }
            [Tt][Rr][Aa][Nn][Ss][Pp][Oo][Rr][Tt] {
                rtsp_token_set(token, RTSP_TOKEN_TRANSPORT, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_COLON;
                return RTSP_TOKEN_TRANSPORT;
            }
            [Cc][Oo][Nn][Tt][Ee][Nn][Tt]"-"[Ll][Ee][Nn][Gg][Tt][Hh] {
                rtsp_token_set(token, RTSP_TOKEN_CONTENT_LENGTH, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_COLON;
                return RTSP_TOKEN_CONTENT_LENGTH;
            }
            hname {
                rtsp_token_set(token, RTSP_TOKEN_TOKEN, token_start, (size_t)(YYCURSOR - token_start));
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_COLON;
                return RTSP_TOKEN_TOKEN;
            }
            $ { return 0; }
            * { return -1; }
        */

    case TURBO_RTSP_LEX_HEADER_COLON:
        YYCURSOR = rtsp_skip_ows(YYCURSOR, YYLIMIT);
        if (YYCURSOR >= YYLIMIT || *YYCURSOR != ':') {
            return -1;
        }
        rtsp_token_set(token, RTSP_TOKEN_COLON, YYCURSOR, 1);
        lexer->cursor = YYCURSOR + 1;
        lexer->state = TURBO_RTSP_LEX_HEADER_VALUE;
        return RTSP_TOKEN_COLON;

    case TURBO_RTSP_LEX_HEADER_VALUE:
        YYCURSOR = rtsp_skip_ows(YYCURSOR, YYLIMIT);
        token_start = YYCURSOR;
        line_end = NULL;
        while (YYCURSOR + 1 < YYLIMIT) {
            if (YYCURSOR[0] == '\r' && YYCURSOR[1] == '\n') {
                line_end = YYCURSOR;
                break;
            }
            ++YYCURSOR;
        }
        if (!line_end) {
            return -1;
        }
        while (line_end > token_start &&
               (line_end[-1] == ' ' || line_end[-1] == '\t')) {
            --line_end;
        }
        rtsp_token_set(token, RTSP_TOKEN_HEADER_VALUE, token_start, (size_t)(line_end - token_start));
        lexer->cursor = YYCURSOR;
        lexer->state = TURBO_RTSP_LEX_HEADER_CRLF;
        return RTSP_TOKEN_HEADER_VALUE;

    case TURBO_RTSP_LEX_HEADER_CRLF:
        token_start = YYCURSOR;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;
            re2c:eof = 0;

            "\r\n" {
                rtsp_token_set(token, RTSP_TOKEN_CRLF, token_start, 2);
                lexer->cursor = YYCURSOR;
                lexer->state = TURBO_RTSP_LEX_HEADER_NAME;
                return RTSP_TOKEN_CRLF;
            }
            $ { return 0; }
            * { return -1; }
        */
    }

    return -1;
}
