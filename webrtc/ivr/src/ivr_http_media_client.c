#include "ivr_http_media_client.h"

#include <chttp/chttp.h>
#include <salts/error_codes.h>

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define IVR_OPUS_RTP_CLOCK_RATE 48000

#define IVR_HTTP_MEDIA_DEFAULT_TIMEOUT_MS 5000u
#define IVR_HTTP_MEDIA_MAX_BASE_URL 2048u
#define IVR_HTTP_MEDIA_MAX_URL 4096u
#define IVR_HTTP_MEDIA_MAX_RESPONSE_HEADERS 8192u

enum {
    IVR_HTTP_MEDIA_QUEUE_CAPACITY = 8,
    IVR_HTTP_MEDIA_MAX_HEADER_COUNT = 16,
    IVR_HTTP_MEDIA_STOP_TIMEOUT_MS = 5000
};

struct ivr_http_media_client_s {
    chttp_client http;
    chttp_client_config http_config;
    chttp_tls_profile tls;
    int http_initialized;
    int tls_initialized;
    uint32_t timeout_ms;
    char *connection_uri;
    char *authority;
    char *base_path;
    char *authorization;
};

static native_io_backend_kind ivr_http_media_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

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

static int ivr_http_media_copy_header(const chttp_response *source,
                                      const char *name, char *destination,
                                      size_t capacity) {
    const char *value = chttp_response_header(source, name);
    size_t length;

    if (!value) {
        destination[0] = '\0';
        return 0;
    }
    length = strlen(value);
    if (length >= capacity || ivr_http_media_has_forbidden_byte(value)) {
        return -1;
    }
    memcpy(destination, value, length + 1u);
    return 0;
}

static int ivr_http_media_method(const char *method, chttp_method *out_method) {
    if (!method || !out_method) {
        return -1;
    }
    if (strcmp(method, "GET") == 0) {
        *out_method = CHTTP_METHOD_GET;
    } else if (strcmp(method, "POST") == 0) {
        *out_method = CHTTP_METHOD_POST;
    } else if (strcmp(method, "PATCH") == 0) {
        *out_method = CHTTP_METHOD_PATCH;
    } else if (strcmp(method, "DELETE") == 0) {
        *out_method = CHTTP_METHOD_DELETE;
    } else {
        return -1;
    }
    return 0;
}

static int ivr_http_media_send(chttp_client *client, chttp_method method,
                               const chttp_options *options,
                               chttp_response *response,
                               chttp_error *error) {
    switch (method) {
        case CHTTP_METHOD_GET:
            return chttp_get(client, options, response, error);
        case CHTTP_METHOD_POST:
            return chttp_post(client, options, response, error);
        case CHTTP_METHOD_PATCH:
            return chttp_patch(client, options, response, error);
        case CHTTP_METHOD_DELETE:
            return chttp_delete(client, options, response, error);
        default:
            return SALTS_EINVAL;
    }
}

static int ivr_http_media_retry_stale_delete(chttp_method method, int status,
                                              const chttp_error *error) {
    return method == CHTTP_METHOD_DELETE && status == SALTS_EPROTO && error &&
           error->stage && strcmp(error->stage, "eof") == 0;
}

static int ivr_http_media_reset_http(ivr_http_media_client_t *client) {
    int status;
    if (!client) {
        return SALTS_EINVAL;
    }
    if (client->http_initialized) {
        status = chttp_client_destroy(&client->http,
                                      IVR_HTTP_MEDIA_STOP_TIMEOUT_MS);
        if (status != SALTS_OK) {
            return status;
        }
        memset(&client->http, 0, sizeof(client->http));
        client->http_initialized = 0;
    }
    status = chttp_client_init(&client->http, &client->http_config);
    if (status == SALTS_OK) {
        client->http_initialized = 1;
    }
    return status;
}

