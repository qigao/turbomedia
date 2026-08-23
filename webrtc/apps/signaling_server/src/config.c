/**
 * @file config.c
 * @brief Signaling server configuration implementation
 */

#include "signaling_server/config.h"
#include "turbo_media_auth.h"
#include <turbo_fs.h>
#include <turbo_parser.h>
#include <tlog.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    SIGNALING_DEFAULT_JOIN_TIMEOUT_MS = 10000,
    SIGNALING_DEFAULT_MAX_MESSAGE_SIZE = 64 * 1024,
    SIGNALING_DEFAULT_MESSAGES_PER_SECOND = 100,
    SIGNALING_DEFAULT_MESSAGE_BURST = 200,
    SIGNALING_DEFAULT_MAX_OUTBOX_MESSAGES = 256,
    SIGNALING_DEFAULT_MAX_OUTBOX_BYTES = 1024 * 1024,
    SIGNALING_DEFAULT_MAX_CONNECTIONS_PER_SOURCE = 100,
    SIGNALING_DEFAULT_SOURCE_ADMISSIONS_PER_SECOND = 20,
    SIGNALING_DEFAULT_SOURCE_ADMISSION_BURST = 50,
    SIGNALING_DEFAULT_MAX_SOURCE_STATES = 4096,
    SIGNALING_DEFAULT_SOURCE_STATE_TTL_MS = 5 * 60 * 1000,
    SIGNALING_MIN_JOIN_TIMEOUT_MS = 1000,
    SIGNALING_MAX_JOIN_TIMEOUT_MS = 5 * 60 * 1000,
    SIGNALING_MIN_MESSAGE_SIZE = 8 * 1024,
    SIGNALING_MAX_MESSAGE_SIZE = 4 * 1024 * 1024,
    SIGNALING_MAX_MESSAGES_PER_SECOND = 10000,
    SIGNALING_MAX_MESSAGE_BURST = 20000,
    SIGNALING_MAX_OUTBOX_MESSAGES = 10000,
    SIGNALING_MAX_OUTBOX_BYTES = 64 * 1024 * 1024,
    SIGNALING_MAX_CONNECTIONS_PER_SOURCE = 10000,
    SIGNALING_MAX_SOURCE_ADMISSIONS_PER_SECOND = 10000,
    SIGNALING_MAX_SOURCE_ADMISSION_BURST = 20000,
    SIGNALING_MAX_SOURCE_STATES = 65536,
    SIGNALING_MIN_SOURCE_STATE_TTL_MS = 1000,
    SIGNALING_MAX_SOURCE_STATE_TTL_MS = 60 * 60 * 1000
};

typedef enum signaling_config_string_e {
    CONFIG_STRING_FILE = 0,
    CONFIG_STRING_NODE_ID,
    CONFIG_STRING_WS_HOST,
    CONFIG_STRING_WS_CERT_FILE,
    CONFIG_STRING_WS_KEY_FILE,
    CONFIG_STRING_HTTP_HOST,
    CONFIG_STRING_HTTP_CERT_FILE,
    CONFIG_STRING_HTTP_KEY_FILE,
    CONFIG_STRING_HTTP_ADMIN_TOKEN,
    CONFIG_STRING_HTTP_AUTH_ISSUER,
    CONFIG_STRING_HTTP_AUTH_ACTIVE_KEY_ID,
    CONFIG_STRING_HTTP_AUTH_ACTIVE_SECRET,
    CONFIG_STRING_HTTP_AUTH_PREVIOUS_KEY_ID,
    CONFIG_STRING_HTTP_AUTH_PREVIOUS_SECRET,
    CONFIG_STRING_HTTP_AUTH_REVOKED_TOKEN_SHA256,
    CONFIG_STRING_JWT_ISSUER,
    CONFIG_STRING_JWT_ACTIVE_KEY_ID,
    CONFIG_STRING_JWT_SECRET,
    CONFIG_STRING_JWT_PREVIOUS_KEY_ID,
    CONFIG_STRING_JWT_PREVIOUS_SECRET,
    CONFIG_STRING_JWT_REVOKED_TOKEN_SHA256,
    CONFIG_STRING_JWT_ALGORITHM,
    CONFIG_STRING_REDIS_HOST,
    CONFIG_STRING_REDIS_PASSWORD,
    CONFIG_STRING_LOG_LEVEL,
    CONFIG_STRING_LOG_FORMAT,
    CONFIG_STRING_LOG_OUTPUT,
    CONFIG_STRING_COUNT
} signaling_config_string_t;

typedef struct signaling_config_storage_s {
    char *values[CONFIG_STRING_COUNT];
} signaling_config_storage_t;

static char *config_string_duplicate(const char *value) {
    size_t size;
    char *copy;

    if (!value) {
        return NULL;
    }
    size = strlen(value) + 1;
    copy = (char *)malloc(size);
    if (copy) {
        memcpy(copy, value, size);
    }
    return copy;
}

static void config_storage_destroy(signaling_config_storage_t *storage) {
    int index;

    if (!storage) {
        return;
    }
    for (index = 0; index < CONFIG_STRING_COUNT; ++index) {
        free(storage->values[index]);
    }
    free(storage);
}

static int config_storage_replace(
    signaling_config_storage_t *storage,
    signaling_config_string_t index,
    char *value,
    const char **target) {
    if (!storage || index < 0 || index >= CONFIG_STRING_COUNT || !target) {
        free(value);
        return -1;
    }
    free(storage->values[index]);
    storage->values[index] = value;
    *target = value;
    return 0;
}

static int config_storage_copy(
    signaling_config_storage_t *storage,
    signaling_config_string_t index,
    const char *value,
    const char **target) {
    char *copy = config_string_duplicate(value);

    if (value && !copy) {
        return -1;
    }
    return config_storage_replace(storage, index, copy, target);
}

