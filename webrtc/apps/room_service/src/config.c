#include "room_service/config.h"
#include "../../config_toml.h"
#include "turbo_media_auth.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum room_service_config_string_e {
    ROOM_CONFIG_STRING_FILE = 0,
    ROOM_CONFIG_STRING_BIND_HOST,
    ROOM_CONFIG_STRING_TLS_CERT_FILE,
    ROOM_CONFIG_STRING_TLS_KEY_FILE,
    ROOM_CONFIG_STRING_NODE_ID,
    ROOM_CONFIG_STRING_CONTROL_TOKEN,
    ROOM_CONFIG_STRING_AUTH_ISSUER,
    ROOM_CONFIG_STRING_AUTH_ACTIVE_KEY_ID,
    ROOM_CONFIG_STRING_AUTH_ACTIVE_SECRET,
    ROOM_CONFIG_STRING_AUTH_PREVIOUS_KEY_ID,
    ROOM_CONFIG_STRING_AUTH_PREVIOUS_SECRET,
    ROOM_CONFIG_STRING_AUTH_REVOKED_TOKEN_SHA256,
    ROOM_CONFIG_STRING_SFU_CONTROL_URL,
    ROOM_CONFIG_STRING_SFU_NODES,
    ROOM_CONFIG_STRING_SFU_CONTROL_TOKEN,
    ROOM_CONFIG_STRING_SFU_CA_FILE,
    ROOM_CONFIG_STRING_SFU_AUTH_ISSUER,
    ROOM_CONFIG_STRING_SFU_AUTH_KEY_ID,
    ROOM_CONFIG_STRING_SFU_AUTH_SECRET,
    ROOM_CONFIG_STRING_LOG_LEVEL,
    ROOM_CONFIG_STRING_COUNT
} room_service_config_string_t;

static const char *room_service_env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

static int room_service_env_bool(const char *name, int current) {
    const char *value = getenv(name);

    if (!value || value[0] == '\0') {
        return current;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        return 0;
    }
    return 2;
}

