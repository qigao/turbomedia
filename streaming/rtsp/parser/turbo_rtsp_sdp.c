#include "turbo_rtsp_sdp.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int turbo_rtsp_sdp_appendf(
    char **cursor,
    size_t *remaining,
    const char *fmt,
    ...) {
    va_list ap;
    int needed = 0;

    if (!cursor || !*cursor || !remaining || !fmt) {
        return -1;
    }

    va_start(ap, fmt);
    needed = vsnprintf(*cursor, *remaining, fmt, ap);
    va_end(ap);

    if (needed < 0 || (size_t)needed >= *remaining) {
        return -1;
    }

    *cursor += needed;
    *remaining -= (size_t)needed;
    return 0;
}

int turbo_rtsp_sdp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_session_t *session,
    const turbo_rtsp_sdp_media_t *media,
    size_t media_count) {
    char *cursor = buffer;
    size_t remaining = buffer_size;
    size_t i = 0;
    const char *origin_user = NULL;
    const char *origin_address = NULL;
    const char *session_name = NULL;
    const char *connection_address = NULL;

    if (!buffer || buffer_size == 0 || !session || (!media && media_count != 0)) {
        return -1;
    }

    buffer[0] = '\0';
    origin_user = session->origin_user ? session->origin_user : "-";
    origin_address = session->origin_address ? session->origin_address : "0.0.0.0";
    session_name = session->session_name ? session->session_name : "-";
    connection_address =
        session->connection_address ? session->connection_address : origin_address;

    if (turbo_rtsp_sdp_appendf(&cursor, &remaining, "v=0\r\n") != 0 ||
        turbo_rtsp_sdp_appendf(
            &cursor,
            &remaining,
            "o=%s %u %u IN IP4 %s\r\n",
            origin_user,
            session->session_id,
            session->session_version,
            origin_address) != 0 ||
        turbo_rtsp_sdp_appendf(&cursor, &remaining, "s=%s\r\n", session_name) != 0 ||
        turbo_rtsp_sdp_appendf(&cursor, &remaining, "c=IN IP4 %s\r\n", connection_address) != 0 ||
        turbo_rtsp_sdp_appendf(&cursor, &remaining, "t=0 0\r\n") != 0) {
        return -1;
    }
    if (session->range &&
        turbo_rtsp_sdp_appendf(&cursor, &remaining, "a=range:%s\r\n", session->range) != 0) {
        return -1;
    }
    if (session->direction &&
        turbo_rtsp_sdp_appendf(&cursor, &remaining, "a=%s\r\n", session->direction) != 0) {
        return -1;
    }

    for (i = 0; i < media_count; ++i) {
        const turbo_rtsp_sdp_media_t *m = &media[i];
        if (!m->media || !m->proto || !m->encoding_name || m->port < 0 ||
            m->payload_type < 0 || m->clock_rate <= 0) {
            return -1;
        }
        if (turbo_rtsp_sdp_appendf(
                &cursor,
                &remaining,
                "m=%s %d %s %d\r\n",
                m->media,
                m->port,
                m->proto,
                m->payload_type) != 0 ||
            (m->connection_address &&
             turbo_rtsp_sdp_appendf(
                 &cursor,
                 &remaining,
                 "c=IN IP4 %s\r\n",
                 m->connection_address) != 0) ||
            turbo_rtsp_sdp_appendf(
                &cursor,
                &remaining,
                "a=rtpmap:%d %s/%d\r\n",
                m->payload_type,
                m->encoding_name,
                m->clock_rate) != 0) {
            return -1;
        }
        if (m->fmtp &&
            turbo_rtsp_sdp_appendf(
                &cursor,
                &remaining,
                "a=fmtp:%d %s\r\n",
                m->payload_type,
                m->fmtp) != 0) {
            return -1;
        }
        if (m->control &&
            turbo_rtsp_sdp_appendf(&cursor, &remaining, "a=control:%s\r\n", m->control) != 0) {
            return -1;
        }
        if (m->range &&
            turbo_rtsp_sdp_appendf(&cursor, &remaining, "a=range:%s\r\n", m->range) != 0) {
            return -1;
        }
        if (m->direction &&
            turbo_rtsp_sdp_appendf(&cursor, &remaining, "a=%s\r\n", m->direction) != 0) {
            return -1;
        }
    }

    return (int)(cursor - buffer);
}

static void turbo_rtsp_sdp_trim_line(const char **start, const char **end) {
    while (*start < *end && (**start == ' ' || **start == '\t')) {
        ++(*start);
    }
    while (*end > *start &&
           ((*end)[-1] == ' ' || (*end)[-1] == '\t' ||
            (*end)[-1] == '\r' || (*end)[-1] == '\n')) {
        --(*end);
    }
}

static int turbo_rtsp_sdp_copy_range(
    char *dst,
    size_t dst_size,
    const char *start,
    const char *end) {
    size_t len = 0;

    if (!dst || dst_size == 0 || !start || !end || end < start) {
        return -1;
    }

    len = (size_t)(end - start);
    if (len >= dst_size) {
        return -1;
    }

    if (len > 0) {
        memcpy(dst, start, len);
    }
    dst[len] = '\0';
    return 0;
}

