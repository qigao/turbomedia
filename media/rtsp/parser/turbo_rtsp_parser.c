#include "turbo_rtsp_parser.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include "rtsp_parser_internal.h"
#include "rtsp_grammar_gen.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *TurboRtspParseAlloc(void *(*mallocProc)(size_t));
void TurboRtspParse(void *parser, int token_id, turbo_rtsp_token_t token, turbo_rtsp_parse_ctx_t *ctx);
void TurboRtspParseFree(void *parser, void (*freeProc)(void *));

static size_t turbo_rtsp_body_len(const turbo_rtsp_response_t *response) {
    if (!response || !response->body) {
        return 0;
    }
    return response->body_len != 0 ? response->body_len : strlen(response->body);
}

static const char *turbo_rtsp_reason_phrase(int status_code) {
    switch (status_code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 454:
        return "Session Not Found";
    case 455:
        return "Method Not Valid in This State";
    case 461:
        return "Unsupported Transport";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "OK";
    }
}

static const char *turbo_rtsp_method_name(turbo_rtsp_method_t method) {
    switch (method) {
    case TURBO_RTSP_METHOD_OPTIONS:
        return "OPTIONS";
    case TURBO_RTSP_METHOD_DESCRIBE:
        return "DESCRIBE";
    case TURBO_RTSP_METHOD_SETUP:
        return "SETUP";
    case TURBO_RTSP_METHOD_PLAY:
        return "PLAY";
    case TURBO_RTSP_METHOD_TEARDOWN:
        return "TEARDOWN";
    case TURBO_RTSP_METHOD_PAUSE:
        return "PAUSE";
    case TURBO_RTSP_METHOD_ANNOUNCE:
        return "ANNOUNCE";
    case TURBO_RTSP_METHOD_RECORD:
        return "RECORD";
    case TURBO_RTSP_METHOD_GET_PARAMETER:
        return "GET_PARAMETER";
    case TURBO_RTSP_METHOD_SET_PARAMETER:
        return "SET_PARAMETER";
    case TURBO_RTSP_METHOD_REDIRECT:
        return "REDIRECT";
    case TURBO_RTSP_METHOD_UNKNOWN:
    default:
        return NULL;
    }
}

static int turbo_rtsp_vappendf(
    char **cursor,
    size_t *remaining,
    const char *fmt,
    va_list ap) {
    va_list ap_copy;
    int needed = 0;

    if (!cursor || !*cursor || !remaining || !fmt) {
        return -1;
    }

    va_copy(ap_copy, ap);
    needed = vsnprintf(*cursor, *remaining, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0 || (size_t)needed >= *remaining) {
        return -1;
    }

    *cursor += needed;
    *remaining -= (size_t)needed;
    return 0;
}

static int turbo_rtsp_appendf(
    char **cursor,
    size_t *remaining,
    const char *fmt,
    ...) {
    va_list ap;
    int rc = 0;

    va_start(ap, fmt);
    rc = turbo_rtsp_vappendf(cursor, remaining, fmt, ap);
    va_end(ap);
    return rc;
}

static int turbo_rtsp_append_bytes(
    char **cursor,
    size_t *remaining,
    const char *data,
    size_t len) {
    if (!cursor || !*cursor || !remaining || !data || len >= *remaining) {
        return -1;
    }
    memcpy(*cursor, data, len);
    *cursor += len;
    *remaining -= len;
    **cursor = '\0';
    return 0;
}