int ivr_http_media_client_create(
    const ivr_http_media_client_config_t *config,
    ivr_http_media_client_t **out_client) {
    chttp_client_config http_config = {0};
    cnet_tls_client_config tls_config = {0};
    ivr_http_media_client_t *client;
    const char *authority_start;
    const char *authority_end;
    const char *base_path;
    const char *connection_scheme;
    size_t authority_length;
    size_t base_path_length;
    size_t connection_scheme_length;
    size_t connection_uri_length;
    uint64_t timeout_ms;
    int https;

    if (!config || config->size < sizeof(*config) || !out_client) {
        return -1;
    }
    *out_client = NULL;
    if (ivr_http_media_validate_base_url(config->base_url,
                                         config->allow_plaintext_loopback,
                                         &https) != 0) {
        return -1;
    }
    client = (ivr_http_media_client_t *)calloc(1, sizeof(*client));
    if (!client) {
        return -1;
    }
    timeout_ms = config->timeout_ms > 0u
                     ? config->timeout_ms
                     : IVR_HTTP_MEDIA_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > UINT32_MAX) {
        free(client);
        return -1;
    }
    client->timeout_ms = (uint32_t)timeout_ms;
    authority_start = config->base_url + (https ? 8u : 7u);
    authority_end = strchr(authority_start, '/');
    if (!authority_end) {
        authority_end = config->base_url + strlen(config->base_url);
    }
    authority_length = (size_t)(authority_end - authority_start);
    base_path = authority_end;
    base_path_length = strlen(base_path);
    while (base_path_length > 0u && base_path[base_path_length - 1u] == '/') {
        --base_path_length;
    }
    connection_scheme = https ? "tls://" : "tcp://";
    connection_scheme_length = strlen(connection_scheme);
    if (authority_length > SIZE_MAX - connection_scheme_length) {
        free(client);
        return -1;
    }
    connection_uri_length = connection_scheme_length + authority_length;
    client->connection_uri = (char *)malloc(connection_uri_length + 1u);
    client->authority = (char *)malloc(authority_length + 1u);
    client->base_path = (char *)malloc(base_path_length + 1u);
    if (!client->connection_uri || !client->authority || !client->base_path) {
        ivr_http_media_client_destroy(client);
        return -1;
    }
    memcpy(client->connection_uri, connection_scheme, connection_scheme_length);
    memcpy(client->connection_uri + connection_scheme_length, authority_start,
           authority_length);
    client->connection_uri[connection_uri_length] = '\0';
    memcpy(client->authority, authority_start, authority_length);
    client->authority[authority_length] = '\0';
    memcpy(client->base_path, base_path, base_path_length);
    client->base_path[base_path_length] = '\0';
    if (config->media_token && config->media_token[0]) {
        static const char bearer_prefix[] = "Bearer ";
        size_t token_length = strlen(config->media_token);
        if (token_length > SIZE_MAX - sizeof(bearer_prefix)) {
            ivr_http_media_client_destroy(client);
            return -1;
        }
        client->authorization =
            (char *)malloc(sizeof(bearer_prefix) + token_length);
        if (!client->authorization) {
            ivr_http_media_client_destroy(client);
            return -1;
        }
        memcpy(client->authorization, bearer_prefix,
               sizeof(bearer_prefix) - 1u);
        memcpy(client->authorization + sizeof(bearer_prefix) - 1u,
               config->media_token, token_length + 1u);
    }

    http_config.network = (cnet_client_config){
        .backend = ivr_http_media_backend(),
        .connection_capacity = 1u,
        .command_capacity = IVR_HTTP_MEDIA_QUEUE_CAPACITY,
        .request_capacity = IVR_HTTP_MEDIA_QUEUE_CAPACITY,
        .completion_batch_capacity = IVR_HTTP_MEDIA_QUEUE_CAPACITY,
        .event_capacity = IVR_HTTP_MEDIA_QUEUE_CAPACITY,
        .max_send_bytes = IVR_HTTP_MEDIA_MAX_SDP,
        .receive_buffer_bytes = IVR_HTTP_MEDIA_MAX_RESPONSE_HEADERS,
        .connect_timeout_ms = client->timeout_ms,
        .read_timeout_ms = client->timeout_ms,
        .write_timeout_ms = client->timeout_ms,
        .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
        .tls_handshake_timeout_ms = client->timeout_ms,
        .command_buffer_bytes = IVR_HTTP_MEDIA_MAX_SDP,
        .event_buffer_bytes = IVR_HTTP_MEDIA_MAX_RESPONSE
    };
    http_config.request_capacity = 1u;
    http_config.max_start_line_bytes = IVR_HTTP_MEDIA_MAX_URL;
    http_config.max_header_count = IVR_HTTP_MEDIA_MAX_HEADER_COUNT;
    http_config.max_header_bytes = IVR_HTTP_MEDIA_MAX_RESPONSE_HEADERS;
    http_config.max_request_body_bytes = IVR_HTTP_MEDIA_MAX_SDP;
    http_config.max_response_body_bytes = IVR_HTTP_MEDIA_MAX_RESPONSE - 1u;
    http_config.max_informational_responses = 4u;
    client->http_config = http_config;
    if (ivr_http_media_reset_http(client) != SALTS_OK) {
        ivr_http_media_client_destroy(client);
        return -1;
    }
    if (https) {
        tls_config.size = sizeof(tls_config);
        tls_config.ca_file = config->ca_file;
        tls_config.cert_file = config->cert_file;
        tls_config.key_file = config->key_file;
        tls_config.key_password = config->key_password;
        tls_config.server_name = config->server_name;
        if (chttp_tls_profile_init(&client->tls, &tls_config) != SALTS_OK) {
            ivr_http_media_client_destroy(client);
            return -1;
        }
        client->tls_initialized = 1;
    }
    *out_client = client;
    return 0;
}

