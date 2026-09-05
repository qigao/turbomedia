#include "ivr_http_media_client.h"

#include <turbo_http.h>

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define IVR_OPUS_RTP_CLOCK_RATE 48000

#define IVR_HTTP_MEDIA_DEFAULT_TIMEOUT_MS 5000u
#define IVR_HTTP_MEDIA_MAX_BASE_URL 2048u
#define IVR_HTTP_MEDIA_MAX_URL 4096u
#define IVR_HTTP_MEDIA_MAX_HEADER_LINE 1024u
#define IVR_HTTP_MEDIA_MAX_RESPONSE_HEADERS 8192u

struct ivr_http_media_client_s {
    turbo_http_t *http;
    char *base_url;
};

char *ivr_http_media_strdup(const char *value) {
    size_t length;
    char *copy;

    if (!value) {
        return NULL;
    }
    length = strlen(value);
    copy = (char *)malloc(length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, length + 1);
    return copy;
}

static int ivr_http_media_path_byte_unreserved(unsigned char value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

char *ivr_http_media_encode_path_segment(const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *input = (const unsigned char *)value;
    size_t input_length;
    size_t input_offset;
    size_t output_offset = 0;
    char *encoded;

    if (!value) {
        return NULL;
    }
    input_length = strlen(value);
    if (input_length > (SIZE_MAX - 1u) / 3u) {
        return NULL;
    }
    for (input_offset = 0; input_offset < input_length; ++input_offset) {
        if (input[input_offset] < 0x20u || input[input_offset] == 0x7fu) {
            return NULL;
        }
    }
    encoded = (char *)malloc(input_length * 3u + 1u);
    if (!encoded) {
        return NULL;
    }
    for (input_offset = 0; input_offset < input_length; ++input_offset) {
        unsigned char byte = input[input_offset];
        if (ivr_http_media_path_byte_unreserved(byte)) {
            encoded[output_offset++] = (char)byte;
        } else {
            encoded[output_offset++] = '%';
            encoded[output_offset++] = hex[byte >> 4u];
            encoded[output_offset++] = hex[byte & 0x0fu];
        }
    }
    encoded[output_offset] = '\0';
    return encoded;
}

static int ivr_http_media_has_forbidden_byte(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    if (!value) {
        return 0;
    }
    while (*cursor) {
        if (*cursor <= 0x20u || *cursor == 0x7fu) {
            return 1;
        }
        ++cursor;
    }
    return 0;
}

static int ivr_http_media_authority_is_loopback(const char *authority,
                                                size_t length) {
    size_t host_length = length;
    const char *colon;

    if (!authority || length == 0 || memchr(authority, '@', length)) {
        return 0;
    }
    if (authority[0] == '[') {
        const char *closing = (const char *)memchr(authority, ']', length);
        if (!closing || (size_t)(closing - authority) != 4u ||
            memcmp(authority, "[::1]", 5u) != 0) {
            return 0;
        }
        return (size_t)(closing - authority + 1) == length ||
               closing[1] == ':';
    }
    colon = (const char *)memchr(authority, ':', length);
    if (colon) {
        host_length = (size_t)(colon - authority);
    }
    return (host_length == 9u && memcmp(authority, "localhost", 9u) == 0) ||
           (host_length == 9u && memcmp(authority, "127.0.0.1", 9u) == 0);
}

static int ivr_http_media_validate_base_url(const char *base_url,
                                            int allow_plaintext_loopback,
                                            int *out_https) {
    const char *authority;
    const char *authority_end;
    size_t length;
    int https;

    if (!base_url || !out_https || ivr_http_media_has_forbidden_byte(base_url)) {
        return -1;
    }
    length = strlen(base_url);
    if (length == 0 || length > IVR_HTTP_MEDIA_MAX_BASE_URL ||
        strchr(base_url, '?') || strchr(base_url, '#')) {
        return -1;
    }
    if (strncmp(base_url, "https://", 8u) == 0) {
        authority = base_url + 8u;
        https = 1;
    } else if (strncmp(base_url, "http://", 7u) == 0) {
        authority = base_url + 7u;
        https = 0;
    } else {
        return -1;
    }
    authority_end = strchr(authority, '/');
    if (!authority_end) {
        authority_end = base_url + length;
    }
    if (authority_end == authority || memchr(authority, '@',
                                              (size_t)(authority_end - authority))) {
        return -1;
    }
    if (!https &&
        (!allow_plaintext_loopback ||
         !ivr_http_media_authority_is_loopback(
             authority, (size_t)(authority_end - authority)))) {
        return -1;
    }
    *out_https = https;
    return 0;
}

static int ivr_http_media_copy_header(http_response_t *source,
                                      const char *name, char *destination,
                                      size_t capacity) {
    char *value = http_response_get_header(source, name);
    size_t length;

    if (!value) {
        destination[0] = '\0';
        return 0;
    }
    length = strlen(value);
    if (length >= capacity || ivr_http_media_has_forbidden_byte(value)) {
        free(value);
        return -1;
    }
    memcpy(destination, value, length + 1u);
    free(value);
    return 0;
}

static int ivr_http_media_method(const char *method, http_method_t *out_method) {
    if (!method || !out_method) {
        return -1;
    }
    if (strcmp(method, "GET") == 0) {
        *out_method = HTTP_GET;
    } else if (strcmp(method, "POST") == 0) {
        *out_method = HTTP_POST;
    } else if (strcmp(method, "PATCH") == 0) {
        *out_method = HTTP_PATCH;
    } else if (strcmp(method, "DELETE") == 0) {
        *out_method = HTTP_DELETE;
    } else {
        return -1;
    }
    return 0;
}

int ivr_http_media_client_create(
    const ivr_http_media_client_config_t *config,
    ivr_http_media_client_t **out_client) {
    turbo_http_options_t options;
    turbo_tls_client_config_t tls;
    ivr_http_media_client_t *client;
    size_t base_url_length;
    int https;

    if (!config || config->size < sizeof(*config) || !out_client) {
        return -1;
    }
    *out_client = NULL;
    if (config->timeout_ms > (uint64_t)INT64_MAX ||
        ivr_http_media_validate_base_url(config->base_url,
                                         config->allow_plaintext_loopback,
                                         &https) != 0) {
        return -1;
    }
    client = (ivr_http_media_client_t *)calloc(1, sizeof(*client));
    if (!client) {
        return -1;
    }
    base_url_length = strlen(config->base_url);
    while (base_url_length > 0u && config->base_url[base_url_length - 1u] == '/') {
        --base_url_length;
    }
    client->base_url = (char *)malloc(base_url_length + 1u);
    if (!client->base_url) {
        free(client);
        return -1;
    }
    memcpy(client->base_url, config->base_url, base_url_length);
    client->base_url[base_url_length] = '\0';

    if (turbo_http_options_init(&options, sizeof(options)) != SALTS_OK) {
        ivr_http_media_client_destroy(client);
        return -1;
    }
    options.transport = https ? TURBO_HTTP_TRANSPORT_AUTO
                              : TURBO_HTTP_TRANSPORT_H1;
    options.follow_redirects = 0;
    options.timeout_ms = config->timeout_ms
                             ? (int64_t)config->timeout_ms
                             : (int64_t)IVR_HTTP_MEDIA_DEFAULT_TIMEOUT_MS;
    if (turbo_http_create_sync(&options, &client->http) != SALTS_OK) {
        ivr_http_media_client_destroy(client);
        return -1;
    }
    turbo_http_set_max_response_size(client->http, IVR_HTTP_MEDIA_MAX_RESPONSE);
    turbo_http_set_max_response_header_size(
        client->http, IVR_HTTP_MEDIA_MAX_RESPONSE_HEADERS);
    if (config->media_token && config->media_token[0] != '\0' &&
        turbo_http_set_bearer_token(client->http, config->media_token) !=
            SALTS_OK) {
        ivr_http_media_client_destroy(client);
        return -1;
    }
    if (https) {
        memset(&tls, 0, sizeof(tls));
        tls.ca_file = config->ca_file;
        tls.cert_file = config->cert_file;
        tls.key_file = config->key_file;
        tls.key_password = config->key_password;
        tls.verify_peer = 1;
        if (turbo_http_set_tls_config(client->http, &tls) != SALTS_OK) {
            ivr_http_media_client_destroy(client);
            return -1;
        }
    }
    *out_client = client;
    return 0;
}

void ivr_http_media_client_destroy(ivr_http_media_client_t *client) {
    if (!client) {
        return;
    }
    turbo_http_destroy(client->http);
    free(client->base_url);
    free(client);
}

int ivr_http_media_request(ivr_http_media_client_t *client,
                           const char *method, const char *path,
                           const char *content_type, const char *if_match,
                           const char *body,
                           ivr_http_media_response_t *response) {
    const char *headers[2];
    char content_type_header[IVR_HTTP_MEDIA_MAX_HEADER_LINE];
    char if_match_header[IVR_HTTP_MEDIA_MAX_HEADER_LINE];
    char url[IVR_HTTP_MEDIA_MAX_URL];
    http_response_t *raw_response;
    http_method_t http_method;
    size_t body_length = body ? strlen(body) : 0u;
    int header_count = 0;
    int url_length;
    int result = -1;

    if (!client || !client->http || !method || !path || path[0] != '/' ||
        ivr_http_media_has_forbidden_byte(path) || !response ||
        ivr_http_media_method(method, &http_method) != 0) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (content_type) {
        int written;
        if (ivr_http_media_has_forbidden_byte(content_type)) {
            return -1;
        }
        written = snprintf(content_type_header, sizeof(content_type_header),
                           "Content-Type: %s", content_type);
        if (written < 0 || (size_t)written >= sizeof(content_type_header)) {
            return -1;
        }
        headers[header_count++] = content_type_header;
    }
    if (if_match) {
        int written;
        if (ivr_http_media_has_forbidden_byte(if_match)) {
            return -1;
        }
        written = snprintf(if_match_header, sizeof(if_match_header),
                           "If-Match: %s", if_match);
        if (written < 0 || (size_t)written >= sizeof(if_match_header)) {
            return -1;
        }
        headers[header_count++] = if_match_header;
    }
    url_length = snprintf(url, sizeof(url), "%s%s", client->base_url, path);
    if (url_length < 0 || (size_t)url_length >= sizeof(url)) {
        return -1;
    }
    raw_response = turbo_http_request_sync(
        client->http, http_method, url, headers, header_count, body, body_length);
    if (!raw_response) {
        return -1;
    }
    if (raw_response->error_code == HTTP_ERROR_NONE &&
        raw_response->body_len < sizeof(response->body) &&
        ivr_http_media_copy_header(raw_response, "Location",
                                   response->location,
                                   sizeof(response->location)) == 0 &&
        ivr_http_media_copy_header(raw_response, "ETag", response->etag,
                                   sizeof(response->etag)) == 0) {
        response->status = raw_response->status_code;
        if (raw_response->body_len > 0u && raw_response->body) {
            memcpy(response->body, raw_response->body, raw_response->body_len);
        }
        response->body[raw_response->body_len] = '\0';
        result = 0;
    }
    http_response_free(raw_response);
    return result;
}

int ivr_sdp_attr_value(const char *sdp, const char *attribute, char *out,
                       size_t capacity) {
    const char *value = sdp && attribute ? strstr(sdp, attribute) : NULL;
    size_t length = 0;

    if (!value || !out || capacity == 0) {
        return 0;
    }
    value += strlen(attribute);
    while (value[length] && value[length] != '\r' && value[length] != '\n' &&
           length + 1 < capacity) {
        out[length] = value[length];
        length++;
    }
    out[length] = '\0';
    return length > 0;
}

static void ivr_sdp_append_prefixed_lines(const char *sdp,
                                          const char *prefix, char *out,
                                          size_t capacity) {
    size_t used = 0;
    const char *line = sdp;
    size_t prefix_length;

    if (!sdp || !prefix || !out || capacity == 0) {
        return;
    }
    prefix_length = strlen(prefix);
    while (line && *line && used + 1 < capacity) {
        const char *end = strstr(line, "\r\n");
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length >= prefix_length &&
            strncmp(line, prefix, prefix_length) == 0) {
            if (used + length + 3 > capacity) {
                break;
            }
            memcpy(out + used, line, length);
            used += length;
            memcpy(out + used, "\r\n", 2);
            used += 2;
        }
        if (!end) {
            break;
        }
        line = end + 2;
    }
    out[used] = '\0';
}