static int turbo_rtsp_token_case_eq(turbo_rtsp_token_t token, const char *text) {
    size_t i = 0;
    const size_t len = strlen(text);

    if (token.length != len) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        if (tolower((unsigned char)token.value[i]) !=
            tolower((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

static int turbo_rtsp_cstr_case_eq(const char *left, const char *right) {
    size_t i = 0;

    if (!left || !right) {
        return 0;
    }
    for (i = 0; left[i] != '\0' && right[i] != '\0'; ++i) {
        if (tolower((unsigned char)left[i]) !=
            tolower((unsigned char)right[i])) {
            return 0;
        }
    }
    return left[i] == '\0' && right[i] == '\0';
}

static int turbo_rtsp_is_managed_response_header(const char *name) {
    return turbo_rtsp_cstr_case_eq(name, "CSeq") ||
           turbo_rtsp_cstr_case_eq(name, "Server") ||
           turbo_rtsp_cstr_case_eq(name, "Public") ||
           turbo_rtsp_cstr_case_eq(name, "Session") ||
           turbo_rtsp_cstr_case_eq(name, "Transport") ||
           turbo_rtsp_cstr_case_eq(name, "Range") ||
           turbo_rtsp_cstr_case_eq(name, "RTP-Info") ||
           turbo_rtsp_cstr_case_eq(name, "Content-Type") ||
           turbo_rtsp_cstr_case_eq(name, "Content-Length");
}

static int turbo_rtsp_is_managed_request_header(const char *name) {
    return turbo_rtsp_cstr_case_eq(name, "CSeq") ||
           turbo_rtsp_cstr_case_eq(name, "User-Agent") ||
           turbo_rtsp_cstr_case_eq(name, "Session") ||
           turbo_rtsp_cstr_case_eq(name, "Transport") ||
           turbo_rtsp_cstr_case_eq(name, "Range") ||
           turbo_rtsp_cstr_case_eq(name, "Content-Type") ||
           turbo_rtsp_cstr_case_eq(name, "Content-Length");
}

static int turbo_rtsp_view_case_eq(
    const char *value,
    size_t value_len,
    const char *text) {
    size_t i = 0;
    const size_t text_len = strlen(text);

    if (!value || !text || value_len != text_len) {
        return 0;
    }
    for (i = 0; i < value_len; ++i) {
        if (tolower((unsigned char)value[i]) !=
            tolower((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

static const char *turbo_rtsp_token_find_case(
    turbo_rtsp_token_t token,
    const char *needle) {
    const size_t needle_len = strlen(needle);
    size_t i = 0;

    if (needle_len == 0 || token.length < needle_len) {
        return NULL;
    }

    for (i = 0; i + needle_len <= token.length; ++i) {
        turbo_rtsp_token_t part;
        part.value = token.value + i;
        part.length = needle_len;
        part.method = TURBO_RTSP_METHOD_UNKNOWN;
        if (turbo_rtsp_token_case_eq(part, needle)) {
            return token.value + i;
        }
    }
    return NULL;
}

static const char *turbo_rtsp_param_end(const char *start, const char *value_end) {
    const char *end = start;

    while (end < value_end && *end != ';' && *end != ',' && *end != '\r' && *end != '\n') {
        ++end;
    }
    return end;
}

static int turbo_rtsp_parse_transport_profile(
    const char *start,
    const char *value_end,
    turbo_rtsp_transport_t *kind,
    const char **params_start) {
    turbo_rtsp_token_t profile;
    const char *profile_end = NULL;

    if (!start || !value_end || !kind || !params_start || start >= value_end) {
        return -1;
    }

    profile_end = turbo_rtsp_param_end(start, value_end);
    profile.value = start;
    profile.length = (size_t)(profile_end - start);
    profile.method = TURBO_RTSP_METHOD_UNKNOWN;

    if (turbo_rtsp_token_case_eq(profile, "RTP/AVP") ||
        turbo_rtsp_token_case_eq(profile, "RTP/AVP/UDP")) {
        *kind = TURBO_RTSP_TRANSPORT_RTP_AVP_UDP;
    } else if (turbo_rtsp_token_case_eq(profile, "RTP/AVP/TCP")) {
        *kind = TURBO_RTSP_TRANSPORT_RTP_AVP_TCP;
    } else {
        return -1;
    }

    *params_start = profile_end < value_end && *profile_end == ';'
                        ? profile_end + 1
                        : profile_end;
    return 0;
}

static int turbo_rtsp_parse_uint16_pair_or_single(
    const char *start,
    const char *end,
    int *first,
    int *second) {
    long a = 0;
    long b = 0;
    char *tail = NULL;

    if (!start || start >= end || !first || !second) {
        return -1;
    }

    a = strtol(start, &tail, 10);
    if (tail == start || tail > end || a < 0 || a > 65535) {
        return -1;
    }
    if (tail == end) {
        a = (a / 2) * 2;
        *first = (int)a;
        *second = (int)a + 1;
        return 0;
    }
    if (*tail != '-') {
        return -1;
    }

    b = strtol(tail + 1, &tail, 10);
    if (tail == start || tail != end || b < 0 || b > 65535) {
        return -1;
    }

    *first = (int)a;
    *second = (int)b;
    return 0;
}

static int turbo_rtsp_parse_channel_pair_or_single(
    const char *start,
    const char *end,
    int *first,
    int *second) {
    long a = 0;
    long b = 0;
    char *tail = NULL;

    if (!start || start >= end || !first || !second) {
        return -1;
    }

    a = strtol(start, &tail, 10);
    if (tail == start || tail > end || a < 0 || a > 255) {
        return -1;
    }
    if (tail == end) {
        *first = (int)a;
        *second = (int)a + 1;
        return *second <= 255 ? 0 : -1;
    }
    if (*tail != '-') {
        return -1;
    }

    b = strtol(tail + 1, &tail, 10);
    if (tail == start || tail != end || b < 0 || b > 255) {
        return -1;
    }

    *first = (int)a;
    *second = (int)b;
    return 0;
}

static int turbo_rtsp_parse_int_value(
    const char *start,
    const char *end,
    int *value) {
    long parsed = 0;
    char *tail = NULL;

    if (!start || start >= end || !value) {
        return -1;
    }

    parsed = strtol(start, &tail, 10);
    if (tail == start || tail != end || parsed < 0 || parsed > 2147483647L) {
        return -1;
    }

    *value = (int)parsed;
    return 0;
}

static int turbo_rtsp_parse_u32_value(
    const char *start,
    const char *end,
    uint32_t *value) {
    uint64_t parsed = 0;
    const char *cursor = start;

    if (!start || start >= end || !value) {
        return -1;
    }

    while (cursor < end) {
        unsigned char c = (unsigned char)*cursor;
        if (c < '0' || c > '9') {
            return -1;
        }
        parsed = parsed * 10u + (uint64_t)(c - '0');
        if (parsed > 0xffffffffull) {
            return -1;
        }
        ++cursor;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static int turbo_rtsp_parse_hex_u32(
    const char *start,
    const char *end,
    uint32_t *value) {
    uint32_t parsed = 0;

    if (!start || start >= end || !value) {
        return -1;
    }
    for (; start < end; ++start) {
        unsigned char c = (unsigned char)*start;
        uint32_t digit = 0;

        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = 10u + (uint32_t)(c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            digit = 10u + (uint32_t)(c - 'A');
        } else {
            return -1;
        }
        parsed = (parsed << 4) | digit;
    }

    *value = parsed;
    return 0;
}

static int turbo_rtsp_copy_range(
    char *dst,
    size_t dst_size,
    const char *start,
    const char *end) {
    size_t len = 0;

    if (!dst || dst_size == 0 || !start || start > end) {
        return -1;
    }
    len = (size_t)(end - start);
    if (len >= dst_size) {
        return -1;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
    return 0;
}

static int turbo_rtsp_token_starts_with_case(turbo_rtsp_token_t token, const char *prefix) {
    turbo_rtsp_token_t part;
    const size_t prefix_len = strlen(prefix);

    if (!token.value || token.length < prefix_len) {
        return 0;
    }
    part.value = token.value;
    part.length = prefix_len;
    part.method = TURBO_RTSP_METHOD_UNKNOWN;
    return turbo_rtsp_token_case_eq(part, prefix);
}

static int turbo_rtsp_range_starts_with_case(const char *start, const char *end, const char *prefix) {
    turbo_rtsp_token_t part;
    const size_t prefix_len = strlen(prefix);

    if (!start || !end || start > end || (size_t)(end - start) < prefix_len) {
        return 0;
    }
    part.value = start;
    part.length = prefix_len;
    part.method = TURBO_RTSP_METHOD_UNKNOWN;
    return turbo_rtsp_token_case_eq(part, prefix);
}

static int turbo_rtsp_parse_range_digits(
    const char **cursor,
    const char *end,
    size_t min_digits,
    size_t max_digits,
    int64_t max_value,
    int64_t *value) {
    const char *p = NULL;
    size_t digits = 0;
    int64_t parsed = 0;

    if (!cursor || !*cursor || !end || *cursor > end || !value || min_digits == 0) {
        return -1;
    }
    p = *cursor;
    while (p < end && *p >= '0' && *p <= '9' && (max_digits == 0 || digits < max_digits)) {
        int64_t digit = (int64_t)(*p - '0');
        if (parsed > (max_value - digit) / 10) {
            return -1;
        }
        parsed = parsed * 10 + digit;
        ++digits;
        ++p;
    }
    if (digits < min_digits) {
        return -1;
    }

    *cursor = p;
    *value = parsed;
    return 0;
}

static int turbo_rtsp_parse_range_fraction_ms(
    const char **cursor,
    const char *end,
    int64_t *fraction_ms) {
    const char *p = NULL;
    int64_t parsed = 0;
    int digits = 0;

    if (!cursor || !*cursor || !end || *cursor > end || !fraction_ms) {
        return -1;
    }
    p = *cursor;
    while (p < end && *p >= '0' && *p <= '9') {
        if (digits < 3) {
            parsed = parsed * 10 + (int64_t)(*p - '0');
            ++digits;
        }
        ++p;
    }
    while (digits < 3) {
        parsed *= 10;
        ++digits;
    }

    *cursor = p;
    *fraction_ms = parsed;
    return 0;
}

static int turbo_rtsp_parse_npt_time(
    const char *start,
    const char *end,
    int64_t *ms,
    int *is_now) {
    const char *cursor = start;
    int64_t first = 0;
    int64_t seconds = 0;
    int64_t fraction_ms = 0;

    if (!start || start >= end || !ms || !is_now) {
        return -1;
    }

    *is_now = 0;
    if ((size_t)(end - start) == 3) {
        turbo_rtsp_token_t token;
        token.value = start;
        token.length = 3;
        token.method = TURBO_RTSP_METHOD_UNKNOWN;
        if (turbo_rtsp_token_case_eq(token, "now")) {
            *ms = 0;
            *is_now = 1;
            return 0;
        }
    }

    if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 0, INT64_MAX, &first) != 0) {
        return -1;
    }
    if (cursor < end && *cursor == ':') {
        int64_t minutes = 0;
        int64_t npt_seconds = 0;

        ++cursor;
        if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 2, 59, &minutes) != 0 ||
            cursor >= end || *cursor != ':') {
            return -1;
        }
        ++cursor;
        if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 2, 59, &npt_seconds) != 0) {
            return -1;
        }
        if (first > (INT64_MAX / 3600) ||
            first * 3600 > INT64_MAX - minutes * 60 - npt_seconds) {
            return -1;
        }
        seconds = first * 3600 + minutes * 60 + npt_seconds;
    } else {
        seconds = first;
    }
    if (cursor < end && *cursor == '.') {
        ++cursor;
        if (turbo_rtsp_parse_range_fraction_ms(&cursor, end, &fraction_ms) != 0) {
            return -1;
        }
    }
    if (cursor != end || seconds > (INT64_MAX - fraction_ms) / 1000) {
        return -1;
    }

    *ms = seconds * 1000 + fraction_ms;
    return 0;
}

static int turbo_rtsp_parse_smpte_time(
    const char *start,
    const char *end,
    int frame_rate,
    int64_t *ms) {
    const char *cursor = start;
    int64_t hours = 0;
    int64_t minutes = 0;
    int64_t seconds = 0;
    int64_t frames = 0;

    if (!start || start >= end || !ms || frame_rate <= 0) {
        return -1;
    }
    if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 0, INT64_MAX, &hours) != 0 ||
        cursor >= end || *cursor != ':') {
        return -1;
    }
    ++cursor;
    if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 2, 59, &minutes) != 0 ||
        cursor >= end || *cursor != ':') {
        return -1;
    }
    ++cursor;
    if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 2, 59, &seconds) != 0) {
        return -1;
    }
    if (cursor < end && *cursor == ':') {
        ++cursor;
        if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 2, frame_rate - 1, &frames) != 0) {
            return -1;
        }
    }
    if (cursor < end && *cursor == '.') {
        int64_t subframes = 0;

        ++cursor;
        if (turbo_rtsp_parse_range_digits(&cursor, end, 1, 2, 99, &subframes) != 0) {
            return -1;
        }
    }
    if (cursor != end ||
        hours > (INT64_MAX / 3600) ||
        hours * 3600 > INT64_MAX - minutes * 60 - seconds) {
        return -1;
    }

    *ms = (hours * 3600 + minutes * 60 + seconds) * 1000 + frames * 1000 / frame_rate;
    return 0;
}