static int turbo_rtsp_sdp_ascii_case_equal(
    const char *left,
    size_t left_len,
    const char *right) {
    size_t i = 0;
    size_t right_len = 0;

    if (!left || !right) {
        return 0;
    }

    right_len = strlen(right);
    if (left_len != right_len) {
        return 0;
    }

    for (i = 0; i < left_len; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int turbo_rtsp_sdp_base64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (int)(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
        return (int)(ch - 'a') + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0') + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

/* Decode SDP sprop values into caller-owned storage; base64_utils exposes only
 * an allocating decode API, which would put heap ownership across DLLs. */
static int turbo_rtsp_sdp_base64_decode_segment(
    const char *encoded,
    size_t encoded_len,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *written) {
    uint32_t accumulator = 0;
    int bits = 0;
    int saw_padding = 0;
    size_t offset = 0;
    size_t i = 0;

    if (written) {
        *written = 0;
    }
    if (!encoded || encoded_len == 0 || !buffer || !written) {
        return -1;
    }

    for (i = 0; i < encoded_len; ++i) {
        int value = 0;
        unsigned char ch = (unsigned char)encoded[i];

        if (ch == '=') {
            saw_padding = 1;
            continue;
        }
        if (saw_padding) {
            return -1;
        }

        value = turbo_rtsp_sdp_base64_value(ch);
        if (value < 0) {
            return -1;
        }

        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (offset >= buffer_size) {
                return -1;
            }
            buffer[offset++] = (uint8_t)((accumulator >> bits) & 0xffu);
        }
    }

    if (offset == 0) {
        return -1;
    }
    *written = offset;
    return 0;
}

static int turbo_rtsp_sdp_h265_fmtp_copy_value(
    char *dst,
    size_t dst_size,
    uint32_t flag,
    const char *value,
    const char *value_end,
    turbo_rtsp_sdp_h265_fmtp_t *h265) {
    if (value >= value_end ||
        turbo_rtsp_sdp_copy_range(dst, dst_size, value, value_end) != 0) {
        return -1;
    }
    h265->flags |= flag;
    return 0;
}

int turbo_rtsp_sdp_h265_fmtp_parse(
    const char *fmtp,
    size_t fmtp_len,
    turbo_rtsp_sdp_h265_fmtp_t *h265) {
    const char *cursor = fmtp;
    const char *end = NULL;

    if (!fmtp || !h265) {
        return -1;
    }
    if (fmtp_len == 0) {
        fmtp_len = strlen(fmtp);
    }
    end = fmtp + fmtp_len;
    memset(h265, 0, sizeof(*h265));

    while (cursor < end) {
        const char *name = NULL;
        const char *name_end = NULL;
        const char *value = NULL;
        const char *value_end = NULL;
        const char *separator = NULL;

        while (cursor < end &&
               (*cursor == ' ' || *cursor == '\t' || *cursor == ';')) {
            ++cursor;
        }
        if (cursor >= end) {
            break;
        }

        name = cursor;
        while (cursor < end && *cursor != '=' && *cursor != ';') {
            ++cursor;
        }
        if (cursor >= end || *cursor != '=') {
            return -1;
        }
        name_end = cursor;
        turbo_rtsp_sdp_trim_line(&name, &name_end);
        if (name >= name_end) {
            return -1;
        }

        value = cursor + 1;
        separator = memchr(value, ';', (size_t)(end - value));
        value_end = separator ? separator : end;
        turbo_rtsp_sdp_trim_line(&value, &value_end);

        if (turbo_rtsp_sdp_ascii_case_equal(
                name,
                (size_t)(name_end - name),
                "sprop-vps")) {
            if (turbo_rtsp_sdp_h265_fmtp_copy_value(
                    h265->sprop_vps,
                    sizeof(h265->sprop_vps),
                    TURBO_RTSP_SDP_H265_FMTP_SPROP_VPS,
                    value,
                    value_end,
                    h265) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "sprop-sps")) {
            if (turbo_rtsp_sdp_h265_fmtp_copy_value(
                    h265->sprop_sps,
                    sizeof(h265->sprop_sps),
                    TURBO_RTSP_SDP_H265_FMTP_SPROP_SPS,
                    value,
                    value_end,
                    h265) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "sprop-pps")) {
            if (turbo_rtsp_sdp_h265_fmtp_copy_value(
                    h265->sprop_pps,
                    sizeof(h265->sprop_pps),
                    TURBO_RTSP_SDP_H265_FMTP_SPROP_PPS,
                    value,
                    value_end,
                    h265) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "sprop-sei")) {
            if (turbo_rtsp_sdp_h265_fmtp_copy_value(
                    h265->sprop_sei,
                    sizeof(h265->sprop_sei),
                    TURBO_RTSP_SDP_H265_FMTP_SPROP_SEI,
                    value,
                    value_end,
                    h265) != 0) {
                return -1;
            }
        }

        cursor = separator ? separator + 1 : end;
    }

    return 0;
}

static int turbo_rtsp_sdp_h265_fmtp_append_sprop(
    char **cursor,
    size_t *remaining,
    int *has_previous,
    const char *name,
    uint32_t flag,
    const char *value,
    const turbo_rtsp_sdp_h265_fmtp_t *h265) {
    if ((h265->flags & flag) == 0 || !value || value[0] == '\0') {
        return 0;
    }

    if (*has_previous &&
        turbo_rtsp_sdp_appendf(cursor, remaining, "; ") != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_appendf(cursor, remaining, "%s=%s", name, value) != 0) {
        return -1;
    }
    *has_previous = 1;
    return 0;
}

int turbo_rtsp_sdp_h265_fmtp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h265_fmtp_t *h265) {
    char *cursor = buffer;
    size_t remaining = buffer_size;
    int has_previous = 0;

    if (!buffer || buffer_size == 0 || !h265) {
        return -1;
    }

    buffer[0] = '\0';
    if (turbo_rtsp_sdp_h265_fmtp_append_sprop(
            &cursor,
            &remaining,
            &has_previous,
            "sprop-vps",
            TURBO_RTSP_SDP_H265_FMTP_SPROP_VPS,
            h265->sprop_vps,
            h265) != 0 ||
        turbo_rtsp_sdp_h265_fmtp_append_sprop(
            &cursor,
            &remaining,
            &has_previous,
            "sprop-sps",
            TURBO_RTSP_SDP_H265_FMTP_SPROP_SPS,
            h265->sprop_sps,
            h265) != 0 ||
        turbo_rtsp_sdp_h265_fmtp_append_sprop(
            &cursor,
            &remaining,
            &has_previous,
            "sprop-pps",
            TURBO_RTSP_SDP_H265_FMTP_SPROP_PPS,
            h265->sprop_pps,
            h265) != 0 ||
        turbo_rtsp_sdp_h265_fmtp_append_sprop(
            &cursor,
            &remaining,
            &has_previous,
            "sprop-sei",
            TURBO_RTSP_SDP_H265_FMTP_SPROP_SEI,
            h265->sprop_sei,
            h265) != 0 ||
        !has_previous) {
        buffer[0] = '\0';
        return -1;
    }

    return (int)(cursor - buffer);
}

static int turbo_rtsp_sdp_h265_fmtp_write_annexb_sprop(
    uint8_t *buffer,
    size_t buffer_size,
    const char *sprop,
    size_t *offset) {
    static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
    const char *cursor = sprop;

    if (!sprop || sprop[0] == '\0') {
        return 0;
    }

    while (*cursor != '\0') {
        const char *next = strchr(cursor, ',');
        const char *segment = cursor;
        const char *segment_end = next ? next : cursor + strlen(cursor);
        char encoded[TURBO_RTSP_SDP_H265_MAX_SPROP_PPS];
        uint8_t decoded[TURBO_RTSP_SDP_H265_MAX_SPROP_PPS];
        size_t decoded_len = 0;

        turbo_rtsp_sdp_trim_line(&segment, &segment_end);
        if (segment >= segment_end ||
            (size_t)(segment_end - segment) >= sizeof(encoded) ||
            turbo_rtsp_sdp_copy_range(
                encoded,
                sizeof(encoded),
                segment,
                segment_end) != 0) {
            return -1;
        }
        if (turbo_rtsp_sdp_base64_decode_segment(
                encoded,
                strlen(encoded),
                decoded,
                sizeof(decoded),
                &decoded_len) != 0) {
            return -1;
        }

        if (*offset > buffer_size ||
            sizeof(start_code) > buffer_size - *offset ||
            decoded_len > buffer_size - *offset - sizeof(start_code)) {
            return -1;
        }

        memcpy(buffer + *offset, start_code, sizeof(start_code));
        *offset += sizeof(start_code);
        if (decoded_len > 0) {
            memcpy(buffer + *offset, decoded, decoded_len);
            *offset += decoded_len;
        }

        cursor = next ? next + 1 : segment_end;
    }

    return 0;
}

int turbo_rtsp_sdp_h265_fmtp_write_annexb(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h265_fmtp_t *h265,
    size_t *written) {
    size_t offset = 0;

    if (written) {
        *written = 0;
    }
    if (!buffer || !h265 || !written) {
        return -1;
    }

    if (((h265->flags & TURBO_RTSP_SDP_H265_FMTP_SPROP_VPS) != 0 &&
         turbo_rtsp_sdp_h265_fmtp_write_annexb_sprop(
             buffer,
             buffer_size,
             h265->sprop_vps,
             &offset) != 0) ||
        ((h265->flags & TURBO_RTSP_SDP_H265_FMTP_SPROP_SPS) != 0 &&
         turbo_rtsp_sdp_h265_fmtp_write_annexb_sprop(
             buffer,
             buffer_size,
             h265->sprop_sps,
             &offset) != 0) ||
        ((h265->flags & TURBO_RTSP_SDP_H265_FMTP_SPROP_PPS) != 0 &&
         turbo_rtsp_sdp_h265_fmtp_write_annexb_sprop(
             buffer,
             buffer_size,
             h265->sprop_pps,
             &offset) != 0) ||
        ((h265->flags & TURBO_RTSP_SDP_H265_FMTP_SPROP_SEI) != 0 &&
         turbo_rtsp_sdp_h265_fmtp_write_annexb_sprop(
             buffer,
             buffer_size,
             h265->sprop_sei,
             &offset) != 0) ||
        offset == 0) {
        return -1;
    }

    *written = offset;
    return 0;
}

static int turbo_rtsp_sdp_token(
    const char **cursor,
    const char *end,
    char *dst,
    size_t dst_size) {
    const char *start = NULL;

    while (*cursor < end && (**cursor == ' ' || **cursor == '\t')) {
        ++(*cursor);
    }
    if (*cursor >= end) {
        return -1;
    }

    start = *cursor;
    while (*cursor < end && **cursor != ' ' && **cursor != '\t') {
        ++(*cursor);
    }

    return turbo_rtsp_sdp_copy_range(dst, dst_size, start, *cursor);
}

static int turbo_rtsp_sdp_parse_int_range(
    const char *start,
    const char *end,
    int min_value,
    int max_value,
    int *value) {
    int parsed = 0;

    if (!start || start >= end || !value || min_value > max_value) {
        return -1;
    }

    while (start < end) {
        unsigned char c = (unsigned char)*start;
        int digit = 0;

        if (c < '0' || c > '9') {
            return -1;
        }
        digit = (int)(c - '0');
        if (parsed > (max_value - digit) / 10) {
            return -1;
        }
        parsed = parsed * 10 + digit;
        ++start;
    }

    if (parsed < min_value || parsed > max_value) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int turbo_rtsp_sdp_parse_int_string(
    const char *value,
    const char *value_end,
    int min_value,
    int max_value,
    int *out) {
    char number[16];

    if (turbo_rtsp_sdp_copy_range(number, sizeof(number), value, value_end) != 0) {
        return -1;
    }
    return turbo_rtsp_sdp_parse_int_range(
        number,
        number + strlen(number),
        min_value,
        max_value,
        out);
}

static int turbo_rtsp_sdp_h264_fmtp_copy_string(
    char *dst,
    size_t dst_size,
    uint32_t flag,
    const char *value,
    const char *value_end,
    turbo_rtsp_sdp_h264_fmtp_t *h264) {
    if (value >= value_end ||
        turbo_rtsp_sdp_copy_range(dst, dst_size, value, value_end) != 0) {
        return -1;
    }
    h264->flags |= flag;
    return 0;
}

int turbo_rtsp_sdp_h264_fmtp_parse(
    const char *fmtp,
    size_t fmtp_len,
    turbo_rtsp_sdp_h264_fmtp_t *h264) {
    const char *cursor = fmtp;
    const char *end = NULL;

    if (!fmtp || !h264) {
        return -1;
    }
    if (fmtp_len == 0) {
        fmtp_len = strlen(fmtp);
    }
    end = fmtp + fmtp_len;
    memset(h264, 0, sizeof(*h264));

    while (cursor < end) {
        const char *name = NULL;
        const char *name_end = NULL;
        const char *value = NULL;
        const char *value_end = NULL;
        const char *separator = NULL;

        while (cursor < end &&
               (*cursor == ' ' || *cursor == '\t' || *cursor == ';')) {
            ++cursor;
        }
        if (cursor >= end) {
            break;
        }

        name = cursor;
        while (cursor < end && *cursor != '=' && *cursor != ';') {
            ++cursor;
        }
        if (cursor >= end || *cursor != '=') {
            return -1;
        }
        name_end = cursor;
        turbo_rtsp_sdp_trim_line(&name, &name_end);
        if (name >= name_end) {
            return -1;
        }

        value = cursor + 1;
        separator = memchr(value, ';', (size_t)(end - value));
        value_end = separator ? separator : end;
        turbo_rtsp_sdp_trim_line(&value, &value_end);

        if (turbo_rtsp_sdp_ascii_case_equal(
                name,
                (size_t)(name_end - name),
                "packetization-mode")) {
            if (turbo_rtsp_sdp_parse_int_string(
                    value,
                    value_end,
                    0,
                    2,
                    &h264->packetization_mode) != 0) {
                return -1;
            }
            h264->flags |= TURBO_RTSP_SDP_H264_FMTP_PACKETIZATION_MODE;
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "profile-level-id")) {
            if (turbo_rtsp_sdp_h264_fmtp_copy_string(
                    h264->profile_level_id,
                    sizeof(h264->profile_level_id),
                    TURBO_RTSP_SDP_H264_FMTP_PROFILE_LEVEL_ID,
                    value,
                    value_end,
                    h264) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "sprop-parameter-sets")) {
            if (turbo_rtsp_sdp_h264_fmtp_copy_string(
                    h264->sprop_parameter_sets,
                    sizeof(h264->sprop_parameter_sets),
                    TURBO_RTSP_SDP_H264_FMTP_SPROP_PARAMETER_SETS,
                    value,
                    value_end,
                    h264) != 0) {
                return -1;
            }
        }

        cursor = separator ? separator + 1 : end;
    }

    return 0;
}

static int turbo_rtsp_sdp_h264_fmtp_append_int(
    char **cursor,
    size_t *remaining,
    int *has_previous,
    const char *name,
    uint32_t flag,
    int value,
    const turbo_rtsp_sdp_h264_fmtp_t *h264) {
    if ((h264->flags & flag) == 0) {
        return 0;
    }
    if (*has_previous &&
        turbo_rtsp_sdp_appendf(cursor, remaining, ";") != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_appendf(cursor, remaining, "%s=%d", name, value) != 0) {
        return -1;
    }
    *has_previous = 1;
    return 0;
}

static int turbo_rtsp_sdp_h264_fmtp_append_string(
    char **cursor,
    size_t *remaining,
    int *has_previous,
    const char *name,
    uint32_t flag,
    const char *value,
    const turbo_rtsp_sdp_h264_fmtp_t *h264) {
    if ((h264->flags & flag) == 0 || !value || value[0] == '\0') {
        return 0;
    }
    if (*has_previous &&
        turbo_rtsp_sdp_appendf(cursor, remaining, ";") != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_appendf(cursor, remaining, "%s=%s", name, value) != 0) {
        return -1;
    }
    *has_previous = 1;
    return 0;
}

int turbo_rtsp_sdp_h264_fmtp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h264_fmtp_t *h264) {
    char *cursor = buffer;
    size_t remaining = buffer_size;
    int has_previous = 0;

    if (!buffer || buffer_size == 0 || !h264) {
        return -1;
    }

    buffer[0] = '\0';
    if (turbo_rtsp_sdp_h264_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "packetization-mode",
            TURBO_RTSP_SDP_H264_FMTP_PACKETIZATION_MODE,
            h264->packetization_mode,
            h264) != 0 ||
        turbo_rtsp_sdp_h264_fmtp_append_string(
            &cursor,
            &remaining,
            &has_previous,
            "profile-level-id",
            TURBO_RTSP_SDP_H264_FMTP_PROFILE_LEVEL_ID,
            h264->profile_level_id,
            h264) != 0 ||
        turbo_rtsp_sdp_h264_fmtp_append_string(
            &cursor,
            &remaining,
            &has_previous,
            "sprop-parameter-sets",
            TURBO_RTSP_SDP_H264_FMTP_SPROP_PARAMETER_SETS,
            h264->sprop_parameter_sets,
            h264) != 0 ||
        !has_previous) {
        buffer[0] = '\0';
        return -1;
    }

    return (int)(cursor - buffer);
}

