#include "sfu_node/config.h"
#include "../../config_toml.h"
#include "turbo_media_auth.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum sfu_node_config_string_e {
    SFU_CONFIG_STRING_FILE = 0,
    SFU_CONFIG_STRING_BIND_HOST,
    SFU_CONFIG_STRING_TLS_CERT_FILE,
    SFU_CONFIG_STRING_TLS_KEY_FILE,
    SFU_CONFIG_STRING_NODE_ID,
    SFU_CONFIG_STRING_LOG_LEVEL,
    SFU_CONFIG_STRING_CONTROL_TOKEN,
    SFU_CONFIG_STRING_MEDIA_ACCESS_TOKEN,
    SFU_CONFIG_STRING_AUTH_ISSUER,
    SFU_CONFIG_STRING_AUTH_ACTIVE_KEY_ID,
    SFU_CONFIG_STRING_AUTH_ACTIVE_SECRET,
    SFU_CONFIG_STRING_AUTH_PREVIOUS_KEY_ID,
    SFU_CONFIG_STRING_AUTH_PREVIOUS_SECRET,
    SFU_CONFIG_STRING_AUTH_REVOKED_TOKEN_SHA256,
    SFU_CONFIG_STRING_STUN_SERVER_0,
    SFU_CONFIG_STRING_STUN_SERVER_1,
    SFU_CONFIG_STRING_STUN_SERVER_2,
    SFU_CONFIG_STRING_STUN_SERVER_3,
    SFU_CONFIG_STRING_TURN_SERVER_0,
    SFU_CONFIG_STRING_TURN_SERVER_1,
    SFU_CONFIG_STRING_TURN_SERVER_2,
    SFU_CONFIG_STRING_TURN_SERVER_3,
    SFU_CONFIG_STRING_COUNT
} sfu_node_config_string_t;

static const char *sfu_node_env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

static int sfu_node_env_bool(const char *name, int current) {
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

static int sfu_node_env_int(const char *name, int current) {
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

void sfu_node_app_config_init(sfu_node_app_config_t *config) {
    if (!config) {
        return;
    }
    config->config_file = NULL;
    config->bind_host = "0.0.0.0";
    config->bind_port = 9190;
    config->use_tls = 0;
    config->tls_cert_file = NULL;
    config->tls_key_file = NULL;
    config->node_id = "sfu-node-local";
    config->max_rooms = 128;
    config->default_room_capacity = 50;
    config->dry_run = 0;
    config->log_level = "info";
    config->control_token = sfu_node_env_value("TURBO_SFU_NODE_CONTROL_TOKEN");
    config->media_access_token =
        sfu_node_env_value("TURBO_SFU_MEDIA_ACCESS_TOKEN");
    config->auth_issuer = "turbomedia";
    config->auth_active_key_id =
        sfu_node_env_value("TURBO_SFU_AUTH_ACTIVE_KEY_ID");
    config->auth_active_secret =
        sfu_node_env_value("TURBO_SFU_AUTH_ACTIVE_SECRET");
    config->auth_previous_key_id =
        sfu_node_env_value("TURBO_SFU_AUTH_PREVIOUS_KEY_ID");
    config->auth_previous_secret =
        sfu_node_env_value("TURBO_SFU_AUTH_PREVIOUS_SECRET");
    config->auth_revoked_token_sha256 =
        sfu_node_env_value("TURBO_SFU_AUTH_REVOKED_TOKEN_SHA256");
    config->auth_dynamic_revocation_capacity = 0;
    config->auth_clock_skew_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_CLOCK_SKEW_SECONDS;
    config->auth_max_ttl_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS;
    for (int index = 0; index < TURBO_SFU_NODE_MAX_STUN_SERVERS; ++index) {
        config->stun_servers[index] = NULL;
    }
    config->stun_server_count = 0;
    for (int index = 0; index < TURBO_SFU_NODE_MAX_TURN_SERVERS; ++index) {
        config->turn_servers[index] = NULL;
    }
    config->turn_server_count = 0;
    config->ice_allow_loopback = 0;
    config->private_data = NULL;
}

static int sfu_node_config_clone_strings(
    const sfu_node_app_config_t *source,
    sfu_node_app_config_t *candidate,
    rtc_app_config_storage_t *storage) {
#define SFU_CONFIG_CLONE(index, field)                                             \
    do {                                                                            \
        if (rtc_app_config_storage_copy(storage, index, source->field,              \
                                        &candidate->field) != 0) {                   \
            return -1;                                                              \
        }                                                                           \
    } while (0)

    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_FILE, config_file);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_BIND_HOST, bind_host);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_TLS_CERT_FILE, tls_cert_file);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_TLS_KEY_FILE, tls_key_file);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_NODE_ID, node_id);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_LOG_LEVEL, log_level);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_CONTROL_TOKEN, control_token);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_MEDIA_ACCESS_TOKEN, media_access_token);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_AUTH_ISSUER, auth_issuer);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_AUTH_ACTIVE_KEY_ID,
                     auth_active_key_id);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_AUTH_ACTIVE_SECRET,
                     auth_active_secret);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_AUTH_PREVIOUS_KEY_ID,
                     auth_previous_key_id);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_AUTH_PREVIOUS_SECRET,
                     auth_previous_secret);
    SFU_CONFIG_CLONE(SFU_CONFIG_STRING_AUTH_REVOKED_TOKEN_SHA256,
                     auth_revoked_token_sha256);
    for (int index = 0; index < TURBO_SFU_NODE_MAX_STUN_SERVERS; ++index) {
        if (rtc_app_config_storage_copy(
                storage, SFU_CONFIG_STRING_STUN_SERVER_0 + (size_t)index,
                source->stun_servers[index],
                &candidate->stun_servers[index]) != 0) {
            return -1;
        }
    }
    for (int index = 0; index < TURBO_SFU_NODE_MAX_TURN_SERVERS; ++index) {
        if (rtc_app_config_storage_copy(
                storage, SFU_CONFIG_STRING_TURN_SERVER_0 + (size_t)index,
                source->turn_servers[index],
                &candidate->turn_servers[index]) != 0) {
            return -1;
        }
    }