static int turbo_rtsp_is_leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int turbo_rtsp_days_in_month(int64_t year, int64_t month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && turbo_rtsp_is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static int64_t turbo_rtsp_days_from_civil(int64_t year, int64_t month, int64_t day) {
    int64_t era = 0;
    uint64_t yoe = 0;
    uint64_t doy = 0;
    uint64_t doe = 0;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    yoe = (uint64_t)(year - era * 400);
    doy = (uint64_t)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int turbo_rtsp_parse_clock_time(
    const char *start,
    const char *end,
    int64_t *ms) {
    const char *cursor = start;
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    int64_t hour = 0;
    int64_t minute = 0;
    int64_t second = 0;
    int64_t fraction_ms = 0;
    int64_t days = 0;

    if (!start || start >= end || !ms) {
        return -1;
    }
    if (turbo_rtsp_parse_range_digits(&cursor, end, 4, 4, 9999, &year) != 0 ||
        turbo_rtsp_parse_range_digits(&cursor, end, 2, 2, 12, &month) != 0 ||
        turbo_rtsp_parse_range_digits(&cursor, end, 2, 2, 31, &day) != 0 ||
        cursor >= end ||
        (*cursor != 'T' && *cursor != 't')) {
        return -1;
    }
    ++cursor;
    if (turbo_rtsp_parse_range_digits(&cursor, end, 2, 2, 23, &hour) != 0 ||
        turbo_rtsp_parse_range_digits(&cursor, end, 2, 2, 59, &minute) != 0) {
        return -1;
    }
    if (cursor < end && *cursor >= '0' && *cursor <= '9') {
        if (turbo_rtsp_parse_range_digits(&cursor, end, 2, 2, 60, &second) != 0) {
            return -1;
        }
    }
    if (cursor < end && *cursor == '.') {
        ++cursor;
        if (turbo_rtsp_parse_range_fraction_ms(&cursor, end, &fraction_ms) != 0) {
            return -1;
        }
    }
    if (cursor < end && (*cursor == 'Z' || *cursor == 'z')) {
        ++cursor;
    }
    if (cursor != end ||
        day < 1 ||
        day > turbo_rtsp_days_in_month(year, month)) {
        return -1;
    }

    days = turbo_rtsp_days_from_civil(year, month, day);
    if (days > INT64_MAX / 86400000LL || days < INT64_MIN / 86400000LL) {
        return -1;
    }
    *ms = days * 86400000LL +
          hour * 3600000LL +
          minute * 60000LL +
          second * 1000LL +
          fraction_ms;
    return 0;
}

static int turbo_rtsp_parse_range_parameter_time(
    const char *start,
    const char *end,
    turbo_rtsp_range_header_t *range) {
    const char *cursor = start;

    if (!start || !end || start > end || !range) {
        return -1;
    }
    while (cursor < end) {
        const char *param_start = cursor;
        const char *param_end = NULL;
        const char *equals = NULL;
        turbo_rtsp_token_t key;

        if (*cursor != ';') {
            break;
        }
        param_start = cursor + 1;
        param_end = turbo_rtsp_param_end(param_start, end);
        equals = param_start;
        while (equals < param_end && *equals != '=') {
            ++equals;
        }
        if (equals < param_end) {
            key.value = param_start;
            key.length = (size_t)(equals - param_start);
            key.method = TURBO_RTSP_METHOD_UNKNOWN;
            if (turbo_rtsp_token_case_eq(key, "time")) {
                if (turbo_rtsp_parse_clock_time(equals + 1, param_end, &range->time_ms) != 0) {
                    return -1;
                }
                range->has_time = 1;
            }
        }
        cursor = param_end;
    }
    return 0;
}

static int turbo_rtsp_parse_range_values(
    const char *start,
    const char *end,
    turbo_rtsp_range_t type,
    int frame_rate,
    turbo_rtsp_range_header_t *range) {
    const char *dash = start;

    if (!start || !end || start >= end || !range) {
        return -1;
    }
    while (dash < end && *dash != '-') {
        ++dash;
    }
    if (dash >= end) {
        return -1;
    }

    range->type = type;
    if (dash > start) {
        if (type == TURBO_RTSP_RANGE_NPT) {
            if (turbo_rtsp_parse_npt_time(start, dash, &range->start_ms, &range->start_is_now) != 0) {
                return -1;
            }
        } else if (type == TURBO_RTSP_RANGE_CLOCK) {
            if (turbo_rtsp_parse_clock_time(start, dash, &range->start_ms) != 0) {
                return -1;
            }
        } else {
            if (turbo_rtsp_parse_smpte_time(start, dash, frame_rate, &range->start_ms) != 0) {
                return -1;
            }
        }
        range->has_start = 1;
    } else if (type != TURBO_RTSP_RANGE_NPT) {
        return -1;
    }

    if (dash + 1 < end) {
        int is_now = 0;

        if (type == TURBO_RTSP_RANGE_NPT) {
            if (turbo_rtsp_parse_npt_time(dash + 1, end, &range->end_ms, &is_now) != 0 ||
                is_now) {
                return -1;
            }
        } else if (type == TURBO_RTSP_RANGE_CLOCK) {
            if (turbo_rtsp_parse_clock_time(dash + 1, end, &range->end_ms) != 0) {
                return -1;
            }
        } else {
            if (turbo_rtsp_parse_smpte_time(dash + 1, end, frame_rate, &range->end_ms) != 0) {
                return -1;
            }
        }
        range->has_end = 1;
    }
    return 0;
}

static int turbo_rtsp_apply_range(turbo_rtsp_request_t *request, turbo_rtsp_token_t token) {
    const char *end = NULL;
    const char *item_end = NULL;
    const char *range_end = NULL;
    const char *params = NULL;
    const char *value = NULL;
    turbo_rtsp_range_header_t parsed_range;

    if (!request || !token.value) {
        return -1;
    }
    memset(&parsed_range, 0, sizeof(parsed_range));
    if (turbo_rtsp_token_copy(request->range, sizeof(request->range), token) != 0 ||
        turbo_rtsp_token_copy(parsed_range.value, sizeof(parsed_range.value), token) != 0) {
        return -1;
    }

    value = token.value;
    end = token.value + token.length;
    while (value < end && isspace((unsigned char)*value)) {
        ++value;
    }
    while (end > value && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    item_end = value;
    while (item_end < end && *item_end != ',') {
        ++item_end;
    }
    range_end = value;
    while (range_end < item_end && *range_end != ';') {
        ++range_end;
    }
    params = range_end;

    if (turbo_rtsp_range_starts_with_case(value, range_end, "npt=")) {
        if (turbo_rtsp_parse_range_values(
                value + strlen("npt="),
                range_end,
                TURBO_RTSP_RANGE_NPT,
                0,
                &parsed_range) != 0) {
            return -1;
        }
    } else if (turbo_rtsp_range_starts_with_case(value, range_end, "clock=")) {
        if (turbo_rtsp_parse_range_values(
                value + strlen("clock="),
                range_end,
                TURBO_RTSP_RANGE_CLOCK,
                0,
                &parsed_range) != 0) {
            return -1;
        }
    } else if (turbo_rtsp_range_starts_with_case(value, range_end, "smpte-30-drop=")) {
        if (turbo_rtsp_parse_range_values(
                value + strlen("smpte-30-drop="),
                range_end,
                TURBO_RTSP_RANGE_SMPTE_30_DROP,
                30,
                &parsed_range) != 0) {
            return -1;
        }
    } else if (turbo_rtsp_range_starts_with_case(value, range_end, "smpte-25=")) {
        if (turbo_rtsp_parse_range_values(
                value + strlen("smpte-25="),
                range_end,
                TURBO_RTSP_RANGE_SMPTE_25,
                25,
                &parsed_range) != 0) {
            return -1;
        }
    } else if (turbo_rtsp_range_starts_with_case(value, range_end, "smpte=")) {
        if (turbo_rtsp_parse_range_values(
                value + strlen("smpte="),
                range_end,
                TURBO_RTSP_RANGE_SMPTE,
                30,
                &parsed_range) != 0) {
            return -1;
        }
    } else {
        request->range_spec = parsed_range;
        return 0;
    }

    if (params < item_end &&
        turbo_rtsp_parse_range_parameter_time(params, item_end, &parsed_range) != 0) {
        return -1;
    }
    request->range_spec = parsed_range;
    return 0;
}

static int turbo_rtsp_parse_mode(
    const char *start,
    const char *end,
    turbo_rtsp_transport_mode_t *mode) {
    turbo_rtsp_token_t token;

    if (!start || start >= end || !mode) {
        return -1;
    }
    if (*start == '"' && end > start + 1 && *(end - 1) == '"') {
        ++start;
        --end;
    }
    token.value = start;
    token.length = (size_t)(end - start);
    token.method = TURBO_RTSP_METHOD_UNKNOWN;

    if (turbo_rtsp_token_case_eq(token, "PLAY")) {
        *mode = TURBO_RTSP_TRANSPORT_MODE_PLAY;
        return 0;
    }
    if (turbo_rtsp_token_case_eq(token, "RECORD")) {
        *mode = TURBO_RTSP_TRANSPORT_MODE_RECORD;
        return 0;
    }
    return -1;
}

int turbo_rtsp_apply_session(turbo_rtsp_request_t *request, turbo_rtsp_token_t token) {
    const char *value_end = NULL;
    const char *param = NULL;
    const char *id_end = NULL;

    if (!request || !token.value) {
        return -1;
    }

    value_end = token.value + token.length;
    id_end = turbo_rtsp_param_end(token.value, value_end);
    if (turbo_rtsp_copy_range(request->session_id, sizeof(request->session_id), token.value, id_end) != 0 ||
        turbo_rtsp_copy_range(request->session.id, sizeof(request->session.id), token.value, id_end) != 0) {
        return -1;
    }

    param = id_end;
    while (param < value_end) {
        const char *param_end = NULL;
        turbo_rtsp_token_t param_token;

        if (*param == ';') {
            ++param;
        }
        param_end = turbo_rtsp_param_end(param, value_end);
        param_token.value = param;
        param_token.length = (size_t)(param_end - param);
        param_token.method = TURBO_RTSP_METHOD_UNKNOWN;

        if (param_token.length > strlen("timeout=") &&
            turbo_rtsp_token_find_case(param_token, "timeout=") == param_token.value) {
            const char *start = param_token.value + strlen("timeout=");
            if (turbo_rtsp_parse_int_value(start, param_end, &request->session.timeout_seconds) != 0) {
                return -1;
            }
            request->session.has_timeout = 1;
        }
        if (param_end >= value_end) {
            break;
        }
        param = param_end + 1;
    }
    return 0;
}

static int turbo_rtsp_find_header_end(
    const char *buffer,
    size_t buffer_len,
    size_t *end_offset) {
    size_t i = 0;

    if (!buffer || !end_offset) {
        return -1;
    }

    for (i = 0; i + 3 < buffer_len; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            *end_offset = i + 4;
            return 0;
        }
    }
    return 1;
}

int turbo_rtsp_token_copy(char *dst, size_t dst_size, turbo_rtsp_token_t token) {
    size_t len = 0;

    if (!dst || dst_size == 0 || !token.value) {
        return -1;
    }

    len = token.length;
    if (len >= dst_size) {
        return -1;
    }

    memcpy(dst, token.value, len);
    dst[len] = '\0';
    return 0;
}

int turbo_rtsp_token_to_u32(turbo_rtsp_token_t token, uint32_t *value) {
    uint32_t parsed = 0;
    size_t i = 0;

    if (!token.value || token.length == 0 || !value) {
        return -1;
    }

    for (i = 0; i < token.length; ++i) {
        unsigned char c = (unsigned char)token.value[i];
        uint32_t digit = 0;

        if (c < '0' || c > '9') {
            return -1;
        }
        digit = (uint32_t)(c - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) {
            return -1;
        }
        parsed = parsed * 10u + digit;
    }

    *value = parsed;
    return 0;
}

int turbo_rtsp_token_to_size(turbo_rtsp_token_t token, size_t *value) {
    size_t parsed = 0;
    size_t i = 0;

    if (!token.value || token.length == 0 || !value) {
        return -1;
    }

    for (i = 0; i < token.length; ++i) {
        unsigned char c = (unsigned char)token.value[i];
        size_t digit = 0;

        if (c < '0' || c > '9') {
            return -1;
        }
        digit = (size_t)(c - '0');
        if (parsed > (((size_t)-1) - digit) / 10u) {
            return -1;
        }
        parsed = parsed * 10u + digit;
    }

    *value = parsed;
    return 0;
}

int turbo_rtsp_apply_transport(turbo_rtsp_request_t *request, turbo_rtsp_token_t token) {
    const char *value_end = NULL;
    const char *cursor = NULL;

    if (!request || !token.value) {
        return -1;
    }

    request->transport_spec.client_rtp_port = -1;
    request->transport_spec.client_rtcp_port = -1;
    request->transport_spec.server_rtp_port = -1;
    request->transport_spec.server_rtcp_port = -1;
    request->transport_spec.multicast_rtp_port = -1;
    request->transport_spec.multicast_rtcp_port = -1;
    request->transport_spec.interleaved_rtp_channel = -1;
    request->transport_spec.interleaved_rtcp_channel = -1;
    request->transport_spec.ttl = -1;
    request->transport_spec.layers = -1;

    if (turbo_rtsp_token_copy(request->transport, sizeof(request->transport), token) != 0) {
        return -1;
    }

    value_end = token.value + token.length;
    if (turbo_rtsp_parse_transport_profile(
            token.value,
            value_end,
            &request->transport_kind,
            &cursor) != 0) {
        return -1;
    }
    request->transport_spec.kind = request->transport_kind;
    request->transport_spec.delivery = TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST;

    while (cursor < value_end) {
        const char *param_end = turbo_rtsp_param_end(cursor, value_end);
        turbo_rtsp_token_t param;

        param.value = cursor;
        param.length = (size_t)(param_end - cursor);
        param.method = TURBO_RTSP_METHOD_UNKNOWN;

        if (param.length == 7 && turbo_rtsp_token_case_eq(param, "unicast")) {
            request->transport_spec.delivery = TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST;
        } else if (param.length == 9 && turbo_rtsp_token_case_eq(param, "multicast")) {
            request->transport_spec.delivery = TURBO_RTSP_TRANSPORT_DELIVERY_MULTICAST;
        } else if (param.length > strlen("client_port=") &&
                   turbo_rtsp_token_find_case(param, "client_port=") == param.value) {
            const char *start = param.value + strlen("client_port=");
            if (turbo_rtsp_parse_uint16_pair_or_single(
                    start,
                    param_end,
                    &request->transport_spec.client_rtp_port,
                    &request->transport_spec.client_rtcp_port) != 0) {
                return -1;
            }
        } else if (param.length > strlen("server_port=") &&
                   turbo_rtsp_token_find_case(param, "server_port=") == param.value) {
            const char *start = param.value + strlen("server_port=");
            if (turbo_rtsp_parse_uint16_pair_or_single(
                    start,
                    param_end,
                    &request->transport_spec.server_rtp_port,
                    &request->transport_spec.server_rtcp_port) != 0) {
                return -1;
            }
        } else if (param.length > strlen("port=") &&
                   turbo_rtsp_token_find_case(param, "port=") == param.value) {
            const char *start = param.value + strlen("port=");
            if (turbo_rtsp_parse_uint16_pair_or_single(
                    start,
                    param_end,
                    &request->transport_spec.multicast_rtp_port,
                    &request->transport_spec.multicast_rtcp_port) != 0) {
                return -1;
            }
        } else if (param.length > strlen("interleaved=") &&
                   turbo_rtsp_token_find_case(param, "interleaved=") == param.value) {
            const char *start = param.value + strlen("interleaved=");
            if (turbo_rtsp_parse_channel_pair_or_single(
                    start,
                    param_end,
                    &request->transport_spec.interleaved_rtp_channel,
                    &request->transport_spec.interleaved_rtcp_channel) != 0) {
                return -1;
            }
        } else if (param.length > strlen("destination=") &&
                   turbo_rtsp_token_find_case(param, "destination=") == param.value) {
            const char *start = param.value + strlen("destination=");
            if (turbo_rtsp_copy_range(
                    request->transport_spec.destination,
                    sizeof(request->transport_spec.destination),
                    start,
                    param_end) != 0) {
                return -1;
            }
        } else if (param.length > strlen("source=") &&
                   turbo_rtsp_token_find_case(param, "source=") == param.value) {
            const char *start = param.value + strlen("source=");
            if (turbo_rtsp_copy_range(
                    request->transport_spec.source,
                    sizeof(request->transport_spec.source),
                    start,
                    param_end) != 0) {
                return -1;
            }
        } else if (param.length > strlen("ttl=") &&
                   turbo_rtsp_token_find_case(param, "ttl=") == param.value) {
            const char *start = param.value + strlen("ttl=");
            if (turbo_rtsp_parse_int_value(start, param_end, &request->transport_spec.ttl) != 0) {
                return -1;
            }
        } else if (param.length > strlen("layers=") &&
                   turbo_rtsp_token_find_case(param, "layers=") == param.value) {
            const char *start = param.value + strlen("layers=");
            if (turbo_rtsp_parse_int_value(start, param_end, &request->transport_spec.layers) != 0) {
                return -1;
            }
        } else if (param.length > strlen("ssrc=") &&
                   turbo_rtsp_token_find_case(param, "ssrc=") == param.value) {
            const char *start = param.value + strlen("ssrc=");
            if (turbo_rtsp_parse_hex_u32(start, param_end, &request->transport_spec.ssrc) != 0) {
                return -1;
            }
            request->transport_spec.has_ssrc = 1;
        } else if (param.length > strlen("mode=") &&
                   turbo_rtsp_token_find_case(param, "mode=") == param.value) {
            const char *start = param.value + strlen("mode=");
            if (turbo_rtsp_parse_mode(start, param_end, &request->transport_spec.mode) != 0) {
                return -1;
            }
        } else if (param.length == strlen("append") &&
                   turbo_rtsp_token_case_eq(param, "append")) {
            request->transport_spec.append = 1;
        }

        if (param_end >= value_end || *param_end == ',') {
            break;
        }
        cursor = param_end + 1;
    }

    request->client_rtp_port = request->transport_spec.client_rtp_port;
    request->client_rtcp_port = request->transport_spec.client_rtcp_port;
    request->interleaved_rtp_channel = request->transport_spec.interleaved_rtp_channel;
    request->interleaved_rtcp_channel = request->transport_spec.interleaved_rtcp_channel;
    return 0;
}

int turbo_rtsp_apply_header(turbo_rtsp_request_t *request, turbo_rtsp_token_t name, turbo_rtsp_token_t value) {
    if (!request || !name.value) {
        return -1;
    }
    if (turbo_rtsp_token_case_eq(name, "Range")) {
        return turbo_rtsp_apply_range(request, value);
    }
    return 0;
}

int turbo_rtsp_parse_ctx_add_header(
    turbo_rtsp_parse_ctx_t *ctx,
    turbo_rtsp_token_t name,
    turbo_rtsp_token_t value) {
    turbo_rtsp_header_view_t *header = NULL;

    if (!ctx || !ctx->message) {
        return 0;
    }
    if (ctx->message->header_count >= TURBO_RTSP_MAX_MESSAGE_HEADERS) {
        return -1;
    }

    header = &ctx->message->headers[ctx->message->header_count++];
    header->name = name.value;
    header->name_len = name.length;
    header->value = value.value;
    header->value_len = value.length;
    return 0;
}

const turbo_rtsp_header_view_t *turbo_rtsp_message_find_header(
    const turbo_rtsp_message_t *message,
    const char *name) {
    size_t i = 0;

    if (!message || !name) {
        return NULL;
    }

    for (i = 0; i < message->header_count; ++i) {
        const turbo_rtsp_header_view_t *header = &message->headers[i];
        if (turbo_rtsp_view_case_eq(header->name, header->name_len, name)) {
            return header;
        }
    }
    return NULL;
}

const turbo_rtsp_header_view_t *turbo_rtsp_response_find_header(
    const turbo_rtsp_response_view_t *response,
    const char *name) {
    size_t i = 0;

    if (!response || !name) {
        return NULL;
    }

    for (i = 0; i < response->header_count; ++i) {
        const turbo_rtsp_header_view_t *header = &response->headers[i];
        if (turbo_rtsp_view_case_eq(header->name, header->name_len, name)) {
            return header;
        }
    }
    return NULL;
}

static const char *turbo_rtsp_find_crlf(
    const char *start,
    const char *end) {
    const char *p = start;

    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') {
            return p;
        }
        ++p;
    }
    return NULL;
}