int turbo_rtsp_sdp_h264_fmtp_write_annexb(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h264_fmtp_t *h264,
    size_t *written) {
    size_t offset = 0;

    if (written) {
        *written = 0;
    }
    if (!buffer || !h264 || !written ||
        (h264->flags & TURBO_RTSP_SDP_H264_FMTP_SPROP_PARAMETER_SETS) == 0) {
        return -1;
    }

    if (turbo_rtsp_sdp_h265_fmtp_write_annexb_sprop(
            buffer,
            buffer_size,
            h264->sprop_parameter_sets,
            &offset) != 0 ||
        offset == 0) {
        return -1;
    }

    *written = offset;
    return 0;
}

static int turbo_rtsp_sdp_mpeg4_fmtp_copy_string(
    char *dst,
    size_t dst_size,
    uint32_t flag,
    const char *value,
    const char *value_end,
    turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4) {
    if (value >= value_end ||
        turbo_rtsp_sdp_copy_range(dst, dst_size, value, value_end) != 0) {
        return -1;
    }
    mpeg4->flags |= flag;
    return 0;
}

static int turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
    int *dst,
    uint32_t flag,
    const char *value,
    const char *value_end,
    turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4) {
    if (!dst ||
        turbo_rtsp_sdp_parse_int_string(value, value_end, 0, INT_MAX, dst) != 0) {
        return -1;
    }
    mpeg4->flags |= flag;
    return 0;
}