#undef SFU_CONFIG_CLONE
    return 0;
}

int sfu_node_app_config_copy(sfu_node_app_config_t *destination,
                             const sfu_node_app_config_t *source) {
    rtc_app_config_storage_t *storage;
    rtc_app_config_storage_t *old_storage;
    sfu_node_app_config_t candidate;

    if (!destination || !source || sfu_node_app_config_validate(source) != 0) {
        return -1;
    }
    storage = rtc_app_config_storage_create(SFU_CONFIG_STRING_COUNT);
    if (!storage) {
        return -1;
    }
    candidate = *source;
    candidate.private_data = storage;
    if (sfu_node_config_clone_strings(source, &candidate, storage) != 0) {
        rtc_app_config_storage_destroy(storage);
        return -1;
    }
    old_storage = (rtc_app_config_storage_t *)destination->private_data;
    *destination = candidate;
    rtc_app_config_storage_destroy(old_storage);
    return 0;
}

static int sfu_node_config_apply_server(
    const toml_table_t *table,
    sfu_node_app_config_t *config,
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
                                  SFU_CONFIG_STRING_BIND_HOST,
                                  &config->bind_host) != 0 ||
        rtc_app_toml_apply_int(table, "server", "port", &config->bind_port) != 0 ||
        rtc_app_toml_apply_bool(table, "server", "use_tls",
                                &config->use_tls) != 0 ||
        rtc_app_toml_apply_string(table, "server", "cert_file", storage,
                                  SFU_CONFIG_STRING_TLS_CERT_FILE,
                                  &config->tls_cert_file) != 0 ||
        rtc_app_toml_apply_string(table, "server", "key_file", storage,
                                  SFU_CONFIG_STRING_TLS_KEY_FILE,
                                  &config->tls_key_file) != 0 ||
        rtc_app_toml_apply_string(table, "server", "node_id", storage,
                                  SFU_CONFIG_STRING_NODE_ID,
                                  &config->node_id) != 0) {
        return -1;
    }
    return 0;
}