static void turbo_rtsp_trim_ows_view(
    const char **value,
    size_t *value_len) {
    const char *start = NULL;
    const char *end = NULL;

    if (!value || !*value || !value_len) {
        return;
    }

    start = *value;
    end = start + *value_len;
    while (start < end && (*start == ' ' || *start == '\t')) {
        ++start;
    }
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) {
        --end;
    }

    *value = start;
    *value_len = (size_t)(end - start);
}

static int turbo_rtsp_view_to_size(
    const char *value,
    size_t value_len,
    size_t *parsed) {
    size_t i = 0;
    size_t result = 0;

    if (!value || value_len == 0 || !parsed) {
        return -1;
    }

    for (i = 0; i < value_len; ++i) {
        unsigned char c = (unsigned char)value[i];
        size_t digit = 0;

        if (c < '0' || c > '9') {
            return -1;
        }
        digit = (size_t)(c - '0');
        if (result > (((size_t)-1) - digit) / 10u) {
            return -1;
        }
        result = result * 10u + digit;
    }

    *parsed = result;
    return 0;
}

static int turbo_rtsp_parse_response_status_line(
    const char *buffer,
    const char *line_end,
    turbo_rtsp_response_view_t *response) {
    const char *code = NULL;

    if (line_end - buffer < 13 ||
        memcmp(buffer, "RTSP/1.0 ", 9) != 0) {
        return -1;
    }

    code = buffer + 9;
    if (!isdigit((unsigned char)code[0]) ||
        !isdigit((unsigned char)code[1]) ||
        !isdigit((unsigned char)code[2]) ||
        code + 3 >= line_end ||
        code[3] != ' ') {
        return -1;
    }

    response->status_code =
        ((int)(code[0] - '0') * 100) +
        ((int)(code[1] - '0') * 10) +
        (int)(code[2] - '0');
    response->reason = code + 4;
    response->reason_len = (size_t)(line_end - response->reason);
    return 0;
}