int turbo_rtsp_sdp_mpeg4_fmtp_parse(
    const char *fmtp,
    size_t fmtp_len,
    turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4) {
    const char *cursor = fmtp;
    const char *end = NULL;

    if (!fmtp || !mpeg4) {
        return -1;
    }
    if (fmtp_len == 0) {
        fmtp_len = strlen(fmtp);
    }
    end = fmtp + fmtp_len;
    memset(mpeg4, 0, sizeof(*mpeg4));

    while (cursor < end) {
        const char *name = NULL;
        const char *name_end = NULL;
        const char *value = NULL;
        const char *value_end = NULL;
        const char *separator = NULL;

        while (cursor < end &&
               (*cursor == ' ' || *cursor == '\t' || *cursor == ';')) {
            ++cursor;
        }
        if (cursor >= end) {
            break;
        }

        name = cursor;
        while (cursor < end && *cursor != '=' && *cursor != ';') {
            ++cursor;
        }
        if (cursor >= end || *cursor != '=') {
            return -1;
        }
        name_end = cursor;
        turbo_rtsp_sdp_trim_line(&name, &name_end);
        if (name >= name_end) {
            return -1;
        }

        value = cursor + 1;
        separator = memchr(value, ';', (size_t)(end - value));
        value_end = separator ? separator : end;
        turbo_rtsp_sdp_trim_line(&value, &value_end);

        if (turbo_rtsp_sdp_ascii_case_equal(
                name,
                (size_t)(name_end - name),
                "streamtype")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->stream_type,
                    TURBO_RTSP_SDP_MPEG4_FMTP_STREAM_TYPE,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "profile-level-id")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_string(
                    mpeg4->profile_level_id,
                    sizeof(mpeg4->profile_level_id),
                    TURBO_RTSP_SDP_MPEG4_FMTP_PROFILE_LEVEL_ID,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "mode")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_string(
                    mpeg4->mode,
                    sizeof(mpeg4->mode),
                    TURBO_RTSP_SDP_MPEG4_FMTP_MODE,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "config")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_string(
                    mpeg4->config,
                    sizeof(mpeg4->config),
                    TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "objecttype")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->object_type,
                    TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT_TYPE,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "object")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->object,
                    TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "cpresent")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->cpresent,
                    TURBO_RTSP_SDP_MPEG4_FMTP_CPRESENT,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "constantsize")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->constant_size,
                    TURBO_RTSP_SDP_MPEG4_FMTP_CONSTANT_SIZE,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "constantduration")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->constant_duration,
                    TURBO_RTSP_SDP_MPEG4_FMTP_CONSTANT_DURATION,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "sizelength")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->size_length,
                    TURBO_RTSP_SDP_MPEG4_FMTP_SIZE_LENGTH,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "indexlength")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->index_length,
                    TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_LENGTH,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        } else if (turbo_rtsp_sdp_ascii_case_equal(
                       name,
                       (size_t)(name_end - name),
                       "indexdeltalength")) {
            if (turbo_rtsp_sdp_mpeg4_fmtp_copy_int(
                    &mpeg4->index_delta_length,
                    TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_DELTA_LENGTH,
                    value,
                    value_end,
                    mpeg4) != 0) {
                return -1;
            }
        }

        cursor = separator ? separator + 1 : end;
    }

    return 0;
}