static int config_clone_strings(
    const signaling_server_config_t *source,
    signaling_server_config_t *candidate,
    signaling_config_storage_t *storage) {
#define CONFIG_CLONE(index, field)                                                   \
    do {                                                                             \
        if (config_storage_copy(storage, index, source->field, &candidate->field) != 0) { \
            return -1;                                                               \
        }                                                                            \
    } while (0)

    CONFIG_CLONE(CONFIG_STRING_FILE, config_file);
    CONFIG_CLONE(CONFIG_STRING_NODE_ID, node_id);
    CONFIG_CLONE(CONFIG_STRING_WS_HOST, ws_host);
    CONFIG_CLONE(CONFIG_STRING_WS_CERT_FILE, ws_cert_file);
    CONFIG_CLONE(CONFIG_STRING_WS_KEY_FILE, ws_key_file);
    CONFIG_CLONE(CONFIG_STRING_HTTP_HOST, http_host);
    CONFIG_CLONE(CONFIG_STRING_HTTP_CERT_FILE, http_cert_file);
    CONFIG_CLONE(CONFIG_STRING_HTTP_KEY_FILE, http_key_file);
    CONFIG_CLONE(CONFIG_STRING_HTTP_ADMIN_TOKEN, http_admin_token);
    CONFIG_CLONE(CONFIG_STRING_HTTP_AUTH_ISSUER, http_auth_issuer);
    CONFIG_CLONE(CONFIG_STRING_HTTP_AUTH_ACTIVE_KEY_ID,
                 http_auth_active_key_id);
    CONFIG_CLONE(CONFIG_STRING_HTTP_AUTH_ACTIVE_SECRET,
                 http_auth_active_secret);
    CONFIG_CLONE(CONFIG_STRING_HTTP_AUTH_PREVIOUS_KEY_ID,
                 http_auth_previous_key_id);
    CONFIG_CLONE(CONFIG_STRING_HTTP_AUTH_PREVIOUS_SECRET,
                 http_auth_previous_secret);
    CONFIG_CLONE(CONFIG_STRING_HTTP_AUTH_REVOKED_TOKEN_SHA256,
                 http_auth_revoked_token_sha256);
    CONFIG_CLONE(CONFIG_STRING_JWT_ISSUER, jwt_issuer);
    CONFIG_CLONE(CONFIG_STRING_JWT_ACTIVE_KEY_ID, jwt_active_key_id);
    CONFIG_CLONE(CONFIG_STRING_JWT_SECRET, jwt_secret);
    CONFIG_CLONE(CONFIG_STRING_JWT_PREVIOUS_KEY_ID, jwt_previous_key_id);
    CONFIG_CLONE(CONFIG_STRING_JWT_PREVIOUS_SECRET, jwt_previous_secret);
    CONFIG_CLONE(CONFIG_STRING_JWT_REVOKED_TOKEN_SHA256,
                 jwt_revoked_token_sha256);
    CONFIG_CLONE(CONFIG_STRING_JWT_ALGORITHM, jwt_algorithm);
    CONFIG_CLONE(CONFIG_STRING_REDIS_HOST, redis_host);
    CONFIG_CLONE(CONFIG_STRING_REDIS_PASSWORD, redis_password);
    CONFIG_CLONE(CONFIG_STRING_LOG_LEVEL, log_level);
    CONFIG_CLONE(CONFIG_STRING_LOG_FORMAT, log_format);
    CONFIG_CLONE(CONFIG_STRING_LOG_OUTPUT, log_output);

#undef CONFIG_CLONE
    return 0;
}

static int config_key_equals(const char *key, int key_length, const char *expected) {
    size_t expected_length;

    if (!key || key_length < 0 || !expected) {
        return 0;
    }
    expected_length = strlen(expected);
    return expected_length == (size_t)key_length &&
           memcmp(key, expected, expected_length) == 0;
}

static int config_table_has_key(const turbo_toml_t *table, const char *expected) {
    int index;
    int count = turbo_toml_len(table);

    for (index = 0; index < count; ++index) {
        int key_length = 0;
        const char *key = turbo_toml_key(table, index, &key_length);
        if (config_key_equals(key, key_length, expected)) {
            return 1;
        }
    }
    return 0;
}

static int config_table_keys_valid(
    const turbo_toml_t *table,
    const char *section,
    const char *const *allowed,
    size_t allowed_count) {
    int index;
    int count;

    if (!table) {
        return -1;
    }
    count = turbo_toml_len(table);
    for (index = 0; index < count; ++index) {
        int key_length = 0;
        const char *key = turbo_toml_key(table, index, &key_length);
        size_t allowed_index;
        int found = 0;

        for (allowed_index = 0; allowed_index < allowed_count; ++allowed_index) {
            if (config_key_equals(key, key_length, allowed[allowed_index])) {
                found = 1;
                break;
            }
        }
        if (!found) {
            TLOG_ERRORF("Unknown TOML key in [{}]: {}", section, key ? key : "(null)");
            return -1;
        }
    }
    return 0;
}

static int config_apply_string(
    const turbo_toml_t *table,
    const char *section,
    const char *key,
    signaling_config_storage_t *storage,
    signaling_config_string_t index,
    const char **target) {
    turbo_toml_value_t value;

    if (!config_table_has_key(table, key)) {
        return 0;
    }
    value = turbo_toml_string(table, key);
    if (!value.ok || !value.u.s || value.u.sl < 0 ||
        strlen(value.u.s) != (size_t)value.u.sl) {
        free(value.ok ? value.u.s : NULL);
        TLOG_ERRORF("TOML key [{}].{} must be a string without embedded NUL", section, key);
        return -1;
    }
    return config_storage_replace(storage, index, value.u.s, target);
}

static int config_apply_bool(
    const turbo_toml_t *table,
    const char *section,
    const char *key,
    int *target) {
    turbo_toml_value_t value;

    if (!config_table_has_key(table, key)) {
        return 0;
    }
    value = turbo_toml_bool(table, key);
    if (!value.ok) {
        TLOG_ERRORF("TOML key [{}].{} must be a boolean", section, key);
        return -1;
    }
    *target = value.u.b ? 1 : 0;
    return 0;
}

static int config_apply_int(
    const turbo_toml_t *table,
    const char *section,
    const char *key,
    int *target) {
    turbo_toml_value_t value;

    if (!config_table_has_key(table, key)) {
        return 0;
    }
    value = turbo_toml_int(table, key);
    if (!value.ok || value.u.i < INT_MIN || value.u.i > INT_MAX) {
        TLOG_ERRORF("TOML key [{}].{} must be a 32-bit integer", section, key);
        return -1;
    }
    *target = (int)value.u.i;
    return 0;
}