static int turbo_rtsp_response_add_header(
    turbo_rtsp_response_view_t *response,
    const char *name,
    size_t name_len,
    const char *value,
    size_t value_len) {
    turbo_rtsp_header_view_t *header = NULL;

    if (!response || !name || !value) {
        return -1;
    }
    if (response->header_count >= TURBO_RTSP_MAX_MESSAGE_HEADERS) {
        return -1;
    }

    header = &response->headers[response->header_count++];
    header->name = name;
    header->name_len = name_len;
    header->value = value;
    header->value_len = value_len;
    return 0;
}

static int turbo_rtsp_parse_response_headers(
    const char *header_start,
    const char *header_end,
    turbo_rtsp_response_view_t *response,
    size_t *content_length) {
    const char *line = header_start;
    int has_content_length = 0;

    if (!response || !content_length) {
        return -1;
    }

    *content_length = 0;
    while (line < header_end) {
        const char *line_end = turbo_rtsp_find_crlf(line, header_end);
        const char *colon = NULL;
        const char *name = NULL;
        const char *value = NULL;
        size_t name_len = 0;
        size_t value_len = 0;

        if (!line_end) {
            return -1;
        }
        if (line_end == line) {
            return 0;
        }

        colon = memchr(line, ':', (size_t)(line_end - line));
        if (!colon || colon == line) {
            return -1;
        }

        name = line;
        name_len = (size_t)(colon - line);
        value = colon + 1;
        value_len = (size_t)(line_end - value);
        turbo_rtsp_trim_ows_view(&value, &value_len);

        if (turbo_rtsp_response_add_header(response, name, name_len, value, value_len) != 0) {
            return -1;
        }

        if (turbo_rtsp_view_case_eq(name, name_len, "Content-Length")) {
            size_t parsed = 0;
            if (turbo_rtsp_view_to_size(value, value_len, &parsed) != 0) {
                return -1;
            }
            if (has_content_length && parsed != *content_length) {
                return -1;
            }
            has_content_length = 1;
            *content_length = parsed;
        }

        line = line_end + 2;
    }

    return -1;
}