static int sfu_node_config_apply_capacity(
    const toml_table_t *table,
    sfu_node_app_config_t *config) {
    static const char *const allowed[] = {"max_rooms", "default_room_capacity"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "capacity", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_int(table, "capacity", "max_rooms",
                               &config->max_rooms) != 0 ||
        rtc_app_toml_apply_int(table, "capacity", "default_room_capacity",
                               &config->default_room_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int sfu_node_config_apply_control(
    const toml_table_t *table,
    sfu_node_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {"token"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "control", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "control", "token", storage,
                                  SFU_CONFIG_STRING_CONTROL_TOKEN,
                                  &config->control_token) != 0) {
        return -1;
    }
    return 0;
}

static int sfu_node_config_apply_media(
    const toml_table_t *table,
    sfu_node_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {"access_token"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "media", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "media", "access_token", storage,
                                  SFU_CONFIG_STRING_MEDIA_ACCESS_TOKEN,
                                  &config->media_access_token) != 0) {
        return -1;
    }
    return 0;
}

static int sfu_node_config_apply_auth(
    const toml_table_t *table,
    sfu_node_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "issuer", "active_key_id", "active_secret", "previous_key_id",
        "previous_secret", "revoked_token_sha256",
        "dynamic_revocation_capacity", "clock_skew_seconds",
        "max_ttl_seconds"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "auth", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "issuer", storage,
                                  SFU_CONFIG_STRING_AUTH_ISSUER,
                                  &config->auth_issuer) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "active_key_id", storage,
                                  SFU_CONFIG_STRING_AUTH_ACTIVE_KEY_ID,
                                  &config->auth_active_key_id) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "active_secret", storage,
                                  SFU_CONFIG_STRING_AUTH_ACTIVE_SECRET,
                                  &config->auth_active_secret) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "previous_key_id", storage,
                                  SFU_CONFIG_STRING_AUTH_PREVIOUS_KEY_ID,
                                  &config->auth_previous_key_id) != 0 ||
        rtc_app_toml_apply_string(table, "auth", "previous_secret", storage,
                                  SFU_CONFIG_STRING_AUTH_PREVIOUS_SECRET,
                                  &config->auth_previous_secret) != 0 ||
        rtc_app_toml_apply_string(
            table, "auth", "revoked_token_sha256", storage,
            SFU_CONFIG_STRING_AUTH_REVOKED_TOKEN_SHA256,
            &config->auth_revoked_token_sha256) != 0 ||
        rtc_app_toml_apply_int(table, "auth", "dynamic_revocation_capacity",
                               &config->auth_dynamic_revocation_capacity) != 0 ||
        rtc_app_toml_apply_int(table, "auth", "clock_skew_seconds",
                               &config->auth_clock_skew_seconds) != 0 ||
        rtc_app_toml_apply_int(table, "auth", "max_ttl_seconds",
                               &config->auth_max_ttl_seconds) != 0) {
        return -1;
    }
    return 0;
}

static int sfu_node_config_apply_ice(
    const toml_table_t *table,
    sfu_node_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "stun_servers", "turn_servers", "allow_loopback"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "ice", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string_array(
            table, "ice", "stun_servers", storage,
            SFU_CONFIG_STRING_STUN_SERVER_0, config->stun_servers,
            TURBO_SFU_NODE_MAX_STUN_SERVERS, &config->stun_server_count) != 0 ||
        rtc_app_toml_apply_string_array(
            table, "ice", "turn_servers", storage,
            SFU_CONFIG_STRING_TURN_SERVER_0, config->turn_servers,
            TURBO_SFU_NODE_MAX_TURN_SERVERS, &config->turn_server_count) != 0 ||
        rtc_app_toml_apply_bool(
            table, "ice", "allow_loopback", &config->ice_allow_loopback) != 0) {
        return -1;
    }
    return 0;
}

static int sfu_node_config_apply_runtime(
    const toml_table_t *table,
    sfu_node_app_config_t *config) {
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

static int sfu_node_config_apply_logging(
    const toml_table_t *table,
    sfu_node_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {"level"};

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "logging", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "logging", "level", storage,
                                  SFU_CONFIG_STRING_LOG_LEVEL,
                                  &config->log_level) != 0) {
        return -1;
    }
    return 0;
}