static int config_get_optional_table(
    const turbo_toml_t *root,
    const char *name,
    turbo_toml_t **table) {
    *table = NULL;
    if (!config_table_has_key(root, name)) {
        return 0;
    }
    *table = turbo_toml_table(root, name);
    if (!*table) {
        TLOG_ERRORF("TOML root key '{}' must be a table", name);
        return -1;
    }
    return 0;
}

static int config_apply_server(
    const turbo_toml_t *table,
    signaling_server_config_t *config,
    signaling_config_storage_t *storage) {
    static const char *const allowed[] = {
        "node_id", "host", "port", "use_tls", "cert_file", "key_file"
    };

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(table, "server", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_string(table, "server", "node_id", storage, CONFIG_STRING_NODE_ID,
                            &config->node_id) != 0 ||
        config_apply_string(table, "server", "host", storage, CONFIG_STRING_WS_HOST,
                            &config->ws_host) != 0 ||
        config_apply_int(table, "server", "port", &config->ws_port) != 0 ||
        config_apply_bool(table, "server", "use_tls", &config->ws_use_tls) != 0 ||
        config_apply_string(table, "server", "cert_file", storage,
                            CONFIG_STRING_WS_CERT_FILE, &config->ws_cert_file) != 0 ||
        config_apply_string(table, "server", "key_file", storage,
                            CONFIG_STRING_WS_KEY_FILE, &config->ws_key_file) != 0) {
        return -1;
    }
    return 0;
}

static int config_apply_http(
    const turbo_toml_t *table,
    signaling_server_config_t *config,
    signaling_config_storage_t *storage) {
    static const char *const allowed[] = {
        "enabled", "host", "port", "use_tls", "cert_file", "key_file",
        "auth_enabled"
    };

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(table, "http_api", allowed,
                                sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_bool(table, "http_api", "enabled", &config->http_enabled) != 0 ||
        config_apply_string(table, "http_api", "host", storage, CONFIG_STRING_HTTP_HOST,
                            &config->http_host) != 0 ||
        config_apply_int(table, "http_api", "port", &config->http_port) != 0 ||
        config_apply_bool(table, "http_api", "use_tls",
                          &config->http_use_tls) != 0 ||
        config_apply_string(table, "http_api", "cert_file", storage,
                            CONFIG_STRING_HTTP_CERT_FILE,
                            &config->http_cert_file) != 0 ||
        config_apply_string(table, "http_api", "key_file", storage,
                            CONFIG_STRING_HTTP_KEY_FILE,
                            &config->http_key_file) != 0 ||
        config_apply_bool(table, "http_api", "auth_enabled",
                          &config->http_auth_enabled) != 0) {
        return -1;
    }
    return 0;
}

static int config_apply_limits(
    const turbo_toml_t *table,
    signaling_server_config_t *config) {
    static const char *const allowed[] = {
        "max_peers", "max_rooms", "peer_timeout_ms", "join_timeout_ms",
        "max_message_size", "messages_per_second", "message_burst",
        "max_outbox_messages", "max_outbox_bytes",
        "max_connections_per_source", "source_admissions_per_second",
        "source_admission_burst", "max_source_states", "source_state_ttl_ms"
    };

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(table, "limits", allowed,
                                sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_int(table, "limits", "max_peers", &config->max_peers) != 0 ||
        config_apply_int(table, "limits", "max_rooms", &config->max_rooms) != 0 ||
        config_apply_int(table, "limits", "peer_timeout_ms",
                         &config->peer_timeout_ms) != 0 ||
        config_apply_int(table, "limits", "join_timeout_ms",
                         &config->join_timeout_ms) != 0 ||
        config_apply_int(table, "limits", "max_message_size",
                         &config->max_message_size) != 0 ||
        config_apply_int(table, "limits", "messages_per_second",
                         &config->messages_per_second) != 0 ||
        config_apply_int(table, "limits", "message_burst",
                         &config->message_burst) != 0 ||
        config_apply_int(table, "limits", "max_outbox_messages",
                         &config->max_outbox_messages) != 0 ||
        config_apply_int(table, "limits", "max_outbox_bytes",
                         &config->max_outbox_bytes) != 0 ||
        config_apply_int(table, "limits", "max_connections_per_source",
                         &config->max_connections_per_source) != 0 ||
        config_apply_int(table, "limits", "source_admissions_per_second",
                         &config->source_admissions_per_second) != 0 ||
        config_apply_int(table, "limits", "source_admission_burst",
                         &config->source_admission_burst) != 0 ||
        config_apply_int(table, "limits", "max_source_states",
                         &config->max_source_states) != 0 ||
        config_apply_int(table, "limits", "source_state_ttl_ms",
                         &config->source_state_ttl_ms) != 0) {
        return -1;
    }
    return 0;
}

static int config_apply_http_auth(
    const turbo_toml_t *table,
    signaling_server_config_t *config,
    signaling_config_storage_t *storage) {
    static const char *const allowed[] = {
        "issuer", "active_key_id", "active_secret", "previous_key_id",
        "previous_secret", "revoked_token_sha256", "clock_skew_seconds",
        "max_ttl_seconds"
    };

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(
            table, "http_auth", allowed,
            sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_string(table, "http_auth", "issuer", storage,
                            CONFIG_STRING_HTTP_AUTH_ISSUER,
                            &config->http_auth_issuer) != 0 ||
        config_apply_string(table, "http_auth", "active_key_id", storage,
                            CONFIG_STRING_HTTP_AUTH_ACTIVE_KEY_ID,
                            &config->http_auth_active_key_id) != 0 ||
        config_apply_string(table, "http_auth", "active_secret", storage,
                            CONFIG_STRING_HTTP_AUTH_ACTIVE_SECRET,
                            &config->http_auth_active_secret) != 0 ||
        config_apply_string(table, "http_auth", "previous_key_id", storage,
                            CONFIG_STRING_HTTP_AUTH_PREVIOUS_KEY_ID,
                            &config->http_auth_previous_key_id) != 0 ||
        config_apply_string(table, "http_auth", "previous_secret", storage,
                            CONFIG_STRING_HTTP_AUTH_PREVIOUS_SECRET,
                            &config->http_auth_previous_secret) != 0 ||
        config_apply_string(
            table, "http_auth", "revoked_token_sha256", storage,
            CONFIG_STRING_HTTP_AUTH_REVOKED_TOKEN_SHA256,
            &config->http_auth_revoked_token_sha256) != 0 ||
        config_apply_int(table, "http_auth", "clock_skew_seconds",
                         &config->http_auth_clock_skew_seconds) != 0 ||
        config_apply_int(table, "http_auth", "max_ttl_seconds",
                         &config->http_auth_max_ttl_seconds) != 0) {
        return -1;
    }
    return 0;
}

