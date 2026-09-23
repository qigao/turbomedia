/** TurboMedia HTTP transport backed by Salts CHTTP. */
#include "turbo_transport.h"
#include "transport_internal.h"

#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HTTP_CONNECTION_CAPACITY = 2,
    HTTP_QUEUE_CAPACITY = 8,
    HTTP_MAX_HEADER_COUNT = 64,
    HTTP_MAX_HEADER_BYTES = 32 * 1024,
    HTTP_MAX_BODY_BYTES = 16 * 1024 * 1024,
    HTTP_MAX_START_LINE_BYTES = 8 * 1024,
    HTTP_STOP_TIMEOUT_MS = 5000,
    HTTP_DEFAULT_TIMEOUT_MS = 30000
};

typedef struct {
    turbo_transport_base_t base;
    chttp_client owned_client;
    chttp_tls_profile tls_profile;
    chttp_client *client;
    int owns_client;
    int tls_initialized;
    int connected;
    turbo_transport_event_cb event_callback;
    void *event_user_data;
    char error_msg[256];
    char *connection_uri;
    char *authority;
} http_transport_impl_t;

typedef struct { turbo_transport_read_cb callback; void *user; } upload_source_t;
typedef struct { turbo_transport_data_cb callback; void *user; } download_sink_t;

static http_transport_impl_t *http_transport_impl(turbo_transport_t *transport) {
    if (!transport ||
        turbo_transport_base(transport)->config.type != TURBO_TRANSPORT_HTTP)
        return NULL;
    return (http_transport_impl_t *)transport;
}