int sfu_node_app_config_load(sfu_node_app_config_t *config, const char *filename) {
    static const char *const root_keys[] = {
        "server", "capacity", "control", "media", "auth", "ice", "runtime",
        "logging"
    };
    rtc_app_toml_document_t document;
    rtc_app_config_storage_t *storage = NULL;
    rtc_app_config_storage_t *old_storage;
    toml_table_t *server = NULL;
    toml_table_t *capacity = NULL;
    toml_table_t *control = NULL;
    toml_table_t *media = NULL;
    toml_table_t *auth = NULL;
    toml_table_t *ice = NULL;
    toml_table_t *runtime = NULL;
    toml_table_t *logging = NULL;
    sfu_node_app_config_t candidate;
    int result = -1;

    if (!config || !filename || filename[0] == '\0') {
        TLOG_ERROR("SFU configuration load requires a non-empty filename");
        return -1;
    }
    storage = rtc_app_config_storage_create(SFU_CONFIG_STRING_COUNT);
    if (!storage) {
        TLOG_ERROR("Failed to allocate SFU configuration storage");
        return -1;
    }
    candidate = *config;
    candidate.private_data = storage;
    if (sfu_node_config_clone_strings(config, &candidate, storage) != 0 ||
        rtc_app_config_storage_copy(storage, SFU_CONFIG_STRING_FILE, filename,
                                    &candidate.config_file) != 0) {
        TLOG_ERROR("Failed to copy SFU configuration strings");
        goto cleanup_storage;
    }
    if (rtc_app_toml_document_open(&document, filename) != 0) {
        goto cleanup_storage;
    }

    if (rtc_app_toml_table_keys_valid(
            document.root, "root", root_keys,
            sizeof(root_keys) / sizeof(root_keys[0])) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "server", &server) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "capacity", &capacity) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "control", &control) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "media", &media) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "auth", &auth) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "ice", &ice) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "runtime", &runtime) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "logging", &logging) != 0 ||
        sfu_node_config_apply_server(server, &candidate, storage) != 0 ||
        sfu_node_config_apply_capacity(capacity, &candidate) != 0 ||
        sfu_node_config_apply_control(control, &candidate, storage) != 0 ||
        sfu_node_config_apply_media(media, &candidate, storage) != 0 ||
        sfu_node_config_apply_auth(auth, &candidate, storage) != 0 ||
        sfu_node_config_apply_ice(ice, &candidate, storage) != 0 ||
        sfu_node_config_apply_runtime(runtime, &candidate) != 0 ||
        sfu_node_config_apply_logging(logging, &candidate, storage) != 0 ||
        sfu_node_app_config_validate(&candidate) != 0) {
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

void sfu_node_app_config_apply_environment(sfu_node_app_config_t *config) {
    const char *control_token;
    const char *media_access_token;
    const char *stun_server;
    const char *turn_server;
    const char *value;

    if (!config) {
        return;
    }
    config->use_tls =
        sfu_node_env_bool("TURBO_SFU_USE_TLS", config->use_tls);
    value = sfu_node_env_value("TURBO_SFU_TLS_CERT_FILE");
    if (value) {
        config->tls_cert_file = value;
    }
    value = sfu_node_env_value("TURBO_SFU_TLS_KEY_FILE");
    if (value) {
        config->tls_key_file = value;
    }
    control_token = sfu_node_env_value("TURBO_SFU_NODE_CONTROL_TOKEN");
    if (control_token) {
        config->control_token = control_token;
    }
    media_access_token = sfu_node_env_value("TURBO_SFU_MEDIA_ACCESS_TOKEN");
    if (media_access_token) {
        config->media_access_token = media_access_token;
    }
    value = sfu_node_env_value("TURBO_SFU_AUTH_ISSUER");
    if (value) {
        config->auth_issuer = value;
    }
    value = sfu_node_env_value("TURBO_SFU_AUTH_ACTIVE_KEY_ID");
    if (value) {
        config->auth_active_key_id = value;
    }
    value = sfu_node_env_value("TURBO_SFU_AUTH_ACTIVE_SECRET");
    if (value) {
        config->auth_active_secret = value;
    }
    value = sfu_node_env_value("TURBO_SFU_AUTH_PREVIOUS_KEY_ID");
    if (value) {
        config->auth_previous_key_id = value;
    }
    value = sfu_node_env_value("TURBO_SFU_AUTH_PREVIOUS_SECRET");
    if (value) {
        config->auth_previous_secret = value;
    }
    value = sfu_node_env_value("TURBO_SFU_AUTH_REVOKED_TOKEN_SHA256");
    if (value) {
        config->auth_revoked_token_sha256 = value;
    }
    config->auth_dynamic_revocation_capacity = sfu_node_env_int(
        "TURBO_SFU_AUTH_DYNAMIC_REVOCATION_CAPACITY",
        config->auth_dynamic_revocation_capacity);
    config->auth_clock_skew_seconds = sfu_node_env_int(
        "TURBO_SFU_AUTH_CLOCK_SKEW_SECONDS",
        config->auth_clock_skew_seconds);
    config->auth_max_ttl_seconds = sfu_node_env_int(
        "TURBO_SFU_AUTH_MAX_TTL_SECONDS", config->auth_max_ttl_seconds);
    stun_server = sfu_node_env_value("TURBO_SFU_STUN_SERVER");
    if (stun_server) {
        for (int index = 0; index < TURBO_SFU_NODE_MAX_STUN_SERVERS; ++index) {
            config->stun_servers[index] = NULL;
        }
        config->stun_servers[0] = stun_server;
        config->stun_server_count = 1;
    }
    turn_server = sfu_node_env_value("TURBO_SFU_TURN_SERVER");
    if (turn_server) {
        for (int index = 0; index < TURBO_SFU_NODE_MAX_TURN_SERVERS; ++index) {
            config->turn_servers[index] = NULL;
        }
        config->turn_servers[0] = turn_server;
        config->turn_server_count = 1;
    }
}

void sfu_node_app_config_cleanup(sfu_node_app_config_t *config) {
    if (!config) {
        return;
    }
    rtc_app_config_storage_destroy(
        (rtc_app_config_storage_t *)config->private_data);
    config->private_data = NULL;
}

int sfu_node_app_config_validate(const sfu_node_app_config_t *config) {
    static const char *const log_levels[] = {
        "trace", "debug", "info", "warn", "error"
    };

    if (!config || !config->bind_host || config->bind_host[0] == '\0' ||
        !config->node_id || config->node_id[0] == '\0' ||
        !config->log_level || config->log_level[0] == '\0') {
        return -1;
    }
    if (config->bind_port < 1 || config->bind_port > 65535 ||
        config->max_rooms <= 0 ||
        config->default_room_capacity <= 0) {
        return -1;
    }
    if ((config->dry_run != 0 && config->dry_run != 1) ||
        (config->use_tls != 0 && config->use_tls != 1) ||
        !rtc_app_config_string_in_set(
            config->log_level, log_levels,
            sizeof(log_levels) / sizeof(log_levels[0]))) {
        return -1;
    }
    if (config->use_tls &&
        (!config->tls_cert_file || config->tls_cert_file[0] == '\0' ||
         !config->tls_key_file || config->tls_key_file[0] == '\0')) {
        return -1;
    }
    if (config->control_token && config->control_token[0] == '\0') {
        return -1;
    }
    if (config->media_access_token && config->media_access_token[0] == '\0') {
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
        if (config->auth_dynamic_revocation_capacity < 0 ||
            config->auth_dynamic_revocation_capacity >
                TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS ||
            (any_signed_auth &&
             turbo_media_auth_config_validate(&auth_config) != 0) ||
            (!any_signed_auth &&
             (!config->auth_issuer || config->auth_issuer[0] == '\0' ||
              config->auth_clock_skew_seconds < 0 ||
              config->auth_max_ttl_seconds < 1))) {
            return -1;
        }
        if (config->auth_dynamic_revocation_capacity > 0 &&
            (!config->use_tls || !config->auth_active_secret ||
             config->auth_active_secret[0] == '\0')) {
            return -1;
        }
    }
    if (config->stun_server_count < 0 ||
        config->stun_server_count > TURBO_SFU_NODE_MAX_STUN_SERVERS ||
        config->turn_server_count < 0 ||
        config->turn_server_count > TURBO_SFU_NODE_MAX_TURN_SERVERS ||
        (config->ice_allow_loopback != 0 && config->ice_allow_loopback != 1)) {
        return -1;
    }
    for (int index = 0; index < config->stun_server_count; ++index) {
        if (!config->stun_servers[index] ||
            strncmp(config->stun_servers[index], "stun:", 5) != 0 ||
            config->stun_servers[index][5] == '\0') {
            return -1;
        }
    }
    for (int index = 0; index < config->turn_server_count; ++index) {
        const char *turn = config->turn_servers[index];
        const char *colon;
        const char *at;
        if (!turn || strncmp(turn, "turn:", 5) != 0) {
            return -1;
        }
        colon = strchr(turn + 5, ':');
        at = colon ? strchr(colon + 1, '@') : NULL;
        if (!colon || colon == turn + 5 || !at || at == colon + 1 ||
            at[1] == '\0') {
            return -1;
        }
    }
    return 0;
}

void sfu_node_app_config_print(const sfu_node_app_config_t *config) {
    if (!config) {
        return;
    }

    printf("SFU Node Configuration\n");
    printf("  bind: %s:%d\n", config->bind_host, config->bind_port);
    printf("  TLS: %s\n", config->use_tls ? "enabled" : "disabled");
    printf("  node_id: %s\n", config->node_id);
    printf("  max_rooms: %d\n", config->max_rooms);
    printf("  default_room_capacity: %d\n", config->default_room_capacity);
    printf("  dry_run: %s\n", config->dry_run ? "true" : "false");
    printf("  log_level: %s\n", config->log_level);
    printf("  control_auth: %s\n",
           (config->control_token && config->control_token[0]) ? "enabled" : "disabled");
    printf("  media_auth: %s\n",
           ((config->media_access_token && config->media_access_token[0]) ||
            (config->auth_active_secret && config->auth_active_secret[0]))
               ? "enabled"
               : "disabled");
    printf("  signed_auth: %s\n",
           (config->auth_active_secret && config->auth_active_secret[0])
               ? "enabled"
               : "disabled");
    printf("  stun_servers: %d\n", config->stun_server_count);
    printf("  turn_servers: %d\n", config->turn_server_count);
    printf("  ice_allow_loopback: %s\n",
           config->ice_allow_loopback ? "true" : "false");
}