static int config_apply_auth(
    const turbo_toml_t *table,
    signaling_server_config_t *config,
    signaling_config_storage_t *storage) {
    static const char *const allowed[] = {
        "enabled", "issuer", "active_key_id", "secret",
        "previous_key_id", "previous_secret", "revoked_token_sha256",
        "clock_skew_seconds", "ttl_seconds", "algorithm"
    };

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(table, "auth", allowed,
                                sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_bool(table, "auth", "enabled", &config->jwt_enabled) != 0 ||
        config_apply_string(table, "auth", "issuer", storage,
                            CONFIG_STRING_JWT_ISSUER,
                            &config->jwt_issuer) != 0 ||
        config_apply_string(table, "auth", "active_key_id", storage,
                            CONFIG_STRING_JWT_ACTIVE_KEY_ID,
                            &config->jwt_active_key_id) != 0 ||
        config_apply_string(table, "auth", "secret", storage, CONFIG_STRING_JWT_SECRET,
                            &config->jwt_secret) != 0 ||
        config_apply_string(table, "auth", "previous_key_id", storage,
                            CONFIG_STRING_JWT_PREVIOUS_KEY_ID,
                            &config->jwt_previous_key_id) != 0 ||
        config_apply_string(table, "auth", "previous_secret", storage,
                            CONFIG_STRING_JWT_PREVIOUS_SECRET,
                            &config->jwt_previous_secret) != 0 ||
        config_apply_string(table, "auth", "revoked_token_sha256", storage,
                            CONFIG_STRING_JWT_REVOKED_TOKEN_SHA256,
                            &config->jwt_revoked_token_sha256) != 0 ||
        config_apply_int(table, "auth", "clock_skew_seconds",
                         &config->jwt_clock_skew_seconds) != 0 ||
        config_apply_int(table, "auth", "ttl_seconds", &config->jwt_ttl_seconds) != 0 ||
        config_apply_string(table, "auth", "algorithm", storage,
                            CONFIG_STRING_JWT_ALGORITHM, &config->jwt_algorithm) != 0) {
        return -1;
    }
    return 0;
}

static int config_apply_redis(
    const turbo_toml_t *table,
    signaling_server_config_t *config,
    signaling_config_storage_t *storage) {
    static const char *const allowed[] = {
        "enabled", "host", "port", "password", "db", "use_streams",
        "stream_read_interval_ms", "stream_max_len"
    };

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(table, "redis", allowed,
                                sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_bool(table, "redis", "enabled", &config->redis_enabled) != 0 ||
        config_apply_string(table, "redis", "host", storage, CONFIG_STRING_REDIS_HOST,
                            &config->redis_host) != 0 ||
        config_apply_int(table, "redis", "port", &config->redis_port) != 0 ||
        config_apply_string(table, "redis", "password", storage,
                            CONFIG_STRING_REDIS_PASSWORD, &config->redis_password) != 0 ||
        config_apply_int(table, "redis", "db", &config->redis_db) != 0 ||
        config_apply_bool(table, "redis", "use_streams",
                          &config->redis_use_streams) != 0 ||
        config_apply_int(table, "redis", "stream_read_interval_ms",
                         &config->redis_stream_read_interval_ms) != 0 ||
        config_apply_int(table, "redis", "stream_max_len",
                         &config->redis_stream_max_len) != 0) {
        return -1;
    }
    return 0;
}

