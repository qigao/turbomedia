#include "chttp_upload.h"

#include <salts/error_codes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    STREAMER_HTTP_URL_CAPACITY = 2048,
    STREAMER_HTTP_DEFAULT_TIMEOUT_MS = 30000
};

int turbo_streamer_chttp_post_file(
    chttp_client *client, const chttp_tls_profile *tls_profile,
    const char *url, const char *path, uint32_t timeout_ms) {
    static const char http_scheme[] = "http://";
    static const char https_scheme[] = "https://";
    static const chttp_header headers[] = {
        {"Content-Type", "application/octet-stream"}
    };
    const char *authority_start;
    const char *target;
    const char *connection_scheme;
    size_t connection_scheme_size;
    size_t authority_size;
    char authority[STREAMER_HTTP_URL_CAPACITY];
    char connection_uri[STREAMER_HTTP_URL_CAPACITY];
    chttp_options options = {0};
    chttp_response response = {0};
    chttp_error error = {0};
    int use_tls;
    int status;
    int result = -1;

    if (!client || !url || !path || strchr(url, '\r') || strchr(url, '\n')) {
        return -1;
    }
    if (strncmp(url, https_scheme, sizeof(https_scheme) - 1u) == 0) {
        if (!tls_profile) {
            return -1;
        }
        authority_start = url + sizeof(https_scheme) - 1u;
        connection_scheme = "tls://";
        use_tls = 1;
    } else if (strncmp(url, http_scheme, sizeof(http_scheme) - 1u) == 0) {
        authority_start = url + sizeof(http_scheme) - 1u;
        connection_scheme = "tcp://";
        use_tls = 0;
    } else {
        return -1;
    }
    target = strchr(authority_start, '/');
    authority_size = target ? (size_t)(target - authority_start)
                            : strlen(authority_start);
    if (authority_size == 0u || authority_size >= sizeof(authority) ||
        memchr(authority_start, '@', authority_size)) {
        return -1;
    }
    memcpy(authority, authority_start, authority_size);
    authority[authority_size] = '\0';
    connection_scheme_size = strlen(connection_scheme);
    if (connection_scheme_size + authority_size >= sizeof(connection_uri)) {
        return -1;
    }
    memcpy(connection_uri, connection_scheme, connection_scheme_size);
    memcpy(connection_uri + connection_scheme_size, authority,
           authority_size + 1u);

    options.connection_uri = connection_uri;
    options.authority = authority;
    options.target = target ? target : "/";
    options.headers = headers;
    options.header_count = sizeof(headers) / sizeof(headers[0]);
    options.timeout_ms = timeout_ms ? timeout_ms
                                    : STREAMER_HTTP_DEFAULT_TIMEOUT_MS;
    options.tls = use_tls ? tls_profile : NULL;
    status = chttp_post_file(
        client, &options, path, NULL, NULL, &response, &error);
    if (status == SALTS_OK && response.status_code >= 200u &&
        response.status_code < 300u) {
        result = 0;
    }
    chttp_response_destroy(&response);
    return result;
}