static int room_service_env_int(const char *name, int current) {
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

static int room_service_has_nonspace(const char *start, const char *end) {
    while (start < end) {
        if (!isspace((unsigned char)*start)) {
            return 1;
        }
        start++;
    }
    return 0;
}

static int room_service_sfu_nodes_syntax_valid(const char *nodes) {
    const char *entry;

    if (!nodes || nodes[0] == '\0') {
        return 1;
    }

    entry = nodes;
    while (*entry) {
        const char *end = strchr(entry, ',');
        const char *equals;

        if (!end) {
            end = entry + strlen(entry);
        }
        equals = memchr(entry, '=', (size_t)(end - entry));
        if (!equals ||
            !room_service_has_nonspace(entry, equals) ||
            !room_service_has_nonspace(equals + 1, end)) {
            return 0;
        }
        entry = (*end == ',') ? end + 1 : end;
    }

    return 1;
}

void room_service_app_config_init(room_service_app_config_t *config) {
    if (!config) {
        return;
    }

    config->config_file = NULL;
    config->bind_host = "0.0.0.0";
    config->bind_port = 9090;
    config->use_tls = 0;
    config->tls_cert_file = NULL;
    config->tls_key_file = NULL;
    config->node_id = "room-service-local";
    config->control_token = room_service_env_value("TURBO_ROOM_SERVICE_CONTROL_TOKEN");
    config->auth_issuer = "turbomedia";
    config->auth_active_key_id =
        room_service_env_value("TURBO_ROOM_SERVICE_AUTH_ACTIVE_KEY_ID");
    config->auth_active_secret =
        room_service_env_value("TURBO_ROOM_SERVICE_AUTH_ACTIVE_SECRET");
    config->auth_previous_key_id =
        room_service_env_value("TURBO_ROOM_SERVICE_AUTH_PREVIOUS_KEY_ID");
    config->auth_previous_secret =
        room_service_env_value("TURBO_ROOM_SERVICE_AUTH_PREVIOUS_SECRET");
    config->auth_revoked_token_sha256 =
        room_service_env_value(
            "TURBO_ROOM_SERVICE_AUTH_REVOKED_TOKEN_SHA256");
    config->auth_clock_skew_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_CLOCK_SKEW_SECONDS;
    config->auth_max_ttl_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS;
    config->sfu_control_url = NULL;
    config->sfu_nodes = room_service_env_value("TURBO_ROOM_SERVICE_SFU_NODES");
    config->sfu_control_token = room_service_env_value("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN");
    config->sfu_ca_file = room_service_env_value("TURBO_ROOM_SERVICE_SFU_CA_FILE");
    config->sfu_auth_issuer = "turbomedia";
    config->sfu_auth_key_id =
        room_service_env_value("TURBO_ROOM_SERVICE_SFU_AUTH_KEY_ID");
    config->sfu_auth_secret =
        room_service_env_value("TURBO_ROOM_SERVICE_SFU_AUTH_SECRET");
    config->sfu_auth_ttl_seconds = 60;
    config->max_rooms = 1024;
    config->auto_create_rooms = 1;
    config->dry_run = 0;
    config->log_level = "info";
    config->private_data = NULL;
}

static int room_service_config_clone_strings(
    const room_service_app_config_t *source,
    room_service_app_config_t *candidate,
    rtc_app_config_storage_t *storage) {
#define ROOM_CONFIG_CLONE(index, field)                                            \
    do {                                                                            \
        if (rtc_app_config_storage_copy(storage, index, source->field,              \
                                        &candidate->field) != 0) {                   \
            return -1;                                                              \
        }                                                                           \
    } while (0)

    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_FILE, config_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_BIND_HOST, bind_host);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_TLS_CERT_FILE, tls_cert_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_TLS_KEY_FILE, tls_key_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_NODE_ID, node_id);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_CONTROL_TOKEN, control_token);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_AUTH_ISSUER, auth_issuer);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_AUTH_ACTIVE_KEY_ID, auth_active_key_id);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_AUTH_ACTIVE_SECRET, auth_active_secret);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_AUTH_PREVIOUS_KEY_ID, auth_previous_key_id);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_AUTH_PREVIOUS_SECRET, auth_previous_secret);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_AUTH_REVOKED_TOKEN_SHA256,
                      auth_revoked_token_sha256);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_CONTROL_URL, sfu_control_url);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_NODES, sfu_nodes);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_CONTROL_TOKEN, sfu_control_token);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_CA_FILE, sfu_ca_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_AUTH_ISSUER, sfu_auth_issuer);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_AUTH_KEY_ID, sfu_auth_key_id);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_SFU_AUTH_SECRET, sfu_auth_secret);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_LOG_LEVEL, log_level);

#undef ROOM_CONFIG_CLONE
    return 0;
}