int turbo_rtsp_parse_message(
    const char *buffer,
    size_t buffer_len,
    size_t *consumed,
    turbo_rtsp_message_t *message) {
    size_t header_end = 0;
    turbo_rtsp_lexer_t lexer;
    turbo_rtsp_token_t token;
    turbo_rtsp_parse_ctx_t ctx;
    void *parser = NULL;
    int token_id = 0;
    int rc = 0;

    if (!buffer || !consumed || !message) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    *consumed = 0;
    rc = turbo_rtsp_find_header_end(buffer, buffer_len, &header_end);
    if (rc > 0) {
        return TURBO_RTSP_PARSE_PARTIAL;
    }
    if (rc < 0) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    memset(message, 0, sizeof(*message));
    message->request.client_rtp_port = -1;
    message->request.client_rtcp_port = -1;
    message->request.interleaved_rtp_channel = -1;
    message->request.interleaved_rtcp_channel = -1;
    message->request.transport_spec.client_rtp_port = -1;
    message->request.transport_spec.client_rtcp_port = -1;
    message->request.transport_spec.server_rtp_port = -1;
    message->request.transport_spec.server_rtcp_port = -1;
    message->request.transport_spec.multicast_rtp_port = -1;
    message->request.transport_spec.multicast_rtcp_port = -1;
    message->request.transport_spec.interleaved_rtp_channel = -1;
    message->request.transport_spec.interleaved_rtcp_channel = -1;
    message->request.transport_spec.ttl = -1;
    message->request.transport_spec.layers = -1;

    memset(&ctx, 0, sizeof(ctx));
    ctx.request = &message->request;
    ctx.message = message;

    parser = TurboRtspParseAlloc(malloc);
    if (!parser) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    memset(&token, 0, sizeof(token));
    turbo_rtsp_lexer_init(&lexer, buffer, header_end);
    while (!ctx.error && (token_id = turbo_rtsp_lexer_next(&lexer, &token)) > 0) {
        TurboRtspParse(parser, token_id, token, &ctx);
    }
    if (token_id < 0) {
        ctx.error = 1;
    }

    TurboRtspParse(parser, 0, token, &ctx);
    TurboRtspParseFree(parser, free);

    if (ctx.error || !ctx.complete) {
        return TURBO_RTSP_PARSE_ERROR;
    }
    if (ctx.content_length > buffer_len - header_end) {
        return TURBO_RTSP_PARSE_PARTIAL;
    }

    *consumed = header_end + ctx.content_length;
    if (ctx.content_length > 0) {
        message->body = buffer + header_end;
        message->body_len = ctx.content_length;
    }
    return TURBO_RTSP_PARSE_OK;
}