void ivr_sdp_build_minimal_audio_offer(const char *source, char *out,
                                       size_t capacity, int sample_rate,
                                       const char *direction) {
    char ufrag[64];
    char password[96];
    char fingerprint[256];
    char ssrc_lines[512];
    int written;

    (void)sample_rate;

    if (!source || !out || capacity == 0 || !direction ||
        !ivr_sdp_attr_value(source, "a=ice-ufrag:", ufrag, sizeof(ufrag)) ||
        !ivr_sdp_attr_value(source, "a=ice-pwd:", password,
                            sizeof(password)) ||
        !ivr_sdp_attr_value(source, "a=fingerprint:sha-256 ", fingerprint,
                            sizeof(fingerprint))) {
        if (source && out && capacity > 0) {
            snprintf(out, capacity, "%s", source);
        }
        return;
    }
    ivr_sdp_append_prefixed_lines(source, "a=ssrc:", ssrc_lines,
                                  sizeof(ssrc_lines));
    written = snprintf(
        out, capacity,
        "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 126\r\nc=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n"
        "a=fingerprint:sha-256 %s\r\n"
        "a=setup:actpass\r\na=mid:0\r\na=%s\r\n"
        "a=rtcp-mux\r\na=rtpmap:111 opus/%d/2\r\n"
        "a=rtpmap:126 telephone-event/8000\r\n"
        "a=fmtp:126 0-16\r\n%s",
        ufrag, password, fingerprint, direction, IVR_OPUS_RTP_CLOCK_RATE,
        ssrc_lines);
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
    }
}

void ivr_sdp_append_candidates(const char *offer, char *out, size_t capacity) {
    ivr_sdp_append_prefixed_lines(offer, "a=candidate:", out, capacity);
}