static int room_service_config_apply_server(
    const turbo_toml_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "host", "port", "use_tls", "cert_file", "key_file", "node_id"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "server", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "server", "host", storage,
                                  ROOM_CONFIG_STRING_BIND_HOST,
                                  &config->bind_host) != 0 ||
        rtc_app_toml_apply_int(table, "server", "port", &config->bind_port) != 0 ||
        rtc_app_toml_apply_bool(table, "server", "use_tls",
                                &config->use_tls) != 0 ||
        rtc_app_toml_apply_string(table, "server", "cert_file", storage,
                                  ROOM_CONFIG_STRING_TLS_CERT_FILE,
                                  &config->tls_cert_file) != 0 ||
        rtc_app_toml_apply_string(table, "server", "key_file", storage,
                                  ROOM_CONFIG_STRING_TLS_KEY_FILE,
                                  &config->tls_key_file) != 0 ||
        rtc_app_toml_apply_string(table, "server", "node_id", storage,
                                  ROOM_CONFIG_STRING_NODE_ID,
                                  &config->node_id) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_control(
    const turbo_toml_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {"token"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "control", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "control", "token", storage,
                                  ROOM_CONFIG_STRING_CONTROL_TOKEN,
                                  &config->control_token) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_auth(
    const turbo_toml_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "issuer", "active_key_id", "active_secret", "previous_key_id",
        "previous_secret", "revoked_token_sha256", "clock_skew_seconds",
        "max_ttl_seconds"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "auth", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "issuer", storage,
                                  ROOM_CONFIG_STRING_AUTH_ISSUER,
                                  &config->auth_issuer) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "active_key_id", storage,
                                  ROOM_CONFIG_STRING_AUTH_ACTIVE_KEY_ID,
                                  &config->auth_active_key_id) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "active_secret", storage,
                                  ROOM_CONFIG_STRING_AUTH_ACTIVE_SECRET,
                                  &config->auth_active_secret) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "previous_key_id", storage,
                                  ROOM_CONFIG_STRING_AUTH_PREVIOUS_KEY_ID,
                                  &config->auth_previous_key_id) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "previous_secret", storage,
                                  ROOM_CONFIG_STRING_AUTH_PREVIOUS_SECRET,
                                  &config->auth_previous_secret) != 0 ||
        rtc_app_toml_apply_string(
            table, "auth", "revoked_token_sha256", storage,
            ROOM_CONFIG_STRING_AUTH_REVOKED_TOKEN_SHA256,
            &config->auth_revoked_token_sha256) != 0 ||
        rtc_app_toml_apply_int(table, "auth", "clock_skew_seconds",
                               &config->auth_clock_skew_seconds) != 0 ||
        rtc_app_toml_apply_int(table, "auth", "max_ttl_seconds",
                               &config->auth_max_ttl_seconds) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_sfu(
    const turbo_toml_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "control_url", "nodes", "control_token", "ca_file"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "sfu", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "sfu", "control_url", storage,
                                  ROOM_CONFIG_STRING_SFU_CONTROL_URL,
                                  &config->sfu_control_url) != 0 ||
        rtc_app_toml_apply_string(table, "sfu", "nodes", storage,
                                  ROOM_CONFIG_STRING_SFU_NODES,
                                  &config->sfu_nodes) != 0 ||
        rtc_app_toml_apply_string(table, "sfu", "control_token", storage,
                                  ROOM_CONFIG_STRING_SFU_CONTROL_TOKEN,
                                  &config->sfu_control_token) != 0 ||
        rtc_app_toml_apply_string(table, "sfu", "ca_file", storage,
                                  ROOM_CONFIG_STRING_SFU_CA_FILE,
                                  &config->sfu_ca_file) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_sfu_auth(
    const turbo_toml_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "issuer", "key_id", "secret", "ttl_seconds"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "sfu_auth", allowed,
            sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "sfu_auth", "issuer", storage,
                                  ROOM_CONFIG_STRING_SFU_AUTH_ISSUER,
                                  &config->sfu_auth_issuer) != 0 ||
        rtc_app_toml_apply_string(table, "sfu_auth", "key_id", storage,
                                  ROOM_CONFIG_STRING_SFU_AUTH_KEY_ID,
                                  &config->sfu_auth_key_id) != 0 ||
        rtc_app_toml_apply_string(table, "sfu_auth", "secret", storage,
                                  ROOM_CONFIG_STRING_SFU_AUTH_SECRET,
                                  &config->sfu_auth_secret) != 0 ||
        rtc_app_toml_apply_int(table, "sfu_auth", "ttl_seconds",
                               &config->sfu_auth_ttl_seconds) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_capacity(
    const turbo_toml_t *table,
    room_service_app_config_t *config) {
    static const char *const allowed[] = {"max_rooms"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "capacity", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_int(table, "capacity", "max_rooms",
                               &config->max_rooms) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_rooms(
    const turbo_toml_t *table,
    room_service_app_config_t *config) {
    static const char *const allowed[] = {"auto_create"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "rooms", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_bool(table, "rooms", "auto_create",
                                &config->auto_create_rooms) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_runtime(
    const turbo_toml_t *table,
    room_service_app_config_t *config) {
    static const char *const allowed[] = {"dry_run"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "runtime", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_bool(table, "runtime", "dry_run",
                                &config->dry_run) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_logging(
    const turbo_toml_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {"level"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "logging", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "logging", "level", storage,
                                  ROOM_CONFIG_STRING_LOG_LEVEL,
                                  &config->log_level) != 0) {
        return -1;
    }
    return 0;
}

int room_service_app_config_load(room_service_app_config_t *config,
                                 const char *filename) {
    static const char *const root_keys[] = {
        "server", "control", "auth", "sfu", "sfu_auth", "capacity",
        "rooms", "runtime", "logging"
    };
    rtc_app_toml_document_t document;
    rtc_app_config_storage_t *storage = NULL;
    rtc_app_config_storage_t *old_storage;
    turbo_toml_t *server = NULL;
    turbo_toml_t *control = NULL;
    turbo_toml_t *auth = NULL;
    turbo_toml_t *sfu = NULL;
    turbo_toml_t *sfu_auth = NULL;
    turbo_toml_t *capacity = NULL;
    turbo_toml_t *rooms = NULL;
    turbo_toml_t *runtime = NULL;
    turbo_toml_t *logging = NULL;
    room_service_app_config_t candidate;
    int result = -1;

    if (!config || !filename || filename[0] == '\0') {
        TLOG_ERROR("Room service configuration load requires a non-empty filename");
        return -1;
    }
    storage = rtc_app_config_storage_create(ROOM_CONFIG_STRING_COUNT);
    if (!storage) {
        TLOG_ERROR("Failed to allocate room service configuration storage");
        return -1;
    }
    candidate = *config;
    candidate.private_data = storage;
    if (room_service_config_clone_strings(config, &candidate, storage) != 0 ||
        rtc_app_config_storage_copy(storage, ROOM_CONFIG_STRING_FILE, filename,
                                    &candidate.config_file) != 0) {
        TLOG_ERROR("Failed to copy room service configuration strings");
        goto cleanup_storage;
    }
    if (rtc_app_toml_document_open(&document, filename) != 0) {
        goto cleanup_storage;
    }

    if (rtc_app_toml_table_keys_valid(
            document.root, "root", root_keys,
            sizeof(root_keys) / sizeof(root_keys[0])) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "server", &server) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "control", &control) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "auth", &auth) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "sfu", &sfu) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "sfu_auth", &sfu_auth) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "capacity", &capacity) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "rooms", &rooms) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "runtime", &runtime) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "logging", &logging) != 0 ||
        room_service_config_apply_server(server, &candidate, storage) != 0 ||
        room_service_config_apply_control(control, &candidate, storage) != 0 ||
        room_service_config_apply_auth(auth, &candidate, storage) != 0 ||
        room_service_config_apply_sfu(sfu, &candidate, storage) != 0 ||
        room_service_config_apply_sfu_auth(sfu_auth, &candidate, storage) != 0 ||
        room_service_config_apply_capacity(capacity, &candidate) != 0 ||
        room_service_config_apply_rooms(rooms, &candidate) != 0 ||
        room_service_config_apply_runtime(runtime, &candidate) != 0 ||
        room_service_config_apply_logging(logging, &candidate, storage) != 0 ||
        room_service_app_config_validate(&candidate) != 0) {
        goto cleanup_document;
    }

    old_storage = (rtc_app_config_storage_t *)config->private_data;
    *config = candidate;
    storage = NULL;
    rtc_app_config_storage_destroy(old_storage);
    result = 0;