int turbo_rtsp_parse_response(
    const char *buffer,
    size_t buffer_len,
    size_t *consumed,
    turbo_rtsp_response_view_t *response) {
    size_t header_end = 0;
    size_t content_length = 0;
    const char *status_line_end = NULL;
    int rc = 0;

    if (!buffer || !consumed || !response) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    *consumed = 0;
    rc = turbo_rtsp_find_header_end(buffer, buffer_len, &header_end);
    if (rc > 0) {
        return TURBO_RTSP_PARSE_PARTIAL;
    }
    if (rc < 0) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    memset(response, 0, sizeof(*response));
    status_line_end = turbo_rtsp_find_crlf(buffer, buffer + header_end);
    if (!status_line_end ||
        turbo_rtsp_parse_response_status_line(buffer, status_line_end, response) != 0) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    if (turbo_rtsp_parse_response_headers(
            status_line_end + 2,
            buffer + header_end,
            response,
            &content_length) != 0) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    if (content_length > buffer_len - header_end) {
        return TURBO_RTSP_PARSE_PARTIAL;
    }

    *consumed = header_end + content_length;
    if (content_length > 0) {
        response->body = buffer + header_end;
        response->body_len = content_length;
    }
    return TURBO_RTSP_PARSE_OK;
}

int turbo_rtsp_parse_request(
    const char *buffer,
    size_t buffer_len,
    size_t *consumed,
    turbo_rtsp_request_t *request) {
    turbo_rtsp_message_t message;
    int rc = 0;

    if (!request) {
        return TURBO_RTSP_PARSE_ERROR;
    }

    rc = turbo_rtsp_parse_message(buffer, buffer_len, consumed, &message);
    if (rc != TURBO_RTSP_PARSE_OK) {
        return rc;
    }

    *request = message.request;
    return TURBO_RTSP_PARSE_OK;
}

int turbo_rtsp_parse_transport(
    const char *transport,
    size_t transport_len,
    turbo_rtsp_transport_spec_t *spec) {
    turbo_rtsp_request_t request;
    turbo_rtsp_token_t token;

    if (!transport || !spec) {
        return -1;
    }

    if (transport_len == 0) {
        transport_len = strlen(transport);
    }
    if (transport_len == 0) {
        return -1;
    }

    memset(&request, 0, sizeof(request));
    token.value = transport;
    token.length = transport_len;
    token.method = TURBO_RTSP_METHOD_UNKNOWN;

    if (turbo_rtsp_apply_transport(&request, token) != 0) {
        return -1;
    }

    *spec = request.transport_spec;
    return 0;
}

int turbo_rtsp_parse_rtp_info(
    const char *rtp_info,
    size_t rtp_info_len,
    turbo_rtsp_rtp_info_t *infos,
    size_t info_capacity,
    size_t *info_count) {
    const char *cursor = rtp_info;
    const char *end = NULL;
    size_t count = 0;

    if (!rtp_info || !infos || info_capacity == 0 || !info_count) {
        return -1;
    }

    if (rtp_info_len == 0) {
        rtp_info_len = strlen(rtp_info);
    }
    end = rtp_info + rtp_info_len;
    *info_count = 0;

    while (cursor < end) {
        const char *entry_end = NULL;
        const char *param = NULL;
        turbo_rtsp_rtp_info_t *info = NULL;

        while (cursor < end &&
               (*cursor == ' ' || *cursor == '\t' || *cursor == ',')) {
            ++cursor;
        }
        if (cursor >= end) {
            break;
        }

        if (count >= info_capacity) {
            return -1;
        }

        entry_end = cursor;
        while (entry_end < end && *entry_end != ',') {
            ++entry_end;
        }

        info = &infos[count];
        memset(info, 0, sizeof(*info));
        param = cursor;
        while (param < entry_end) {
            const char *param_end = param;
            const char *key_end = NULL;
            const char *value = NULL;
            turbo_rtsp_token_t key;

            while (param < entry_end && (*param == ' ' || *param == '\t' || *param == ';')) {
                ++param;
            }
            if (param >= entry_end) {
                break;
            }

            param_end = param;
            while (param_end < entry_end && *param_end != ';') {
                ++param_end;
            }

            key_end = param;
            while (key_end < param_end && *key_end != '=') {
                ++key_end;
            }
            if (key_end >= param_end || *key_end != '=') {
                return -1;
            }

            key.value = param;
            key.length = (size_t)(key_end - param);
            key.method = TURBO_RTSP_METHOD_UNKNOWN;
            value = key_end + 1;
            if (value >= param_end) {
                return -1;
            }

            if (turbo_rtsp_token_case_eq(key, "url")) {
                if (turbo_rtsp_copy_range(info->url, sizeof(info->url), value, param_end) != 0) {
                    return -1;
                }
            } else if (turbo_rtsp_token_case_eq(key, "seq")) {
                if (turbo_rtsp_parse_u32_value(value, param_end, &info->seq) != 0) {
                    return -1;
                }
                info->has_seq = 1;
            } else if (turbo_rtsp_token_case_eq(key, "rtptime")) {
                if (turbo_rtsp_parse_u32_value(value, param_end, &info->rtptime) != 0) {
                    return -1;
                }
                info->has_rtptime = 1;
            }

            param = param_end < entry_end ? param_end + 1 : param_end;
        }

        if (info->url[0] == '\0') {
            return -1;
        }

        ++count;
        cursor = entry_end < end ? entry_end + 1 : entry_end;
    }

    *info_count = count;
    return count > 0 ? 0 : -1;
}

