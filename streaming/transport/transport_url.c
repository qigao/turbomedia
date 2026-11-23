#include "turbo_transport.h"

#include <uri_parser.h>

#include <stdlib.h>
#include <string.h>

enum { TURBO_TRANSPORT_MAX_PORT = 65535 };

static int transport_default_port(turbo_transport_type_t type, int use_tls) {
    switch (type) {
        case TURBO_TRANSPORT_HTTP:
        case TURBO_TRANSPORT_WEBSOCKET:
            return use_tls ? 443 : 80;
        case TURBO_TRANSPORT_TLS:
            return 443;
        case TURBO_TRANSPORT_RTMP:
            return 1935;
        default:
            return 0;
    }
}

static char *transport_build_uri_path(const uri_t *uri) {
    const char *path = uri->path;
    const char *query = uri->query;
    size_t path_len = path && path[0] ? strlen(path) : 1;
    size_t query_len = query && query[0] ? strlen(query) : 0;
    char *out = (char *)malloc(path_len + (query_len ? query_len + 1 : 0) + 1);

    if (!out) return NULL;
    if (path && path[0]) {
        memcpy(out, path, path_len);
    } else {
        out[0] = '/';
    }
    if (query_len) {
        out[path_len] = '?';
        memcpy(out + path_len + 1, query, query_len);
        out[path_len + query_len + 1] = '\0';
    } else {
        out[path_len] = '\0';
    }

    return out;
}

int turbo_transport_parse_url(const char *url, turbo_transport_config_t *config) {
    uri_t uri;
    turbo_transport_config_t parsed_config = {0};
    const char *scheme;
    const char *host;
    int port;

    if (!url || !config) return -1;
    memset(config, 0, sizeof(*config));

    if (!uri_parse(url, &uri) || !uri.valid || uri.overflow_flags != 0) {
        return -1;
    }

    scheme = uri.scheme;
    host = uri.host;
    if (!scheme[0] || !host[0]) return -1;

    if (strcmp(scheme, "tcp") == 0) {
        parsed_config.type = TURBO_TRANSPORT_TCP;
    } else if (strcmp(scheme, "tls") == 0) {
        parsed_config.type = TURBO_TRANSPORT_TLS;
        parsed_config.use_tls = 1;
    } else if (strcmp(scheme, "ws") == 0) {
        parsed_config.type = TURBO_TRANSPORT_WEBSOCKET;
    } else if (strcmp(scheme, "wss") == 0) {
        parsed_config.type = TURBO_TRANSPORT_WEBSOCKET;
        parsed_config.use_tls = 1;
    } else if (strcmp(scheme, "http") == 0) {
        parsed_config.type = TURBO_TRANSPORT_HTTP;
    } else if (strcmp(scheme, "https") == 0) {
        parsed_config.type = TURBO_TRANSPORT_HTTP;
        parsed_config.use_tls = 1;
    } else if (strcmp(scheme, "rtmp") == 0) {
        parsed_config.type = TURBO_TRANSPORT_RTMP;
    } else {
        return -1;
    }

    port = uri.port;
    if ((uri.component_flags & URI_COMPONENT_PORT) != 0) {
        if (port <= 0 || port > TURBO_TRANSPORT_MAX_PORT) return -1;
        parsed_config.port = port;
    } else {
        parsed_config.port = transport_default_port(parsed_config.type,
                                                    parsed_config.use_tls);
    }
    if (parsed_config.port <= 0) return -1;

    parsed_config.host = strdup(host);
    if (!parsed_config.host) return -1;

    if (parsed_config.type == TURBO_TRANSPORT_HTTP ||
        parsed_config.type == TURBO_TRANSPORT_WEBSOCKET ||
        parsed_config.type == TURBO_TRANSPORT_RTMP) {
        parsed_config.path = transport_build_uri_path(&uri);
        if (!parsed_config.path) {
            free((void *)parsed_config.host);
            return -1;
        }
    }

    *config = parsed_config;
    return 0;
}