cleanup_document:
    rtc_app_toml_document_close(&document);
cleanup_storage:
    rtc_app_config_storage_destroy(storage);
    return result;
}

void room_service_app_config_apply_environment(
    room_service_app_config_t *config) {
    const char *value;

    if (!config) {
        return;
    }
    config->use_tls = room_service_env_bool(
        "TURBO_ROOM_SERVICE_USE_TLS", config->use_tls);
    value = room_service_env_value("TURBO_ROOM_SERVICE_TLS_CERT_FILE");
    if (value) {
        config->tls_cert_file = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_TLS_KEY_FILE");
    if (value) {
        config->tls_key_file = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_CONTROL_TOKEN");
    if (value) {
        config->control_token = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_AUTH_ISSUER");
    if (value) {
        config->auth_issuer = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_AUTH_ACTIVE_KEY_ID");
    if (value) {
        config->auth_active_key_id = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_AUTH_ACTIVE_SECRET");
    if (value) {
        config->auth_active_secret = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_AUTH_PREVIOUS_KEY_ID");
    if (value) {
        config->auth_previous_key_id = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_AUTH_PREVIOUS_SECRET");
    if (value) {
        config->auth_previous_secret = value;
    }
    value = room_service_env_value(
        "TURBO_ROOM_SERVICE_AUTH_REVOKED_TOKEN_SHA256");
    if (value) {
        config->auth_revoked_token_sha256 = value;
    }
    config->auth_clock_skew_seconds = room_service_env_int(
        "TURBO_ROOM_SERVICE_AUTH_CLOCK_SKEW_SECONDS",
        config->auth_clock_skew_seconds);
    config->auth_max_ttl_seconds = room_service_env_int(
        "TURBO_ROOM_SERVICE_AUTH_MAX_TTL_SECONDS",
        config->auth_max_ttl_seconds);
    value = room_service_env_value("TURBO_ROOM_SERVICE_SFU_NODES");
    if (value) {
        config->sfu_nodes = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN");
    if (value) {
        config->sfu_control_token = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_SFU_CA_FILE");
    if (value) {
        config->sfu_ca_file = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_SFU_AUTH_ISSUER");
    if (value) {
        config->sfu_auth_issuer = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_SFU_AUTH_KEY_ID");
    if (value) {
        config->sfu_auth_key_id = value;
    }
    value = room_service_env_value("TURBO_ROOM_SERVICE_SFU_AUTH_SECRET");
    if (value) {
        config->sfu_auth_secret = value;
    }
    config->sfu_auth_ttl_seconds = room_service_env_int(
        "TURBO_ROOM_SERVICE_SFU_AUTH_TTL_SECONDS",
        config->sfu_auth_ttl_seconds);
}

void room_service_app_config_cleanup(room_service_app_config_t *config) {
    if (!config) {
        return;
    }
    rtc_app_config_storage_destroy(
        (rtc_app_config_storage_t *)config->private_data);
    config->private_data = NULL;
}

int room_service_app_config_validate(const room_service_app_config_t *config) {
    static const char *const log_levels[] = {
        "trace", "debug", "info", "warn", "error"
    };

    if (!config || !config->bind_host || config->bind_host[0] == '\0' ||
        !config->node_id || config->node_id[0] == '\0' ||
        !config->log_level || config->log_level[0] == '\0') {
        return -1;
    }
    if (config->bind_port < 1 || config->bind_port > 65535 ||
        config->max_rooms <= 0) {
        return -1;
    }
    if ((config->auto_create_rooms != 0 && config->auto_create_rooms != 1) ||
        (config->dry_run != 0 && config->dry_run != 1) ||
        (config->use_tls != 0 && config->use_tls != 1) ||
        !rtc_app_config_string_in_set(
            config->log_level, log_levels,
            sizeof(log_levels) / sizeof(log_levels[0]))) {
        return -1;
    }
    if ((config->control_token && config->control_token[0] == '\0') ||
        (config->sfu_control_url && config->sfu_control_url[0] == '\0') ||
        (config->sfu_nodes && config->sfu_nodes[0] == '\0') ||
        (config->sfu_control_token && config->sfu_control_token[0] == '\0') ||
        (config->sfu_ca_file && config->sfu_ca_file[0] == '\0')) {
        return -1;
    }
    {
        turbo_media_auth_config_t auth_config = {
            .issuer = config->auth_issuer,
            .active_key_id = config->auth_active_key_id,
            .active_secret = config->auth_active_secret,
            .previous_key_id = config->auth_previous_key_id,
            .previous_secret = config->auth_previous_secret,
            .revoked_token_sha256 = config->auth_revoked_token_sha256,
            .clock_skew_seconds = config->auth_clock_skew_seconds,
            .max_ttl_seconds = config->auth_max_ttl_seconds
        };
        int any_signed_auth =
            (config->auth_active_key_id &&
             config->auth_active_key_id[0] != '\0') ||
            (config->auth_active_secret &&
             config->auth_active_secret[0] != '\0') ||
            (config->auth_previous_key_id &&
             config->auth_previous_key_id[0] != '\0') ||
            (config->auth_previous_secret &&
             config->auth_previous_secret[0] != '\0') ||
            (config->auth_revoked_token_sha256 &&
             config->auth_revoked_token_sha256[0] != '\0');

        if ((any_signed_auth &&
             turbo_media_auth_config_validate(&auth_config) != 0) ||
            (!any_signed_auth &&
             (!config->auth_issuer || config->auth_issuer[0] == '\0' ||
              config->auth_clock_skew_seconds < 0 ||
              config->auth_max_ttl_seconds < 1))) {
            return -1;
        }
    }
    {
        turbo_media_auth_config_t sfu_auth_config = {
            .issuer = config->sfu_auth_issuer,
            .active_key_id = config->sfu_auth_key_id,
            .active_secret = config->sfu_auth_secret,
            .clock_skew_seconds = 0,
            .max_ttl_seconds = config->sfu_auth_ttl_seconds
        };
        int any_sfu_signed_auth =
            (config->sfu_auth_key_id &&
             config->sfu_auth_key_id[0] != '\0') ||
            (config->sfu_auth_secret &&
             config->sfu_auth_secret[0] != '\0');

        if ((any_sfu_signed_auth &&
             turbo_media_auth_config_validate(&sfu_auth_config) != 0) ||
            (!any_sfu_signed_auth &&
             (!config->sfu_auth_issuer ||
              config->sfu_auth_issuer[0] == '\0' ||
              config->sfu_auth_ttl_seconds < 1))) {
            return -1;
        }
    }
    if (config->use_tls &&
        (!config->tls_cert_file || config->tls_cert_file[0] == '\0' ||
         !config->tls_key_file || config->tls_key_file[0] == '\0')) {
        return -1;
    }
    if (!room_service_sfu_nodes_syntax_valid(config->sfu_nodes)) {
        return -1;
    }
    return 0;
}

void room_service_app_config_print(const room_service_app_config_t *config) {
    if (!config) {
        return;
    }

    printf("Room Service Configuration\n");
    printf("  bind: %s:%d\n", config->bind_host, config->bind_port);
    printf("  TLS: %s\n", config->use_tls ? "enabled" : "disabled");
    printf("  node_id: %s\n", config->node_id);
    printf("  control_auth: %s\n",
           ((config->control_token && config->control_token[0]) ||
            (config->auth_active_secret && config->auth_active_secret[0]))
               ? "enabled"
               : "disabled");
    printf("  sfu_control_url: %s\n",
           config->sfu_control_url ? config->sfu_control_url : "(disabled)");
    printf("  sfu_nodes: %s\n", config->sfu_nodes ? config->sfu_nodes : "(none)");
    printf("  sfu_control_auth: %s\n",
           (config->sfu_auth_secret && config->sfu_auth_secret[0])
               ? "scoped signed token"
               : ((config->sfu_control_token && config->sfu_control_token[0])
                      ? "static token"
                      : "disabled"));
    printf("  sfu_private_ca: %s\n",
           config->sfu_ca_file ? "configured" : "system trust");
    printf("  max_rooms: %d\n", config->max_rooms);
    printf("  auto_create_rooms: %s\n", config->auto_create_rooms ? "true" : "false");
    printf("  dry_run: %s\n", config->dry_run ? "true" : "false");
    printf("  log_level: %s\n", config->log_level);
}