int turbo_rtsp_format_request(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_request_message_t *request) {
    char *cursor = buffer;
    size_t remaining = buffer_size;
    size_t i = 0;
    size_t body_len = 0;
    const char *method = NULL;

    if (!buffer || buffer_size == 0 || !request || !request->uri || request->cseq == 0) {
        return -1;
    }

    buffer[0] = '\0';
    method = turbo_rtsp_method_name(request->method);
    if (!method) {
        return -1;
    }

    if (request->body) {
        body_len = request->body_len != 0 ? request->body_len : strlen(request->body);
    }

    if (turbo_rtsp_appendf(
            &cursor,
            &remaining,
            "%s %s RTSP/1.0\r\nCSeq: %u\r\n",
            method,
            request->uri,
            request->cseq) != 0) {
        return -1;
    }
    if (request->user_agent && request->user_agent[0] != '\0' &&
        turbo_rtsp_appendf(&cursor, &remaining, "User-Agent: %s\r\n", request->user_agent) != 0) {
        return -1;
    }
    if (request->session_id && request->session_id[0] != '\0' &&
        turbo_rtsp_appendf(&cursor, &remaining, "Session: %s\r\n", request->session_id) != 0) {
        return -1;
    }
    if (request->transport && request->transport[0] != '\0' &&
        turbo_rtsp_appendf(&cursor, &remaining, "Transport: %s\r\n", request->transport) != 0) {
        return -1;
    }
    if (request->range && request->range[0] != '\0' &&
        turbo_rtsp_appendf(&cursor, &remaining, "Range: %s\r\n", request->range) != 0) {
        return -1;
    }
    if (request->content_type && request->content_type[0] != '\0' &&
        turbo_rtsp_appendf(&cursor, &remaining, "Content-Type: %s\r\n", request->content_type) != 0) {
        return -1;
    }

    for (i = 0; i < request->header_count; ++i) {
        const turbo_rtsp_header_t *header = &request->headers[i];
        if (header->name && turbo_rtsp_is_managed_request_header(header->name)) {
            return -1;
        }
        if (header->name && header->value &&
            turbo_rtsp_appendf(&cursor, &remaining, "%s: %s\r\n", header->name, header->value) != 0) {
            return -1;
        }
    }

    if (turbo_rtsp_appendf(&cursor, &remaining, "Content-Length: %zu\r\n", body_len) != 0) {
        return -1;
    }
    if (turbo_rtsp_appendf(&cursor, &remaining, "\r\n") != 0) {
        return -1;
    }
    if (body_len > 0 &&
        turbo_rtsp_append_bytes(&cursor, &remaining, request->body, body_len) != 0) {
        return -1;
    }

    return (int)(cursor - buffer);
}

int turbo_rtsp_format_response(
    char *buffer,
    size_t buffer_size,
    uint32_t cseq,
    const char *server_name,
    const turbo_rtsp_response_t *response) {
    char *cursor = buffer;
    size_t remaining = buffer_size;
    size_t i = 0;
    size_t body_len = 0;
    int status_code = 0;
    const char *reason = NULL;

    if (!buffer || buffer_size == 0 || !response) {
        return -1;
    }

    buffer[0] = '\0';
    status_code = response->status_code != 0 ? response->status_code : 200;
    reason = response->reason ? response->reason : turbo_rtsp_reason_phrase(status_code);
    body_len = turbo_rtsp_body_len(response);

    if (turbo_rtsp_appendf(
            &cursor,
            &remaining,
            "RTSP/1.0 %d %s\r\nCSeq: %u\r\n",
            status_code,
            reason,
            cseq) != 0) {
        return -1;
    }

    if (server_name && server_name[0] != '\0' &&
        turbo_rtsp_appendf(&cursor, &remaining, "Server: %s\r\n", server_name) != 0) {
        return -1;
    }
    if (response->public_methods &&
        turbo_rtsp_appendf(&cursor, &remaining, "Public: %s\r\n", response->public_methods) != 0) {
        return -1;
    }
    if (response->session_id &&
        turbo_rtsp_appendf(&cursor, &remaining, "Session: %s\r\n", response->session_id) != 0) {
        return -1;
    }
    if (response->transport &&
        turbo_rtsp_appendf(&cursor, &remaining, "Transport: %s\r\n", response->transport) != 0) {
        return -1;
    }
    if (response->range &&
        turbo_rtsp_appendf(&cursor, &remaining, "Range: %s\r\n", response->range) != 0) {
        return -1;
    }
    if (response->rtp_info &&
        turbo_rtsp_appendf(&cursor, &remaining, "RTP-Info: %s\r\n", response->rtp_info) != 0) {
        return -1;
    }
    if (response->content_type &&
        turbo_rtsp_appendf(&cursor, &remaining, "Content-Type: %s\r\n", response->content_type) != 0) {
        return -1;
    }

    for (i = 0; i < response->header_count; ++i) {
        const turbo_rtsp_header_t *header = &response->headers[i];
        if (header->name && turbo_rtsp_is_managed_response_header(header->name)) {
            return -1;
        }
        if (header->name && header->value &&
            turbo_rtsp_appendf(&cursor, &remaining, "%s: %s\r\n", header->name, header->value) != 0) {
            return -1;
        }
    }

    if (turbo_rtsp_appendf(&cursor, &remaining, "Content-Length: %zu\r\n", body_len) != 0) {
        return -1;
    }

    if (turbo_rtsp_appendf(&cursor, &remaining, "\r\n") != 0) {
        return -1;
    }

    if (body_len > 0 &&
        turbo_rtsp_append_bytes(&cursor, &remaining, response->body, body_len) != 0) {
        return -1;
    }

    return (int)(cursor - buffer);
}

#endif /* TURBO_MEDIA_HAS_RTSP */
