#include "sip-message.h"

static const char sip_version[] = "SIP/2.0";
static const char token_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.!%*_+`'~";

/* Header parsers consume C strings. Bound the header copy and reject embedded
 * NUL/control bytes before a parser can see a different span. The body is opaque. */
static size_t header_size(const unsigned char* packet, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        unsigned char c = packet[i];
        if (c == '\r') {
            if (i + 1 == bytes || packet[i + 1] != '\n')
                return 0;
            if (i + 3 < bytes && packet[i + 2] == '\r' && packet[i + 3] == '\n')
                return i + 4;
            ++i;
        } else if ((c < 32 && c != '\t') || c == 127) {
            return 0;
        }
    }
    return 0;
}

static void unfold_headers(char* output, const char* input, size_t bytes)
{
    const char* end = input + bytes;
    /* The start line cannot be folded. */
    const char* line_end = memchr(input, '\r', bytes);
    size_t start_bytes = (size_t)(line_end - input) + 2;
    memcpy(output, input, start_bytes);
    output += start_bytes;
    input += start_bytes;
    while (input < end) {
        if (end - input >= 3 && input[0] == '\r' && (input[2] == ' ' || input[2] == '\t')) {
            input += 2;
            do { ++input; } while (input < end && (*input == ' ' || *input == '\t'));
            *output++ = ' ';
        } else {
            *output++ = *input++;
        }
    }
    *output = '\0';
}

static int read_uint(const char** input, unsigned limit, unsigned* value)
{
    const char* p = *input;
    unsigned result = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (result > limit / 10 || (result == limit / 10 && digit > limit % 10))
            return -1;
        result = result * 10 + digit;
        ++p;
    }
    if (p == *input)
        return -1;
    *input = p;
    *value = result;
    return 0;
}

static int parse_start_line(struct sip_message_t* msg, char* line)
{
    char* space = strchr(line, ' ');
    if (!space)
        return -1;
    *space++ = '\0';

    if (msg->mode == SIP_MESSAGE_REPLY) {
        const char* reason = space;
        unsigned code;
        if (read_uint(&reason, 699, &code) || reason - space != 3 || *reason != ' ' || code < 100)
            return -1;
        msg->u.s.code = (int)code;
        msg->u.s.vermajor = 2;
        memcpy(msg->u.s.protocol, "SIP", sizeof("SIP"));
        ++reason;
        msg->ptr.ptr = sip_string_view_clone(msg->ptr.ptr, msg->ptr.end,
                                             &msg->u.s.reason, reason, strlen(reason));
        return msg->ptr.ptr == msg->ptr.end ? -1 : 0;
    }

    char* version = strchr(space, ' ');
    if (*line == '\0' || line[strspn(line, token_chars)] || !version ||
        strcmp(version + 1, sip_version) != 0)
        return -1;
    if ((size_t)(msg->ptr.end - msg->ptr.ptr) < strlen(line) + (size_t)(version - space) + 2)
        return -1;
    msg->ptr.ptr = sip_string_view_clone(msg->ptr.ptr, msg->ptr.end,
                                         &msg->u.c.method, line, strlen(line));
    vstr uri;
    msg->ptr.ptr = sip_string_view_clone(msg->ptr.ptr, msg->ptr.end, &uri, space, (size_t)(version - space));
    if (sip_header_uri(uri.data, uri.data + uri.len, &msg->u.c.uri))
        return -1;
    return sip_sv_valid(&msg->u.c.uri.scheme) && sip_sv_valid(&msg->u.c.uri.host) ? 0 : -1;
}

static int parse_header(struct sip_message_t* msg, char* line, int* content_length)
{
    char* delimiter = strchr(line, ':');
    if (!delimiter || delimiter == line)
        return -1;
    char* name_end = delimiter;
    while (name_end > line && (name_end[-1] == ' ' || name_end[-1] == '\t'))
        --name_end;
    *name_end = '\0';
    if (name_end == line || line[strspn(line, token_chars)])
        return -1;
    char* value = delimiter + 1;
    value += strspn(value, " \t");
    char* value_end = value + strlen(value);
    while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
        --value_end;
    *value_end = '\0';
    if (!sip_cstr_casecmp(line, "Content-Length") || !sip_cstr_casecmp(line, SIP_HEADER_ABBR_CONTENT_LENGTH)) {
        const char* end = value;
        unsigned length;
        if (*content_length >= 0 || read_uint(&end, SIP_DATAGRAM_MAX_BYTES, &length) || *end)
            return -1;
        *content_length = (int)length;
    }
    if (!sip_cstr_casecmp(line, SIP_HEADER_CSEQ)) {
        const char* method = value;
        unsigned sequence;
        if (sip_sv_valid(&msg->cseq.method) || read_uint(&method, INT32_MAX, &sequence) ||
            (*method != ' ' && *method != '\t'))
            return -1;
        method += strspn(method, " \t");
        if (!*method || method[strspn(method, token_chars)])
            return -1;
    }
    if ((!sip_cstr_casecmp(line, SIP_HEADER_CALLID) || !sip_cstr_casecmp(line, SIP_HEADER_ABBR_CALLID)) &&
        (!*value || sip_sv_valid(&msg->callid)))
        return -1;
    if ((!sip_cstr_casecmp(line, SIP_HEADER_FROM) || !sip_cstr_casecmp(line, SIP_HEADER_ABBR_FROM)) &&
        sip_sv_valid(&msg->from.uri.scheme))
        return -1;
    if ((!sip_cstr_casecmp(line, SIP_HEADER_TO) || !sip_cstr_casecmp(line, SIP_HEADER_ABBR_TO)) &&
        sip_sv_valid(&msg->to.uri.scheme))
        return -1;
    if ((size_t)(msg->ptr.end - msg->ptr.ptr) < (size_t)(name_end - line) + (size_t)(value_end - value) + 2)
        return -1;
    return sip_message_add_header(msg, line, value);
}

struct sip_message_t* sip_message_parse_datagram(const void* data, size_t bytes)
{
    char headers[SIP_DATAGRAM_MAX_BYTES + 1];
    int content_length = -1;
    if (!data || bytes > SIP_DATAGRAM_MAX_BYTES)
        return NULL;
    size_t header_bytes = header_size(data, bytes);
    if (!header_bytes)
        return NULL;

    unfold_headers(headers, data, header_bytes);
    int mode = strncmp(headers, "SIP/2.0 ", sizeof("SIP/2.0 ") - 1) == 0
        ? SIP_MESSAGE_REPLY : SIP_MESSAGE_REQUEST;
    struct sip_message_t* msg = sip_message_create(mode);
    if (!msg)
        return NULL;

    char* end = strstr(headers, "\r\n");
    *end = '\0';
    if (parse_start_line(msg, headers))
        goto invalid;
    for (char* line = end + 2; *line != '\r'; line = end + 2) {
        end = strstr(line, "\r\n");
        *end = '\0';
        if (parse_header(msg, line, &content_length))
            goto invalid;
    }

    const vstr* branch = sip_vias_top_branch(&msg->vias);
    if (!sip_sv_valid(branch) || !sip_sv_valid(&msg->from.uri.host) ||
        !sip_sv_valid(&msg->to.uri.host) || !sip_sv_valid(&msg->callid) ||
        !sip_sv_valid(&msg->cseq.method) ||
        (mode == SIP_MESSAGE_REQUEST && !sip_sv_equal(&msg->u.c.method, &msg->cseq.method)))
        goto invalid;
    size_t payload_bytes = bytes - header_bytes;
    /* RFC 3261 §18.3: UDP framing permits trailing bytes past the body. */
    if (content_length >= 0 && (size_t)content_length > payload_bytes)
        goto invalid;
    msg->payload = (const unsigned char*)data + header_bytes;
    msg->size = content_length >= 0 ? content_length : (int)payload_bytes;
    return msg;

invalid:
    sip_message_destroy(msg);
    return NULL;
}