static int config_apply_logging(
    const turbo_toml_t *table,
    signaling_server_config_t *config,
    signaling_config_storage_t *storage) {
    static const char *const allowed[] = {"level", "format", "output"};

    if (!table) {
        return 0;
    }
    if (config_table_keys_valid(table, "logging", allowed,
                                sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        config_apply_string(table, "logging", "level", storage, CONFIG_STRING_LOG_LEVEL,
                            &config->log_level) != 0 ||
        config_apply_string(table, "logging", "format", storage, CONFIG_STRING_LOG_FORMAT,
                            &config->log_format) != 0 ||
        config_apply_string(table, "logging", "output", storage, CONFIG_STRING_LOG_OUTPUT,
                            &config->log_output) != 0) {
        return -1;
    }
    return 0;
}

static int config_string_in_set(
    const char *value,
    const char *const *allowed,
    size_t allowed_count) {
    size_t index;

    if (!value) {
        return 0;
    }
    for (index = 0; index < allowed_count; ++index) {
        if (strcmp(value, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Initialize configuration with defaults
 */
void signaling_server_config_init(signaling_server_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    
    /* Defaults */
    config->config_file = NULL;
    config->node_id = NULL; /* Auto-generated */
    
    /* WebSocket */
    config->ws_host = "0.0.0.0";
    config->ws_port = 8080;
    config->ws_use_tls = 0;
    config->ws_cert_file = NULL;
    config->ws_key_file = NULL;
    
    /* HTTP API */
    config->http_enabled = 0;
    config->http_host = "0.0.0.0";
    config->http_port = 8081;
    config->http_use_tls = 0;
    config->http_cert_file = NULL;
    config->http_key_file = NULL;
    config->http_auth_enabled = 1;
    config->http_admin_token = getenv("TURBO_SIGNALING_ADMIN_TOKEN");
    config->http_auth_issuer = "turbomedia";
    config->http_auth_active_key_id =
        getenv("TURBO_SIGNALING_HTTP_AUTH_ACTIVE_KEY_ID");
    config->http_auth_active_secret =
        getenv("TURBO_SIGNALING_HTTP_AUTH_ACTIVE_SECRET");
    config->http_auth_previous_key_id =
        getenv("TURBO_SIGNALING_HTTP_AUTH_PREVIOUS_KEY_ID");
    config->http_auth_previous_secret =
        getenv("TURBO_SIGNALING_HTTP_AUTH_PREVIOUS_SECRET");
    config->http_auth_revoked_token_sha256 =
        getenv("TURBO_SIGNALING_HTTP_AUTH_REVOKED_TOKEN_SHA256");
    config->http_auth_clock_skew_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_CLOCK_SKEW_SECONDS;
    config->http_auth_max_ttl_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS;
    
    /* Limits */
    config->max_peers = 1000;
    config->max_rooms = 100;
    config->peer_timeout_ms = 60000; /* 60 seconds */
    config->join_timeout_ms = SIGNALING_DEFAULT_JOIN_TIMEOUT_MS;
    config->max_message_size = SIGNALING_DEFAULT_MAX_MESSAGE_SIZE;
    config->messages_per_second = SIGNALING_DEFAULT_MESSAGES_PER_SECOND;
    config->message_burst = SIGNALING_DEFAULT_MESSAGE_BURST;
    config->max_outbox_messages = SIGNALING_DEFAULT_MAX_OUTBOX_MESSAGES;
    config->max_outbox_bytes = SIGNALING_DEFAULT_MAX_OUTBOX_BYTES;
    config->max_connections_per_source =
        SIGNALING_DEFAULT_MAX_CONNECTIONS_PER_SOURCE;
    config->source_admissions_per_second =
        SIGNALING_DEFAULT_SOURCE_ADMISSIONS_PER_SECOND;
    config->source_admission_burst =
        SIGNALING_DEFAULT_SOURCE_ADMISSION_BURST;
    config->max_source_states = SIGNALING_DEFAULT_MAX_SOURCE_STATES;
    config->source_state_ttl_ms = SIGNALING_DEFAULT_SOURCE_STATE_TTL_MS;
    
    /* JWT */
    config->jwt_enabled = 0;
    config->jwt_issuer = "turbomedia";
    config->jwt_active_key_id =
        getenv("TURBO_SIGNALING_AUTH_ACTIVE_KEY_ID");
    config->jwt_secret = getenv("TURBO_SIGNALING_AUTH_ACTIVE_SECRET");
    config->jwt_previous_key_id =
        getenv("TURBO_SIGNALING_AUTH_PREVIOUS_KEY_ID");
    config->jwt_previous_secret =
        getenv("TURBO_SIGNALING_AUTH_PREVIOUS_SECRET");
    config->jwt_revoked_token_sha256 =
        getenv("TURBO_SIGNALING_AUTH_REVOKED_TOKEN_SHA256");
    config->jwt_clock_skew_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_CLOCK_SKEW_SECONDS;
    config->jwt_ttl_seconds = TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS;
    config->jwt_algorithm = "HS256";
    
    /* Redis */
    config->redis_enabled = 0; /* Disabled by default */
    config->redis_host = "localhost";
    config->redis_port = 6379;
    config->redis_password = NULL;
    config->redis_db = 0;
    config->redis_use_streams = 1;
    config->redis_stream_read_interval_ms = 100;
    config->redis_stream_max_len = 1000;
    
    /* Logging */
    config->log_level = "info";
    config->log_format = "text";
    config->log_output = "stdout";
}

int signaling_server_config_load(signaling_server_config_t *config, const char *filename) {
    static const char *const root_keys[] = {
        "server", "http_api", "http_auth", "limits", "auth", "redis",
        "logging"
    };
    turbo_fs_buf_t file = {0};
    turbo_toml_t *root = NULL;
    turbo_toml_t *server = NULL;
    turbo_toml_t *http = NULL;
    turbo_toml_t *http_auth = NULL;
    turbo_toml_t *limits = NULL;
    turbo_toml_t *auth = NULL;
    turbo_toml_t *redis = NULL;
    turbo_toml_t *logging = NULL;
    signaling_config_storage_t *storage = NULL;
    signaling_config_storage_t *old_storage;
    signaling_server_config_t candidate;
    int result = -1;

    if (!config || !filename || filename[0] == '\0') {
        TLOG_ERROR("Configuration load requires an initialized config and non-empty filename");
        return -1;
    }
    storage = (signaling_config_storage_t *)calloc(1, sizeof(*storage));
    if (!storage) {
        TLOG_ERROR("Failed to allocate configuration storage");
        return -1;
    }
    candidate = *config;
    candidate.private_data = storage;
    if (config_clone_strings(config, &candidate, storage) != 0 ||
        config_storage_copy(storage, CONFIG_STRING_FILE, filename,
                            &candidate.config_file) != 0) {
        TLOG_ERROR("Failed to copy configuration strings");
        goto cleanup;
    }

    if (turbo_fs_read_file(filename, &file) != 0) {
        TLOG_ERRORF("Failed to open configuration file: {}", filename);
        goto cleanup;
    }
    if (turbo_parse_toml((const uint8_t *)file.base, file.len, &root) != 0) {
        TLOG_ERRORF("Failed to parse TOML configuration file: {}", filename);
        goto cleanup;
    }
    if (config_table_keys_valid(root, "root", root_keys,
                                sizeof(root_keys) / sizeof(root_keys[0])) != 0 ||
        config_get_optional_table(root, "server", &server) != 0 ||
        config_get_optional_table(root, "http_api", &http) != 0 ||
        config_get_optional_table(root, "http_auth", &http_auth) != 0 ||
        config_get_optional_table(root, "limits", &limits) != 0 ||
        config_get_optional_table(root, "auth", &auth) != 0 ||
        config_get_optional_table(root, "redis", &redis) != 0 ||
        config_get_optional_table(root, "logging", &logging) != 0 ||
        config_apply_server(server, &candidate, storage) != 0 ||
        config_apply_http(http, &candidate, storage) != 0 ||
        config_apply_http_auth(http_auth, &candidate, storage) != 0 ||
        config_apply_limits(limits, &candidate) != 0 ||
        config_apply_auth(auth, &candidate, storage) != 0 ||
        config_apply_redis(redis, &candidate, storage) != 0 ||
        config_apply_logging(logging, &candidate, storage) != 0 ||
        signaling_server_config_validate(&candidate) != 0) {
        goto cleanup;
    }

    old_storage = (signaling_config_storage_t *)config->private_data;
    *config = candidate;
    storage = NULL;
    config_storage_destroy(old_storage);
    result = 0;

cleanup:
    turbo_free_toml(&root);
    turbo_fs_buf_free(&file);
    config_storage_destroy(storage);
    return result;
}

static int signaling_config_env_int(const char *name, int current) {
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0') {
        return current;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return INT_MIN;
    }
    return (int)parsed;
}

void signaling_server_config_apply_environment(signaling_server_config_t *config) {
    const char *admin_token;
    const char *value;

    if (!config) {
        return;
    }
    admin_token = getenv("TURBO_SIGNALING_ADMIN_TOKEN");
    if (admin_token && admin_token[0] != '\0') {
        config->http_admin_token = admin_token;
    }
    value = getenv("TURBO_SIGNALING_HTTP_AUTH_ISSUER");
    if (value && value[0] != '\0') {
        config->http_auth_issuer = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_AUTH_ACTIVE_KEY_ID");
    if (value && value[0] != '\0') {
        config->http_auth_active_key_id = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_AUTH_ACTIVE_SECRET");
    if (value && value[0] != '\0') {
        config->http_auth_active_secret = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_AUTH_PREVIOUS_KEY_ID");
    if (value && value[0] != '\0') {
        config->http_auth_previous_key_id = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_AUTH_PREVIOUS_SECRET");
    if (value && value[0] != '\0') {
        config->http_auth_previous_secret = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_AUTH_REVOKED_TOKEN_SHA256");
    if (value && value[0] != '\0') {
        config->http_auth_revoked_token_sha256 = value;
    }
    config->http_auth_clock_skew_seconds = signaling_config_env_int(
        "TURBO_SIGNALING_HTTP_AUTH_CLOCK_SKEW_SECONDS",
        config->http_auth_clock_skew_seconds);
    config->http_auth_max_ttl_seconds = signaling_config_env_int(
        "TURBO_SIGNALING_HTTP_AUTH_MAX_TTL_SECONDS",
        config->http_auth_max_ttl_seconds);
    value = getenv("TURBO_SIGNALING_AUTH_ISSUER");
    if (value && value[0] != '\0') {
        config->jwt_issuer = value;
    }
    value = getenv("TURBO_SIGNALING_AUTH_ACTIVE_KEY_ID");
    if (value && value[0] != '\0') {
        config->jwt_active_key_id = value;
    }
    value = getenv("TURBO_SIGNALING_AUTH_ACTIVE_SECRET");
    if (value && value[0] != '\0') {
        config->jwt_secret = value;
    }
    value = getenv("TURBO_SIGNALING_AUTH_PREVIOUS_KEY_ID");
    if (value && value[0] != '\0') {
        config->jwt_previous_key_id = value;
    }
    value = getenv("TURBO_SIGNALING_AUTH_PREVIOUS_SECRET");
    if (value && value[0] != '\0') {
        config->jwt_previous_secret = value;
    }
    value = getenv("TURBO_SIGNALING_AUTH_REVOKED_TOKEN_SHA256");
    if (value && value[0] != '\0') {
        config->jwt_revoked_token_sha256 = value;
    }
    config->jwt_clock_skew_seconds = signaling_config_env_int(
        "TURBO_SIGNALING_AUTH_CLOCK_SKEW_SECONDS",
        config->jwt_clock_skew_seconds);
    config->jwt_ttl_seconds = signaling_config_env_int(
        "TURBO_SIGNALING_AUTH_MAX_TTL_SECONDS",
        config->jwt_ttl_seconds);
    value = getenv("TURBO_SIGNALING_USE_TLS");
    if (value && value[0] != '\0') {
        config->ws_use_tls =
            (strcmp(value, "1") == 0 || strcmp(value, "true") == 0)
                ? 1
                : ((strcmp(value, "0") == 0 ||
                    strcmp(value, "false") == 0) ? 0 : 2);
    }
    value = getenv("TURBO_SIGNALING_TLS_CERT_FILE");
    if (value && value[0] != '\0') {
        config->ws_cert_file = value;
    }
    value = getenv("TURBO_SIGNALING_TLS_KEY_FILE");
    if (value && value[0] != '\0') {
        config->ws_key_file = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_USE_TLS");
    if (value && value[0] != '\0') {
        config->http_use_tls =
            (strcmp(value, "1") == 0 || strcmp(value, "true") == 0)
                ? 1
                : ((strcmp(value, "0") == 0 ||
                    strcmp(value, "false") == 0) ? 0 : 2);
    }
    value = getenv("TURBO_SIGNALING_HTTP_TLS_CERT_FILE");
    if (value && value[0] != '\0') {
        config->http_cert_file = value;
    }
    value = getenv("TURBO_SIGNALING_HTTP_TLS_KEY_FILE");
    if (value && value[0] != '\0') {
        config->http_key_file = value;
    }
}

/**
 * Cleanup configuration resources
 */
void signaling_server_config_cleanup(signaling_server_config_t *config) {
    if (!config) {
        return;
    }
    config_storage_destroy((signaling_config_storage_t *)config->private_data);
    config->private_data = NULL;
}

/**
 * Validate configuration
 */
int signaling_server_config_validate(const signaling_server_config_t *config) {
    static const char *const log_levels[] = {"trace", "debug", "info", "warn", "error"};
    static const char *const log_formats[] = {"text"};
    static const char *const log_outputs[] = {"stdout"};

    if (!config || !config->ws_host || config->ws_host[0] == '\0' ||
        !config->log_level || !config->log_format || !config->log_output) {
        TLOG_ERROR("Configuration contains a missing required value");
        return -1;
    }

    /* Validate ports */
    if (config->ws_port < 1 || config->ws_port > 65535) {
        TLOG_ERRORF("Invalid WebSocket port: {}", config->ws_port);
        return -1;
    }
    if ((config->ws_use_tls != 0 && config->ws_use_tls != 1) ||
        (config->http_enabled != 0 && config->http_enabled != 1) ||
        (config->http_use_tls != 0 && config->http_use_tls != 1) ||
        (config->http_auth_enabled != 0 &&
         config->http_auth_enabled != 1)) {
        TLOG_ERROR("Invalid signaling boolean configuration");
        return -1;
    }
    
    if (config->http_enabled && (config->http_port < 1 || config->http_port > 65535)) {
        TLOG_ERRORF("Invalid HTTP port: {}", config->http_port);
        return -1;
    }
    if (config->http_enabled &&
        (!config->http_host || config->http_host[0] == '\0')) {
        TLOG_ERROR("HTTP host must be non-empty");
        return -1;
    }
    {
        turbo_media_auth_config_t auth_config = {
            .issuer = config->http_auth_issuer,
            .active_key_id = config->http_auth_active_key_id,
            .active_secret = config->http_auth_active_secret,
            .previous_key_id = config->http_auth_previous_key_id,
            .previous_secret = config->http_auth_previous_secret,
            .revoked_token_sha256 =
                config->http_auth_revoked_token_sha256,
            .clock_skew_seconds = config->http_auth_clock_skew_seconds,
            .max_ttl_seconds = config->http_auth_max_ttl_seconds
        };
        int has_static =
            config->http_admin_token &&
            config->http_admin_token[0] != '\0';
        int has_signed =
            config->http_auth_active_secret &&
            config->http_auth_active_secret[0] != '\0';
        int any_signed =
            has_signed ||
            (config->http_auth_active_key_id &&
             config->http_auth_active_key_id[0] != '\0') ||
            (config->http_auth_previous_key_id &&
             config->http_auth_previous_key_id[0] != '\0') ||
            (config->http_auth_previous_secret &&
             config->http_auth_previous_secret[0] != '\0') ||
            (config->http_auth_revoked_token_sha256 &&
             config->http_auth_revoked_token_sha256[0] != '\0');

        if (config->http_enabled &&
            (!config->http_auth_enabled ||
             (!has_static && !has_signed))) {
            TLOG_ERROR(
                "Enabled HTTP management API requires static or scoped "
                "bearer authentication");
            return -1;
        }
        if ((any_signed &&
             turbo_media_auth_config_validate(&auth_config) != 0) ||
            (!any_signed &&
             (!config->http_auth_issuer ||
              config->http_auth_issuer[0] == '\0' ||
              config->http_auth_clock_skew_seconds < 0 ||
              config->http_auth_max_ttl_seconds < 1))) {
            TLOG_ERROR("Invalid signaling management token configuration");
            return -1;
        }
    }
    
    /* Validate limits */
    if (config->max_peers < 1) {
        TLOG_ERRORF("Invalid max_peers: {}", config->max_peers);
        return -1;
    }
    
    if (config->max_rooms < 1) {
        TLOG_ERRORF("Invalid max_rooms: {}", config->max_rooms);
        return -1;
    }
    if (config->peer_timeout_ms < 1) {
        TLOG_ERRORF("Invalid peer_timeout_ms: {}", config->peer_timeout_ms);
        return -1;
    }
    if (config->join_timeout_ms < SIGNALING_MIN_JOIN_TIMEOUT_MS ||
        config->join_timeout_ms > SIGNALING_MAX_JOIN_TIMEOUT_MS) {
        TLOG_ERRORF("Invalid join_timeout_ms: {}", config->join_timeout_ms);
        return -1;
    }
    if (config->max_message_size < SIGNALING_MIN_MESSAGE_SIZE ||
        config->max_message_size > SIGNALING_MAX_MESSAGE_SIZE) {
        TLOG_ERRORF("Invalid max_message_size: {}", config->max_message_size);
        return -1;
    }
    if (config->messages_per_second < 1 ||
        config->messages_per_second > SIGNALING_MAX_MESSAGES_PER_SECOND) {
        TLOG_ERRORF("Invalid messages_per_second: {}",
                   config->messages_per_second);
        return -1;
    }
    if (config->message_burst < 1 ||
        config->message_burst > SIGNALING_MAX_MESSAGE_BURST) {
        TLOG_ERRORF("Invalid message_burst: {}", config->message_burst);
        return -1;
    }
    if (config->max_outbox_messages < 1 ||
        config->max_outbox_messages > SIGNALING_MAX_OUTBOX_MESSAGES) {
        TLOG_ERRORF("Invalid max_outbox_messages: {}",
                   config->max_outbox_messages);
        return -1;
    }
    if (config->max_outbox_bytes < config->max_message_size ||
        config->max_outbox_bytes > SIGNALING_MAX_OUTBOX_BYTES) {
        TLOG_ERRORF("Invalid max_outbox_bytes: {}", config->max_outbox_bytes);
        return -1;
    }
    if (config->max_connections_per_source < 0 ||
        config->max_connections_per_source >
            SIGNALING_MAX_CONNECTIONS_PER_SOURCE) {
        TLOG_ERRORF("Invalid max_connections_per_source: {}",
                   config->max_connections_per_source);
        return -1;
    }
    if (config->source_admissions_per_second < 0 ||
        config->source_admissions_per_second >
            SIGNALING_MAX_SOURCE_ADMISSIONS_PER_SECOND) {
        TLOG_ERRORF("Invalid source_admissions_per_second: {}",
                   config->source_admissions_per_second);
        return -1;
    }
    if (config->source_admission_burst < 0 ||
        config->source_admission_burst >
            SIGNALING_MAX_SOURCE_ADMISSION_BURST ||
        ((config->source_admissions_per_second == 0) !=
         (config->source_admission_burst == 0))) {
        TLOG_ERRORF("Invalid source_admission_burst: {}",
                   config->source_admission_burst);
        return -1;
    }
    if (config->max_source_states < 0 ||
        config->max_source_states > SIGNALING_MAX_SOURCE_STATES) {
        TLOG_ERRORF("Invalid max_source_states: {}", config->max_source_states);
        return -1;
    }
    if (config->source_state_ttl_ms < 0 ||
        config->source_state_ttl_ms > SIGNALING_MAX_SOURCE_STATE_TTL_MS ||
        ((config->max_connections_per_source > 0 ||
          config->source_admissions_per_second > 0)
             ? (config->max_source_states < 1 ||
                config->source_state_ttl_ms <
                    SIGNALING_MIN_SOURCE_STATE_TTL_MS)
             : (config->max_source_states != 0 ||
                config->source_state_ttl_ms != 0))) {
        TLOG_ERRORF("Invalid source_state_ttl_ms: {}",
                   config->source_state_ttl_ms);
        return -1;
    }
    
    /* Validate signaling peer admission tokens. */
    {
        turbo_media_auth_config_t auth_config = {
            .issuer = config->jwt_issuer,
            .active_key_id = config->jwt_active_key_id,
            .active_secret = config->jwt_secret,
            .previous_key_id = config->jwt_previous_key_id,
            .previous_secret = config->jwt_previous_secret,
            .revoked_token_sha256 = config->jwt_revoked_token_sha256,
            .clock_skew_seconds = config->jwt_clock_skew_seconds,
            .max_ttl_seconds = config->jwt_ttl_seconds
        };

        if ((config->jwt_enabled != 0 && config->jwt_enabled != 1) ||
            !config->jwt_algorithm ||
            strcmp(config->jwt_algorithm, "HS256") != 0 ||
            config->jwt_clock_skew_seconds < 0 ||
            config->jwt_ttl_seconds < 1 ||
            (config->jwt_enabled &&
             turbo_media_auth_config_validate(&auth_config) != 0)) {
            TLOG_ERROR("Invalid signaling peer admission token configuration");
            return -1;
        }
    }

    if (config->ws_use_tls &&
        (!config->ws_cert_file || config->ws_cert_file[0] == '\0' ||
         !config->ws_key_file || config->ws_key_file[0] == '\0')) {
        TLOG_ERROR("WSS requires cert_file and key_file");
        return -1;
    }
    if (config->http_enabled && config->http_use_tls &&
        (!config->http_cert_file || config->http_cert_file[0] == '\0' ||
         !config->http_key_file || config->http_key_file[0] == '\0')) {
        TLOG_ERROR("HTTPS management API requires cert_file and key_file");
        return -1;
    }

    if (config->redis_port < 1 || config->redis_port > 65535 ||
        config->redis_db < 0 ||
        config->redis_stream_read_interval_ms < 1 ||
        config->redis_stream_max_len < 1) {
        TLOG_ERROR("Invalid Redis configuration");
        return -1;
    }
    if (config->redis_enabled) {
        TLOG_ERROR("Redis integration is not implemented by the current signaling server");
        return -1;
    }

    if (!config_string_in_set(config->log_level, log_levels,
                              sizeof(log_levels) / sizeof(log_levels[0])) ||
        !config_string_in_set(config->log_format, log_formats,
                              sizeof(log_formats) / sizeof(log_formats[0])) ||
        !config_string_in_set(config->log_output, log_outputs,
                              sizeof(log_outputs) / sizeof(log_outputs[0]))) {
        TLOG_ERROR("Invalid logging configuration");
        return -1;
    }

    return 0;
}

/**
 * Print configuration (for debugging)
 */
void signaling_server_config_print(const signaling_server_config_t *config) {
    if (!config) {
        return;
    }
    TLOG_DEBUG("Configuration:");
    TLOG_DEBUGF("  Node ID: {}", config->node_id ? config->node_id : "auto");
    TLOG_DEBUGF("  WebSocket: {}:{}", config->ws_host, config->ws_port);
    TLOG_DEBUGF("  WebSocket TLS: {}", config->ws_use_tls ? "enabled" : "disabled");
    TLOG_DEBUGF("  HTTP API: {}:{}", config->http_host, config->http_port);
    TLOG_DEBUGF("  HTTP API: {}", config->http_enabled ? "enabled" : "disabled");
    TLOG_DEBUGF("  HTTP API TLS: {}", config->http_use_tls ? "enabled" : "disabled");
    TLOG_DEBUGF("  HTTP management auth: {}",
               config->http_auth_enabled ? "enabled" : "disabled");
    TLOG_DEBUGF("  HTTP management scoped auth: {}",
               (config->http_auth_active_secret &&
                config->http_auth_active_secret[0])
                   ? "enabled"
                   : "disabled");
    TLOG_DEBUGF("  Max Peers: {}", config->max_peers);
    TLOG_DEBUGF("  Max Rooms: {}", config->max_rooms);
    TLOG_DEBUGF("  Peer Timeout: {}ms", config->peer_timeout_ms);
    TLOG_DEBUGF("  Join Timeout: {}ms", config->join_timeout_ms);
    TLOG_DEBUGF("  WebSocket Message Limit: {} bytes",
               config->max_message_size);
    TLOG_DEBUGF("  Message Rate: {}/s, burst {}",
               config->messages_per_second, config->message_burst);
    TLOG_DEBUGF("  Outbox Limit: {} messages / {} bytes",
               config->max_outbox_messages, config->max_outbox_bytes);
    TLOG_DEBUGF("  Source Limit: {} active, {}/s burst {}",
               config->max_connections_per_source,
               config->source_admissions_per_second,
               config->source_admission_burst);
    TLOG_DEBUGF("  Source State: {} entries / {}ms TTL",
               config->max_source_states, config->source_state_ttl_ms);
    TLOG_DEBUGF("  Peer admission auth: {}",
               config->jwt_enabled ? "enabled" : "disabled");
    TLOG_DEBUGF("  Redis: {}", config->redis_enabled ? "enabled" : "disabled");
    if (config->redis_enabled) {
        TLOG_DEBUGF("  Redis Host: {}:{}", config->redis_host, config->redis_port);
        TLOG_DEBUGF("  Redis Streams: {}", config->redis_use_streams ? "enabled" : "disabled");
    }
    TLOG_DEBUGF("  Log Level: {}", config->log_level);
}