static int turbo_rtsp_sdp_mpeg4_fmtp_append_int(
    char **cursor,
    size_t *remaining,
    int *has_previous,
    const char *name,
    uint32_t flag,
    int value,
    const turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4) {
    if ((mpeg4->flags & flag) == 0) {
        return 0;
    }
    if (*has_previous &&
        turbo_rtsp_sdp_appendf(cursor, remaining, ";") != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_appendf(cursor, remaining, "%s=%d", name, value) != 0) {
        return -1;
    }
    *has_previous = 1;
    return 0;
}

static int turbo_rtsp_sdp_mpeg4_fmtp_append_string(
    char **cursor,
    size_t *remaining,
    int *has_previous,
    const char *name,
    uint32_t flag,
    const char *value,
    const turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4) {
    if ((mpeg4->flags & flag) == 0 || !value || value[0] == '\0') {
        return 0;
    }
    if (*has_previous &&
        turbo_rtsp_sdp_appendf(cursor, remaining, ";") != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_appendf(cursor, remaining, "%s=%s", name, value) != 0) {
        return -1;
    }
    *has_previous = 1;
    return 0;
}

int turbo_rtsp_sdp_mpeg4_fmtp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4) {
    char *cursor = buffer;
    size_t remaining = buffer_size;
    int has_previous = 0;

    if (!buffer || buffer_size == 0 || !mpeg4) {
        return -1;
    }

    buffer[0] = '\0';
    if (turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "streamtype",
            TURBO_RTSP_SDP_MPEG4_FMTP_STREAM_TYPE,
            mpeg4->stream_type,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_string(
            &cursor,
            &remaining,
            &has_previous,
            "profile-level-id",
            TURBO_RTSP_SDP_MPEG4_FMTP_PROFILE_LEVEL_ID,
            mpeg4->profile_level_id,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_string(
            &cursor,
            &remaining,
            &has_previous,
            "mode",
            TURBO_RTSP_SDP_MPEG4_FMTP_MODE,
            mpeg4->mode,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "objecttype",
            TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT_TYPE,
            mpeg4->object_type,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "object",
            TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT,
            mpeg4->object,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "cpresent",
            TURBO_RTSP_SDP_MPEG4_FMTP_CPRESENT,
            mpeg4->cpresent,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "sizelength",
            TURBO_RTSP_SDP_MPEG4_FMTP_SIZE_LENGTH,
            mpeg4->size_length,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "indexlength",
            TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_LENGTH,
            mpeg4->index_length,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_int(
            &cursor,
            &remaining,
            &has_previous,
            "indexdeltalength",
            TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_DELTA_LENGTH,
            mpeg4->index_delta_length,
            mpeg4) != 0 ||
        turbo_rtsp_sdp_mpeg4_fmtp_append_string(
            &cursor,
            &remaining,
            &has_previous,
            "config",
            TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG,
            mpeg4->config,
            mpeg4) != 0 ||
        !has_previous) {
        buffer[0] = '\0';
        return -1;
    }

    return (int)(cursor - buffer);
}

static int turbo_rtsp_sdp_hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int turbo_rtsp_sdp_mpeg4_fmtp_write_config(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4,
    size_t *written) {
    size_t config_len = 0;
    size_t i = 0;

    if (written) {
        *written = 0;
    }
    if (!buffer || !mpeg4 || !written ||
        (mpeg4->flags & TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG) == 0) {
        return -1;
    }

    config_len = strlen(mpeg4->config);
    if (config_len == 0 || (config_len % 2u) != 0 ||
        config_len / 2u > buffer_size) {
        return -1;
    }

    for (i = 0; i < config_len; ++i) {
        if (turbo_rtsp_sdp_hex_value(mpeg4->config[i]) < 0) {
            return -1;
        }
    }

    for (i = 0; i < config_len; i += 2u) {
        int high = turbo_rtsp_sdp_hex_value(mpeg4->config[i]);
        int low = turbo_rtsp_sdp_hex_value(mpeg4->config[i + 1u]);
        buffer[i / 2u] = (uint8_t)((high << 4) | low);
    }

    *written = config_len / 2u;
    return 0;
}

typedef struct {
    int payload_type;
    const char *encoding_name;
    int clock_rate;
    int encoding_parameters;
} turbo_rtsp_sdp_static_payload_profile_t;

static const turbo_rtsp_sdp_static_payload_profile_t
    turbo_rtsp_sdp_static_payload_profiles[] = {
        {0, "PCMU", 8000, 1},
        {3, "GSM", 8000, 1},
        {4, "G723", 8000, 1},
        {5, "DVI4", 8000, 1},
        {6, "DVI4", 16000, 1},
        {7, "LPC", 8000, 1},
        {8, "PCMA", 8000, 1},
        {9, "G722", 8000, 1},
        {10, "L16", 44100, 2},
        {11, "L16", 44100, 1},
        {12, "QCELP", 8000, 1},
        {13, "CN", 8000, 1},
        {14, "MPA", 90000, 2},
        {15, "G728", 8000, 1},
        {16, "DVI4", 11025, 1},
        {17, "DVI4", 22050, 1},
        {18, "G729", 8000, 1},
        {25, "CelB", 90000, 0},
        {26, "JPEG", 90000, 0},
        {28, "nv", 90000, 0},
        {31, "H261", 90000, 0},
        {32, "MPV", 90000, 0},
        {33, "MP2T", 90000, 0},
        {34, "H263", 90000, 0},
    };

static int turbo_rtsp_sdp_proto_is_rtp_avp(const char *proto) {
    size_t proto_len = proto ? strlen(proto) : 0;

    return turbo_rtsp_sdp_ascii_case_equal(proto, proto_len, "RTP/AVP") ||
           turbo_rtsp_sdp_ascii_case_equal(proto, proto_len, "RTP/AVP/UDP") ||
           turbo_rtsp_sdp_ascii_case_equal(proto, proto_len, "RTP/AVP/TCP");
}

static void turbo_rtsp_sdp_apply_static_payload_profile(
    turbo_rtsp_sdp_media_desc_t *media) {
    size_t i = 0;

    if (!media || !turbo_rtsp_sdp_proto_is_rtp_avp(media->proto)) {
        return;
    }

    for (i = 0;
         i < sizeof(turbo_rtsp_sdp_static_payload_profiles) /
                 sizeof(turbo_rtsp_sdp_static_payload_profiles[0]);
         ++i) {
        const turbo_rtsp_sdp_static_payload_profile_t *profile =
            &turbo_rtsp_sdp_static_payload_profiles[i];

        if (profile->payload_type == media->payload_type) {
            snprintf(
                media->encoding_name,
                sizeof(media->encoding_name),
                "%s",
                profile->encoding_name);
            media->clock_rate = profile->clock_rate;
            media->encoding_parameters = profile->encoding_parameters;
            return;
        }
    }
}

static int turbo_rtsp_sdp_parse_media_line(
    const char *value,
    const char *end,
    turbo_rtsp_sdp_media_desc_t *media) {
    const char *cursor = value;
    char port_buf[16];
    char pt_buf[16];

    if (!media) {
        return -1;
    }

    memset(media, 0, sizeof(*media));
    media->payload_type = -1;
    media->port = -1;
    if (turbo_rtsp_sdp_token(&cursor, end, media->media, sizeof(media->media)) != 0 ||
        turbo_rtsp_sdp_token(&cursor, end, port_buf, sizeof(port_buf)) != 0 ||
        turbo_rtsp_sdp_token(&cursor, end, media->proto, sizeof(media->proto)) != 0 ||
        turbo_rtsp_sdp_token(&cursor, end, pt_buf, sizeof(pt_buf)) != 0) {
        return -1;
    }

    if (turbo_rtsp_sdp_parse_int_range(
            port_buf,
            port_buf + strlen(port_buf),
            0,
            65535,
            &media->port) != 0 ||
        turbo_rtsp_sdp_parse_int_range(
            pt_buf,
            pt_buf + strlen(pt_buf),
            0,
            127,
            &media->payload_type) != 0) {
        return -1;
    }
    turbo_rtsp_sdp_apply_static_payload_profile(media);
    return 0;
}

static int turbo_rtsp_sdp_parse_rtpmap(
    const char *value,
    const char *end,
    turbo_rtsp_sdp_media_desc_t *media) {
    const char *cursor = value;
    const char *pt_end = NULL;
    const char *encoding_start = NULL;
    const char *encoding_end = NULL;
    const char *rate_start = NULL;
    const char *parameters_start = NULL;
    char pt_buf[16];
    char rate_buf[16];
    char parameters_buf[16];
    int payload_type = 0;

    if (!media) {
        return -1;
    }

    pt_end = memchr(cursor, ' ', (size_t)(end - cursor));
    if (!pt_end) {
        return -1;
    }
    if (turbo_rtsp_sdp_copy_range(pt_buf, sizeof(pt_buf), cursor, pt_end) != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_parse_int_range(
            pt_buf,
            pt_buf + strlen(pt_buf),
            0,
            127,
            &payload_type) != 0) {
        return -1;
    }
    if (payload_type != media->payload_type) {
        return 0;
    }

    cursor = pt_end + 1;
    encoding_start = cursor;
    while (cursor < end && *cursor != '/') {
        ++cursor;
    }
    if (cursor >= end || cursor == encoding_start) {
        return -1;
    }
    encoding_end = cursor;
    rate_start = cursor + 1;
    cursor = rate_start;
    while (cursor < end && *cursor != '/') {
        ++cursor;
    }
    if (rate_start >= cursor) {
        return -1;
    }
    if (cursor < end && *cursor == '/') {
        parameters_start = cursor + 1;
        if (parameters_start >= end) {
            return -1;
        }
    }

    if (turbo_rtsp_sdp_copy_range(
            media->encoding_name,
            sizeof(media->encoding_name),
            encoding_start,
            encoding_end) != 0 ||
        turbo_rtsp_sdp_copy_range(rate_buf, sizeof(rate_buf), rate_start, cursor) != 0) {
        return -1;
    }

    if (turbo_rtsp_sdp_parse_int_range(
            rate_buf,
            rate_buf + strlen(rate_buf),
            1,
            INT_MAX,
            &media->clock_rate) != 0) {
        return -1;
    }

    media->encoding_parameters = 0;
    if (parameters_start) {
        if (turbo_rtsp_sdp_copy_range(
                parameters_buf,
                sizeof(parameters_buf),
                parameters_start,
                end) != 0 ||
            turbo_rtsp_sdp_parse_int_range(
                parameters_buf,
                parameters_buf + strlen(parameters_buf),
                1,
                INT_MAX,
                &media->encoding_parameters) != 0) {
            return -1;
        }
    }

    return 0;
}

static int turbo_rtsp_sdp_parse_fmtp(
    const char *value,
    const char *end,
    turbo_rtsp_sdp_media_desc_t *media) {
    const char *cursor = value;
    const char *pt_end = NULL;
    const char *fmtp_start = NULL;
    char pt_buf[16];
    int payload_type = 0;

    if (!media) {
        return -1;
    }

    pt_end = memchr(cursor, ' ', (size_t)(end - cursor));
    if (!pt_end) {
        return -1;
    }
    if (turbo_rtsp_sdp_copy_range(pt_buf, sizeof(pt_buf), cursor, pt_end) != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_parse_int_range(
            pt_buf,
            pt_buf + strlen(pt_buf),
            0,
            127,
            &payload_type) != 0) {
        return -1;
    }
    if (payload_type != media->payload_type) {
        return 0;
    }

    fmtp_start = pt_end + 1;
    turbo_rtsp_sdp_trim_line(&fmtp_start, &end);
    if (fmtp_start >= end) {
        return -1;
    }

    if (turbo_rtsp_sdp_copy_range(media->fmtp, sizeof(media->fmtp), fmtp_start, end) != 0) {
        return -1;
    }
    if (turbo_rtsp_sdp_ascii_case_equal(
            media->encoding_name,
            strlen(media->encoding_name),
            "H264")) {
        if (turbo_rtsp_sdp_h264_fmtp_parse(
                media->fmtp,
                0,
                &media->h264_fmtp) != 0) {
            memset(&media->h264_fmtp, 0, sizeof(media->h264_fmtp));
        }
    }
    if (turbo_rtsp_sdp_ascii_case_equal(
            media->encoding_name,
            strlen(media->encoding_name),
            "H265")) {
        if (turbo_rtsp_sdp_h265_fmtp_parse(
                media->fmtp,
                0,
                &media->h265_fmtp) != 0) {
            memset(&media->h265_fmtp, 0, sizeof(media->h265_fmtp));
        }
    }
    if (turbo_rtsp_sdp_ascii_case_equal(
            media->encoding_name,
            strlen(media->encoding_name),
            "MPEG4-GENERIC") ||
        turbo_rtsp_sdp_ascii_case_equal(
            media->encoding_name,
            strlen(media->encoding_name),
            "MP4A-LATM")) {
        if (turbo_rtsp_sdp_mpeg4_fmtp_parse(
                media->fmtp,
                0,
                &media->mpeg4_fmtp) != 0) {
            memset(&media->mpeg4_fmtp, 0, sizeof(media->mpeg4_fmtp));
        }
    }
    return 0;
}

static int turbo_rtsp_sdp_is_direction(
    const char *value,
    const char *end) {
    const size_t len = (size_t)(end - value);

    return (len == strlen("sendrecv") && memcmp(value, "sendrecv", len) == 0) ||
           (len == strlen("sendonly") && memcmp(value, "sendonly", len) == 0) ||
           (len == strlen("recvonly") && memcmp(value, "recvonly", len) == 0) ||
           (len == strlen("inactive") && memcmp(value, "inactive", len) == 0);
}

int turbo_rtsp_sdp_parse(
    const char *sdp,
    size_t sdp_len,
    turbo_rtsp_sdp_description_t *description) {
    const char *cursor = sdp;
    const char *end = NULL;
    turbo_rtsp_sdp_media_desc_t *current_media = NULL;

    if (!sdp || !description) {
        return -1;
    }

    if (sdp_len == 0) {
        sdp_len = strlen(sdp);
    }
    end = sdp + sdp_len;
    memset(description, 0, sizeof(*description));

    while (cursor < end) {
        const char *line_start = cursor;
        const char *line_end = NULL;
        const char *value = NULL;
        const char *value_end = NULL;

        while (cursor < end && *cursor != '\n') {
            ++cursor;
        }
        line_end = cursor;
        if (cursor < end && *cursor == '\n') {
            ++cursor;
        }

        turbo_rtsp_sdp_trim_line(&line_start, &line_end);
        if (line_end - line_start < 2 || line_start[1] != '=') {
            continue;
        }

        value = line_start + 2;
        value_end = line_end;
        turbo_rtsp_sdp_trim_line(&value, &value_end);

        if (line_start[0] == 's') {
            if (turbo_rtsp_sdp_copy_range(
                    description->session_name,
                    sizeof(description->session_name),
                    value,
                    value_end) != 0) {
                return -1;
            }
        } else if (line_start[0] == 'c') {
            const char *addr = value_end;
            char *connection_address = current_media
                                           ? current_media->connection_address
                                           : description->connection_address;
            size_t connection_address_size = current_media
                                                 ? sizeof(current_media->connection_address)
                                                 : sizeof(description->connection_address);
            while (addr > value && addr[-1] != ' ' && addr[-1] != '\t') {
                --addr;
            }
            if (addr < value_end &&
                turbo_rtsp_sdp_copy_range(
                    connection_address,
                    connection_address_size,
                    addr,
                    value_end) != 0) {
                return -1;
            }
        } else if (line_start[0] == 'm') {
            if (description->media_count >= TURBO_RTSP_SDP_MAX_MEDIA) {
                return -1;
            }
            current_media = &description->media[description->media_count++];
            if (turbo_rtsp_sdp_parse_media_line(value, value_end, current_media) != 0) {
                return -1;
            }
        } else if (line_start[0] == 'a') {
            if ((size_t)(value_end - value) > strlen("control:") &&
                memcmp(value, "control:", strlen("control:")) == 0) {
                const char *control = value + strlen("control:");
                if (current_media) {
                    if (turbo_rtsp_sdp_copy_range(
                            current_media->control,
                            sizeof(current_media->control),
                            control,
                            value_end) != 0) {
                        return -1;
                    }
                } else if (turbo_rtsp_sdp_copy_range(
                               description->control,
                               sizeof(description->control),
                               control,
                               value_end) != 0) {
                    return -1;
                }
            } else if ((size_t)(value_end - value) > strlen("range:") &&
                       memcmp(value, "range:", strlen("range:")) == 0) {
                const char *range = value + strlen("range:");
                if (current_media) {
                    if (turbo_rtsp_sdp_copy_range(
                            current_media->range,
                            sizeof(current_media->range),
                            range,
                            value_end) != 0) {
                        return -1;
                    }
                } else if (turbo_rtsp_sdp_copy_range(
                               description->range,
                               sizeof(description->range),
                               range,
                               value_end) != 0) {
                    return -1;
                }
            } else if (turbo_rtsp_sdp_is_direction(value, value_end)) {
                if (current_media) {
                    if (turbo_rtsp_sdp_copy_range(
                            current_media->direction,
                            sizeof(current_media->direction),
                            value,
                            value_end) != 0) {
                        return -1;
                    }
                } else if (turbo_rtsp_sdp_copy_range(
                               description->direction,
                               sizeof(description->direction),
                               value,
                               value_end) != 0) {
                    return -1;
                }
            } else if ((size_t)(value_end - value) > strlen("rtpmap:") &&
                       memcmp(value, "rtpmap:", strlen("rtpmap:")) == 0 &&
                       current_media) {
                if (turbo_rtsp_sdp_parse_rtpmap(
                        value + strlen("rtpmap:"),
                        value_end,
                        current_media) != 0) {
                    return -1;
                }
            } else if ((size_t)(value_end - value) > strlen("fmtp:") &&
                       memcmp(value, "fmtp:", strlen("fmtp:")) == 0 &&
                       current_media) {
                if (turbo_rtsp_sdp_parse_fmtp(
                        value + strlen("fmtp:"),
                        value_end,
                        current_media) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

#endif /* TURBO_MEDIA_HAS_RTSP */