void ivr_http_media_client_destroy(ivr_http_media_client_t *client) {
    if (!client) {
        return;
    }
    if (client->http_initialized) {
        (void)chttp_client_destroy(&client->http,
                                   IVR_HTTP_MEDIA_STOP_TIMEOUT_MS);
    }
    if (client->tls_initialized) {
        (void)chttp_tls_profile_destroy(&client->tls);
    }
    free(client->authorization);
    free(client->base_path);
    free(client->authority);
    free(client->connection_uri);
    free(client);
}

int ivr_http_media_request(ivr_http_media_client_t *client,
                           const char *method, const char *path,
                           const char *content_type, const char *if_match,
                           const char *body,
                           ivr_http_media_response_t *response) {
    chttp_header headers[3];
    char target[IVR_HTTP_MEDIA_MAX_URL];
    chttp_response raw_response = {0};
    chttp_error error = {0};
    chttp_options options = {0};
    chttp_method http_method;
    size_t body_length = body ? strlen(body) : 0u;
    size_t header_count = 0;
    int target_length;
    int status;
    int result = -1;

    if (!client || !client->http_initialized || !method || !path || path[0] != '/' ||
        ivr_http_media_has_forbidden_byte(path) || !response ||
        ivr_http_media_method(method, &http_method) != 0) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (content_type) {
        if (ivr_http_media_has_forbidden_byte(content_type)) {
            return -1;
        }
        headers[header_count++] = (chttp_header){"Content-Type", content_type};
    }
    if (if_match) {
        if (ivr_http_media_has_forbidden_byte(if_match)) {
            return -1;
        }
        headers[header_count++] = (chttp_header){"If-Match", if_match};
    }
    if (client->authorization) {
        headers[header_count++] =
            (chttp_header){"Authorization", client->authorization};
    }
    target_length = snprintf(target, sizeof(target), "%s%s",
                             client->base_path, path);
    if (target_length < 0 || (size_t)target_length >= sizeof(target)) {
        return -1;
    }
    options.connection_uri = client->connection_uri;
    options.authority = client->authority;
    options.target = target;
    options.headers = header_count ? headers : NULL;
    options.header_count = header_count;
    options.body = body;
    options.body_size = body_length;
    options.timeout_ms = client->timeout_ms;
    options.tls = client->tls_initialized ? &client->tls : NULL;
    options.protocol = CHTTP_HTTP_1_1;
    status = ivr_http_media_send(&client->http, http_method, &options,
                                 &raw_response, &error);
    if (ivr_http_media_retry_stale_delete(http_method, status, &error)) {
        /* A CHTTP H1 slot can observe EOF when the peer closed an idle
           keep-alive connection. DELETE is idempotent, so one immediate retry
           is safe even if the first request reached the peer; POST/PATCH are
           deliberately never retried here. */
        chttp_response_destroy(&raw_response);
        memset(&raw_response, 0, sizeof(raw_response));
        memset(&error, 0, sizeof(error));
        status = ivr_http_media_reset_http(client);
        if (status == SALTS_OK) {
            status = ivr_http_media_send(&client->http, http_method, &options,
                                         &raw_response, &error);
        }
    }
    if (status == SALTS_OK && raw_response.body_size < sizeof(response->body) &&
        ivr_http_media_copy_header(&raw_response, "Location",
                                   response->location,
                                   sizeof(response->location)) == 0 &&
        ivr_http_media_copy_header(&raw_response, "ETag", response->etag,
                                   sizeof(response->etag)) == 0 &&
        ivr_http_media_copy_header(&raw_response, "Content-Type",
                                   response->content_type,
                                   sizeof(response->content_type)) == 0) {
        response->status = (int)raw_response.status_code;
        if (raw_response.body_size > 0u && raw_response.body) {
            memcpy(response->body, raw_response.body, raw_response.body_size);
        }
        response->body[raw_response.body_size] = '\0';
        result = 0;
    } else if (status != SALTS_OK) {
        fprintf(stderr,
                "[ivr_http_media] request failed method=%s status=%d "
                "native_status=%d stage=%s\n",
                method, status, error.native_status,
                error.stage ? error.stage : "unknown");
    }
    chttp_response_destroy(&raw_response);
    status = ivr_http_media_reset_http(client);
    if (status != SALTS_OK) {
        fprintf(stderr,
                "[ivr_http_media] H1 client reset failed method=%s status=%d\n",
                method, status);
    }
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