static char *http_dup(const char *value) {
    size_t size;
    char *copy;
    if (!value) return NULL;
    size = strlen(value) + 1;
    copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static void http_free_secret(const char **value) {
    char *owned;
    volatile unsigned char *bytes;
    size_t size;

    if (!value || !*value) return;
    owned = (char *)*value;
    size = strlen(owned);
    bytes = (volatile unsigned char *)owned;
    while (size > 0u) {
        *bytes++ = 0u;
        --size;
    }
    free(owned);
    *value = NULL;
}

static char *http_target(const http_transport_impl_t *transport,
                         const char *path) {
    const char *base = transport && transport->base.config.path
                           ? transport->base.config.path
                           : "";
    const char *suffix = path && path[0] ? path : "/";
    size_t base_size = strlen(base);
    size_t suffix_size = strlen(suffix);
    int base_is_root = base_size == 1u && base[0] == '/';
    int duplicate_slash = !base_is_root && base_size > 0u && suffix_size > 0u &&
                          base[base_size - 1u] == '/' && suffix[0] == '/';
    size_t size;
    char *target;

    if (base_is_root) base_size = 0u;
    if (base_size > HTTP_MAX_START_LINE_BYTES ||
        suffix_size > HTTP_MAX_START_LINE_BYTES - base_size)
        return NULL;
    size = base_size + suffix_size - (duplicate_slash ? 1u : 0u);
    if (size == 0u || size > HTTP_MAX_START_LINE_BYTES) return NULL;
    target = (char *)malloc(size + 1u);
    if (!target) return NULL;
    if (base_size) memcpy(target, base, base_size);
    memcpy(target + base_size, suffix + (duplicate_slash ? 1u : 0u),
           suffix_size - (duplicate_slash ? 1u : 0u));
    target[size] = '\0';
    return target;
}

static native_io_backend_kind http_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void set_error(http_transport_impl_t *transport,
                      const chttp_error *error, const char *fallback) {
    const char *stage = error && error->stage ? error->stage : fallback;
    snprintf(transport->error_msg, sizeof(transport->error_msg), "%s",
             stage ? stage : "CHTTP request failed");
}

static int format_endpoint(http_transport_impl_t *transport,
                           const turbo_transport_config_t *config) {
    char uri[512];
    char authority[320];
    int port = config->port > 0 ? config->port : (config->use_tls ? 443 : 80);
    int uri_size;
    int authority_size;
    const char *scheme = config->use_tls ? "tls" : "tcp";
    if (!config->host || !config->host[0]) return SALTS_EINVAL;
    if (strchr(config->host, ':')) {
        uri_size = snprintf(uri, sizeof(uri), "%s://[%s]:%d", scheme, config->host, port);
        authority_size = snprintf(authority, sizeof(authority), "[%s]:%d", config->host, port);
    } else {
        uri_size = snprintf(uri, sizeof(uri), "%s://%s:%d", scheme, config->host, port);
        authority_size = snprintf(authority, sizeof(authority), "%s:%d", config->host, port);
    }
    if (uri_size < 0 || (size_t)uri_size >= sizeof(uri) ||
        authority_size < 0 || (size_t)authority_size >= sizeof(authority))
        return SALTS_EMSGSIZE;
    transport->connection_uri = http_dup(uri);
    transport->authority = http_dup(authority);
    return transport->connection_uri && transport->authority ? SALTS_OK : SALTS_ENOMEM;
}

static chttp_client_config make_client_config(const turbo_transport_config_t *config) {
    chttp_client_config result = {0};
    uint32_t connect_timeout = config->connect_timeout_ms > 0
        ? (uint32_t)config->connect_timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;
    result.network = (cnet_client_config){
        .backend = http_backend(),
        .connection_capacity = HTTP_CONNECTION_CAPACITY,
        .command_capacity = HTTP_QUEUE_CAPACITY,
        .request_capacity = HTTP_QUEUE_CAPACITY,
        .completion_batch_capacity = HTTP_QUEUE_CAPACITY,
        .event_capacity = HTTP_QUEUE_CAPACITY,
        .max_send_bytes = HTTP_MAX_BODY_BYTES,
        .receive_buffer_bytes = HTTP_MAX_HEADER_BYTES,
        .connect_timeout_ms = connect_timeout,
        .read_timeout_ms = config->read_timeout_ms > 0
            ? (uint32_t)config->read_timeout_ms : HTTP_DEFAULT_TIMEOUT_MS,
        .write_timeout_ms = config->write_timeout_ms > 0
            ? (uint32_t)config->write_timeout_ms : HTTP_DEFAULT_TIMEOUT_MS,
        .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
        .tls_handshake_timeout_ms = connect_timeout,
        .command_buffer_bytes = HTTP_MAX_BODY_BYTES,
        .event_buffer_bytes = HTTP_MAX_BODY_BYTES
    };
    result.request_capacity = HTTP_QUEUE_CAPACITY;
    result.max_start_line_bytes = HTTP_MAX_START_LINE_BYTES;
    result.max_header_count = HTTP_MAX_HEADER_COUNT;
    result.max_header_bytes = HTTP_MAX_HEADER_BYTES;
    result.max_request_body_bytes = HTTP_MAX_BODY_BYTES;
    result.max_response_body_bytes = HTTP_MAX_BODY_BYTES;
    result.max_informational_responses = 4;
    return result;
}

static int upload_read(void *user, void *buffer, size_t capacity, size_t *out_size) {
    upload_source_t *source = (upload_source_t *)user;
    if (!source || !source->callback || !out_size) return SALTS_EINVAL;
    *out_size = source->callback((uint8_t *)buffer, capacity, source->user);
    return *out_size <= capacity ? SALTS_OK : SALTS_EMSGSIZE;
}

static int download_write(void *user, const void *data, size_t size) {
    download_sink_t *sink = (download_sink_t *)user;
    if (!sink || !sink->callback) return SALTS_EINVAL;
    sink->callback((const uint8_t *)data, size, sink->user);
    return SALTS_OK;
}

static chttp_response *execute_request(http_transport_impl_t *transport,
                                       turbo_http_method_t method, const char *path,
                                       const void *body, size_t body_size,
                                       const chttp_body_source *source,
                                       const chttp_body_sink *sink,
                                       const char **headers, int header_count) {
    chttp_header local_headers[HTTP_MAX_HEADER_COUNT];
    char authorization[1024];
    chttp_options options = {0};
    chttp_error error = {0};
    chttp_response *response;
    char *target;
    size_t count = 0;
    int status;
    if (!transport || !transport->client || header_count < 0 ||
        header_count > HTTP_MAX_HEADER_COUNT * 2 ||
        (header_count > 0 && (!headers || (header_count % 2) != 0))) return NULL;
    target = http_target(transport, path);
    if (!target) return NULL;
    for (int i = 0; i < header_count; i += 2)
        local_headers[count++] = (chttp_header){headers[i], headers[i + 1]};
    if (transport->base.config.user_agent && count < HTTP_MAX_HEADER_COUNT)
        local_headers[count++] = (chttp_header){"User-Agent", transport->base.config.user_agent};
    if (transport->base.config.auth_token && count < HTTP_MAX_HEADER_COUNT) {
        int written = snprintf(authorization, sizeof(authorization), "Bearer %s",
                               transport->base.config.auth_token);
        if (written < 0 || (size_t)written >= sizeof(authorization)) return NULL;
        local_headers[count++] = (chttp_header){"Authorization", authorization};
    }
    options = (chttp_options){
        .connection_uri = transport->connection_uri,
        .authority = transport->authority,
        .target = target,
        .headers = count ? local_headers : NULL,
        .header_count = count,
        .body = body,
        .body_size = body_size,
        .body_source = source,
        .body_sink = sink,
        .tls = transport->tls_initialized ? &transport->tls_profile : NULL,
        .protocol = CHTTP_HTTP_1_1,
        .timeout_ms = transport->base.config.read_timeout_ms > 0
            ? (uint32_t)transport->base.config.read_timeout_ms : HTTP_DEFAULT_TIMEOUT_MS
    };
    response = (chttp_response *)calloc(1, sizeof(*response));
    if (!response) {
        free(target);
        return NULL;
    }
    switch (method) {
        case TURBO_HTTP_GET: status = chttp_get(transport->client, &options, response, &error); break;
        case TURBO_HTTP_POST: status = chttp_post(transport->client, &options, response, &error); break;
        case TURBO_HTTP_PUT: status = chttp_put(transport->client, &options, response, &error); break;
        case TURBO_HTTP_DELETE: status = chttp_delete(transport->client, &options, response, &error); break;
        case TURBO_HTTP_HEAD: status = chttp_head(transport->client, &options, response, &error); break;
        default: status = SALTS_EINVAL; break;
    }
    if (status != SALTS_OK) {
        set_error(transport, &error, "CHTTP request failed");
        chttp_response_destroy(response);
        free(response);
        free(target);
        return NULL;
    }
    free(target);
    transport->error_msg[0] = '\0';
    return response;
}

chttp_response *turbo_transport_http_request(turbo_transport_t *transport,
                                              turbo_http_method_t method,
                                              const char *path, const uint8_t *body,
                                              size_t body_size, const char **headers,
                                              int header_count) {
    return execute_request(http_transport_impl(transport), method, path,
                           body, body_size, NULL, NULL, headers, header_count);
}

chttp_response *turbo_transport_http_upload_stream(turbo_transport_t *transport,
                                                    const char *path,
                                                    turbo_transport_read_cb callback,
                                                    size_t content_length,
                                                    void *user_data) {
    upload_source_t state = {callback, user_data};
    chttp_body_source source = {upload_read, &state, content_length, 1};
    return execute_request(http_transport_impl(transport), TURBO_HTTP_POST,
                           path, NULL, 0, &source, NULL, NULL, 0);
}

int turbo_transport_http_download_stream(turbo_transport_t *transport,
                                         const char *path,
                                         turbo_transport_data_cb callback,
                                         void *user_data) {
    download_sink_t state = {callback, user_data};
    chttp_body_sink sink = {download_write, &state};
    chttp_response *response = execute_request(http_transport_impl(transport),
        TURBO_HTTP_GET, path, NULL, 0, NULL, &sink, NULL, 0);
    int success;
    if (!response) return -1;
    success = response->status_code >= 200 && response->status_code < 300;
    chttp_response_destroy(response);
    free(response);
    return success ? 0 : -1;
}

turbo_transport_t *turbo_transport_create_http(const turbo_transport_config_t *config) {
    http_transport_impl_t *transport;
    chttp_client_config client_config;
    cnet_tls_client_config tls_config = {0};
    if (!config) return NULL;
    transport = (http_transport_impl_t *)calloc(1, sizeof(*transport));
    if (!transport) return NULL;
    transport->base.config = *config;
    transport->base.config.host = http_dup(config->host);
    transport->base.config.path = http_dup(config->path);
    transport->base.config.user_agent = http_dup(config->user_agent);
    transport->base.config.auth_token = http_dup(config->auth_token);
    if ((config->host && !transport->base.config.host) ||
        (config->path && !transport->base.config.path) ||
        (config->user_agent && !transport->base.config.user_agent) ||
        (config->auth_token && !transport->base.config.auth_token))
        goto fail;
    if (format_endpoint(transport, config) != SALTS_OK) goto fail;
    if (config->use_tls) {
        if (config->tls) {
            tls_config = *config->tls;
        } else {
            tls_config.size = sizeof(tls_config);
            tls_config.ca_file = config->ca_cert_path;
        }
        if (!tls_config.server_name || !tls_config.server_name[0]) {
            /* Bind certificate verification and SNI to the parsed URL host.
               Callers may still provide an explicit name when connecting by IP. */
            tls_config.server_name = transport->base.config.host;
        }
        if (tls_config.size != sizeof(tls_config) ||
            chttp_tls_profile_init(&transport->tls_profile, &tls_config) != SALTS_OK)
            goto fail;
        transport->tls_initialized = 1;
    } else if ((config->tls != NULL) ||
               (config->ca_cert_path && config->ca_cert_path[0])) {
        goto fail;
    }
    if (config->http_client) {
        transport->client = config->http_client;
    } else {
        client_config = make_client_config(config);
        if (chttp_client_init(&transport->owned_client, &client_config) != SALTS_OK) goto fail;
        transport->client = &transport->owned_client;
        transport->owns_client = 1;
    }
    return (turbo_transport_t *)transport;
fail:
    if (transport->tls_initialized)
        (void)chttp_tls_profile_destroy(&transport->tls_profile);
    free(transport->connection_uri);
    free(transport->authority);
    free((void *)transport->base.config.host);
    free((void *)transport->base.config.path);
    free((void *)transport->base.config.user_agent);
    http_free_secret(&transport->base.config.auth_token);
    free(transport);
    return NULL;
}

int turbo_transport_destroy_http(turbo_transport_t *transport) {
    http_transport_impl_t *impl = (http_transport_impl_t *)transport;
    if (!impl) return -1;
    if (impl->owns_client) {
        if (chttp_client_destroy(&impl->owned_client, HTTP_STOP_TIMEOUT_MS) !=
            SALTS_OK)
            return -1;
        memset(&impl->owned_client, 0, sizeof(impl->owned_client));
        impl->client = NULL;
        impl->owns_client = 0;
    }
    if (impl->tls_initialized) {
        if (chttp_tls_profile_destroy(&impl->tls_profile) != SALTS_OK)
            return -1;
        memset(&impl->tls_profile, 0, sizeof(impl->tls_profile));
        impl->tls_initialized = 0;
    }
    free(impl->connection_uri);
    free(impl->authority);
    free((void *)impl->base.config.host);
    free((void *)impl->base.config.path);
    free((void *)impl->base.config.user_agent);
    http_free_secret(&impl->base.config.auth_token);
    free(impl);
    return 0;
}

int turbo_transport_connect_http(turbo_transport_t *transport) {
    http_transport_impl_t *impl = (http_transport_impl_t *)transport;
    if (!impl || !impl->client) return -1;
    if (impl->connected) return 0;
    impl->connected = 1;
    if (impl->event_callback)
        impl->event_callback(transport, TURBO_TRANSPORT_EVENT_CONNECTED, NULL,
                             impl->event_user_data);
    return 0;
}

int turbo_transport_disconnect_http(turbo_transport_t *transport) {
    http_transport_impl_t *impl = (http_transport_impl_t *)transport;
    if (!impl) return -1;
    if (!impl->connected) return 0;
    impl->connected = 0;
    if (impl->event_callback)
        impl->event_callback(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED,
                             NULL, impl->event_user_data);
    return 0;
}

int turbo_transport_is_connected_http(turbo_transport_t *transport) {
    http_transport_impl_t *impl = (http_transport_impl_t *)transport;
    return impl ? impl->connected : 0;
}

void turbo_transport_set_event_callback_http(
    turbo_transport_t *transport, turbo_transport_event_cb callback,
    void *user_data) {
    http_transport_impl_t *impl = (http_transport_impl_t *)transport;
    if (!impl) return;
    impl->event_callback = callback;
    impl->event_user_data = user_data;
}

chttp_client *turbo_transport_get_http_client(turbo_transport_t *transport) {
    http_transport_impl_t *impl = http_transport_impl(transport);
    return impl ? impl->client : NULL;
}

const char *turbo_transport_get_error_http(turbo_transport_t *transport) {
    http_transport_impl_t *impl = (http_transport_impl_t *)transport;
    return impl && impl->error_msg[0] ? impl->error_msg : NULL;
}
