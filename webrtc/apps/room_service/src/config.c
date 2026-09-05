#include "room_service/config.h"
#include "../../config_toml.h"
#include "turbo_media_auth.h"
#include "platform.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ROOM_SERVICE_IRIS_DEAD_RETENTION_SECONDS_DEFAULT = 86400,
    ROOM_SERVICE_IRIS_ARCHIVE_RETENTION_SECONDS_DEFAULT = 2592000,
    ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS_DEFAULT = 60000,
    ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS_MIN = 1000,
    ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE_DEFAULT = 128,
    ROOM_SERVICE_IRIS_RETENTION_SECONDS_MAX = 31536000,
    ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS_MAX = 3600000,
    ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE_MAX = 256
};

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
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_HOST,
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_TOPIC,
    ROOM_CONFIG_STRING_IRIS_PROVIDER_INSTANCE_ID,
    ROOM_CONFIG_STRING_IRIS_IDENTITY,
    ROOM_CONFIG_STRING_IRIS_CERTIFICATE_SHA256,
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_CA_FILE,
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_CERT_FILE,
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_KEY_FILE,
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_KEY_PASSWORD,
    ROOM_CONFIG_STRING_IRIS_FLOWMQ_SERVER_NAME,
    ROOM_CONFIG_STRING_IRIS_EVENT_STORE_CONFIG,
    ROOM_CONFIG_STRING_IRIS_EVENT_STORE_CHANNEL,
    ROOM_CONFIG_STRING_IRIS_COMMAND_LEDGER_CHANNEL,
    ROOM_CONFIG_STRING_FMQ_BIND_HOST,
    ROOM_CONFIG_STRING_FMQ_CA_FILE,
    ROOM_CONFIG_STRING_FMQ_CERT_FILE,
    ROOM_CONFIG_STRING_FMQ_KEY_FILE,
    ROOM_CONFIG_STRING_FMQ_KEY_PASSWORD,
    ROOM_CONFIG_STRING_FMQ_WORKER_BASE,
    ROOM_CONFIG_STRING_COUNT =
        ROOM_CONFIG_STRING_FMQ_WORKER_BASE +
        ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES * 7
} room_service_config_string_t;

#define ROOM_CONFIG_FMQ_WORKER_STRING(worker_index, field_index) \
    (ROOM_CONFIG_STRING_FMQ_WORKER_BASE + (worker_index) * 7 + (field_index))

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

static int room_service_is_loopback(const char *host) {
    return host &&
           (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
            strcmp(host, "localhost") == 0);
}

static int room_service_fingerprint_valid(const char *value) {
    size_t i;
    if (!value || strlen(value) != 71u || memcmp(value, "sha256:", 7u) != 0) {
        return 0;
    }
    for (i = 7u; i < 71u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* 1 when scope is NULL, "*", or a comma-separated list of non-empty tokens
   with no spaces (the adapter performs the final semantic validation). */
static int room_service_scope_valid(const char *scope) {
    const char *p;
    if (!scope) {
        return 1;
    }
    if (scope[0] == '\0' || strcmp(scope, "*") == 0) {
        return scope[0] != '\0' ? 1 : 0;
    }
    p = scope;
    while (*p) {
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' ||
            *p == '\n') {
            return 0;
        }
        while (*p && *p != ',') {
            ++p;
        }
        if (*p == ',') {
            ++p;
            if (*p == '\0' || *p == ',') {
                return 0;
            }
        }
    }
    return 1;
}

static int room_service_fmq_identities_valid(
    const room_service_app_config_t *config) {
    uint64_t now_ms = turbo_realtime_ms();
    int i;
    int j;
    if (config->fmq_worker_identity_count <= 0 ||
        config->fmq_worker_identity_count >
            ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES) {
        return 0;
    }
    for (i = 0; i < config->fmq_worker_identity_count; ++i) {
        const room_service_fmq_worker_identity_t *entry =
            &config->fmq_worker_identities[i];
        if (!entry->worker_id || entry->worker_id[0] == '\0' ||
            strlen(entry->worker_id) > 127u ||
            !room_service_fingerprint_valid(
                entry->active_certificate_sha256) ||
            entry->generation == 0u) {
            return 0;
        }
        if (entry->previous_certificate_sha256) {
            if (!room_service_fingerprint_valid(
                    entry->previous_certificate_sha256) ||
                entry->previous_expires_at_ms <= now_ms ||
                strcmp(entry->previous_certificate_sha256,
                       entry->active_certificate_sha256) == 0) {
                return 0;
            }
        } else if (entry->previous_expires_at_ms != 0u) {
            return 0;
        }
        if ((entry->tenant_id && (entry->tenant_id[0] == '\0' ||
                                  strlen(entry->tenant_id) > 63u)) ||
            !room_service_scope_valid(entry->room_scope) ||
            !room_service_scope_valid(entry->call_scope) ||
            !room_service_scope_valid(entry->content_capabilities)) {
            return 0;
        }
        for (j = 0; j < i; ++j) {
            const room_service_fmq_worker_identity_t *prior =
                &config->fmq_worker_identities[j];
            if (strcmp(prior->worker_id, entry->worker_id) == 0 ||
                strcmp(prior->active_certificate_sha256,
                       entry->active_certificate_sha256) == 0 ||
                (entry->previous_certificate_sha256 &&
                 strcmp(prior->active_certificate_sha256,
                        entry->previous_certificate_sha256) == 0) ||
                (prior->previous_certificate_sha256 &&
                 strcmp(prior->previous_certificate_sha256,
                        entry->active_certificate_sha256) == 0) ||
                (prior->previous_certificate_sha256 &&
                 entry->previous_certificate_sha256 &&
                 strcmp(prior->previous_certificate_sha256,
                        entry->previous_certificate_sha256) == 0)) {
                return 0;
            }
        }
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
    config->iris_flowmq_host =
        room_service_env_value("TURBO_ROOM_SERVICE_IRIS_FLOWMQ_HOST");
    config->iris_flowmq_port = 0;
    config->iris_flowmq_topic = "media-provider-v1";
    config->iris_provider_instance_id = NULL;
    config->iris_identity = NULL;
    config->iris_certificate_sha256 = NULL;
    config->iris_flowmq_ca_file = NULL;
    config->iris_flowmq_cert_file = NULL;
    config->iris_flowmq_key_file = NULL;
    config->iris_flowmq_key_password = NULL;
    config->iris_flowmq_server_name = NULL;
    config->iris_flowmq_use_tls = 1;
    config->iris_flowmq_allow_insecure_loopback = 0;
    config->iris_ack_timeout_ms = 5000;
    config->iris_event_store_config = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_EVENT_STORE_CONFIG");
    config->iris_event_store_channel = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_EVENT_STORE_CHANNEL");
    config->iris_command_ledger_channel = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_CHANNEL");
    config->iris_allow_development_sqlite = 0;
    config->iris_correlation_capacity = 1024;
    config->iris_completion_queue_capacity = 1024;
    config->iris_reconcile_inventory_queue_capacity = 8;
    config->iris_outbox_request_queue_capacity = 1024;
    config->iris_command_ledger_queue_capacity = 1024;
    config->iris_command_terminal_retention_seconds =
        ROOM_SERVICE_IRIS_ARCHIVE_RETENTION_SECONDS_DEFAULT;
    config->iris_command_retention_batch_size =
        ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE_DEFAULT;
    config->iris_dead_retention_seconds =
        ROOM_SERVICE_IRIS_DEAD_RETENTION_SECONDS_DEFAULT;
    config->iris_archive_retention_seconds =
        ROOM_SERVICE_IRIS_ARCHIVE_RETENTION_SECONDS_DEFAULT;
    config->iris_retention_sweep_interval_ms =
        ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS_DEFAULT;
    config->iris_retention_sweep_batch_size =
        ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE_DEFAULT;
    config->iris_retry_max_attempts = 8;
    config->iris_retry_backoff_ms = 250;
    config->iris_drain_timeout_ms = 30000;
    config->fmq_bind_host = "127.0.0.1";
    config->fmq_bind_port = 0;
    config->fmq_worker_heartbeat_ms = 5000;
    config->fmq_worker_lease_ms = 15000;
    config->fmq_dispatch_deadline_ms = 5000;
    config->fmq_dialog_capacity = 256;
    config->fmq_use_tls = 0;
    config->fmq_allow_insecure_loopback = 0;
    config->fmq_ca_file = NULL;
    config->fmq_cert_file = NULL;
    config->fmq_key_file = NULL;
    config->fmq_key_password = NULL;
    memset(config->fmq_worker_identities, 0,
           sizeof(config->fmq_worker_identities));
    config->fmq_worker_identity_count = 0;
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
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_HOST, iris_flowmq_host);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_TOPIC, iris_flowmq_topic);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_PROVIDER_INSTANCE_ID,
                      iris_provider_instance_id);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_IDENTITY, iris_identity);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_CERTIFICATE_SHA256,
                      iris_certificate_sha256);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_CA_FILE,
                      iris_flowmq_ca_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_CERT_FILE,
                      iris_flowmq_cert_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_KEY_FILE,
                      iris_flowmq_key_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_KEY_PASSWORD,
                      iris_flowmq_key_password);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_FLOWMQ_SERVER_NAME,
                      iris_flowmq_server_name);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_EVENT_STORE_CONFIG,
                      iris_event_store_config);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_EVENT_STORE_CHANNEL,
                      iris_event_store_channel);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_IRIS_COMMAND_LEDGER_CHANNEL,
                      iris_command_ledger_channel);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_FMQ_BIND_HOST, fmq_bind_host);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_FMQ_CA_FILE, fmq_ca_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_FMQ_CERT_FILE, fmq_cert_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_FMQ_KEY_FILE, fmq_key_file);
    ROOM_CONFIG_CLONE(ROOM_CONFIG_STRING_FMQ_KEY_PASSWORD, fmq_key_password);

    for (int i = 0; i < ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES; ++i) {
        if (rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 0),
                source->fmq_worker_identities[i].worker_id,
                &candidate->fmq_worker_identities[i].worker_id) != 0 ||
            rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 1),
                source->fmq_worker_identities[i].active_certificate_sha256,
                &candidate->fmq_worker_identities[i]
                     .active_certificate_sha256) != 0 ||
            rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 2),
                source->fmq_worker_identities[i].previous_certificate_sha256,
                &candidate->fmq_worker_identities[i]
                     .previous_certificate_sha256) != 0 ||
            rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 3),
                source->fmq_worker_identities[i].tenant_id,
                &candidate->fmq_worker_identities[i].tenant_id) != 0 ||
            rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 4),
                source->fmq_worker_identities[i].room_scope,
                &candidate->fmq_worker_identities[i].room_scope) != 0 ||
            rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 5),
                source->fmq_worker_identities[i].call_scope,
                &candidate->fmq_worker_identities[i].call_scope) != 0 ||
            rtc_app_config_storage_copy(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 6),
                source->fmq_worker_identities[i].content_capabilities,
                &candidate->fmq_worker_identities[i]
                     .content_capabilities) != 0) {
            return -1;
        }
    }

#undef ROOM_CONFIG_CLONE
    return 0;
}

static int room_service_config_apply_server(
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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
    const toml_table_t *table,
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

static int room_service_config_apply_fmq_workers(
    const toml_table_t *table, room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    toml_array_t *workers;
    int count;
    int i;
    if (!rtc_app_toml_table_has_key(table, "workers")) {
        return 0;
    }
    workers = toml_table_array(table, "workers");
    if (!workers) {
        TLOG_ERROR("TOML key [fmq].workers must be an array of tables");
        return -1;
    }
    count = toml_array_len(workers);
    if (count < 0 || count > ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES) {
        TLOG_ERRORF("TOML key [fmq].workers exceeds the maximum of {} entries",
                   ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES);
        return -1;
    }
    for (i = 0; i < ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES; ++i) {
        room_service_fmq_worker_identity_t *identity =
            &config->fmq_worker_identities[i];
        if (rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 0), NULL,
                &identity->worker_id) != 0 ||
            rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 1), NULL,
                &identity->active_certificate_sha256) != 0 ||
            rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 2), NULL,
                &identity->previous_certificate_sha256) != 0 ||
            rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 3), NULL,
                &identity->tenant_id) != 0 ||
            rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 4), NULL,
                &identity->room_scope) != 0 ||
            rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 5), NULL,
                &identity->call_scope) != 0 ||
            rtc_app_config_storage_replace(
                storage, ROOM_CONFIG_FMQ_WORKER_STRING(i, 6), NULL,
                &identity->content_capabilities) != 0) {
            return -1;
        }
        identity->previous_expires_at_ms = 0u;
        identity->generation = 0u;
    }
    for (i = 0; i < count; ++i) {
        static const char *const allowed[] = {
            "worker_id", "active_certificate_sha256",
            "previous_certificate_sha256", "previous_expires_at_ms",
            "generation", "tenant_id", "room_scope", "call_scope",
            "content_capabilities"};
        toml_table_t *worker = toml_array_table(workers, i);
        room_service_fmq_worker_identity_t *identity =
            &config->fmq_worker_identities[i];
        toml_value_t value;
        if (!worker ||
            rtc_app_toml_table_keys_valid(
                worker, "fmq.workers", allowed,
                sizeof(allowed) / sizeof(allowed[0])) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "worker_id", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 0),
                &identity->worker_id) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "active_certificate_sha256", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 1),
                &identity->active_certificate_sha256) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "previous_certificate_sha256", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 2),
                &identity->previous_certificate_sha256) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "tenant_id", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 3),
                &identity->tenant_id) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "room_scope", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 4),
                &identity->room_scope) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "call_scope", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 5),
                &identity->call_scope) != 0 ||
            rtc_app_toml_apply_string(
                worker, "fmq.workers", "content_capabilities", storage,
                ROOM_CONFIG_FMQ_WORKER_STRING(i, 6),
                &identity->content_capabilities) != 0) {
            return -1;
        }
        if (rtc_app_toml_table_has_key(worker, "previous_expires_at_ms")) {
            value = toml_table_int(worker, "previous_expires_at_ms");
            if (!value.ok || value.u.i <= 0) return -1;
            identity->previous_expires_at_ms = (uint64_t)value.u.i;
        }
        if (!rtc_app_toml_table_has_key(worker, "generation")) return -1;
        value = toml_table_int(worker, "generation");
        if (!value.ok || value.u.i <= 0) return -1;
        identity->generation = (uint64_t)value.u.i;
    }
    config->fmq_worker_identity_count = count;
    return 0;
}

static int room_service_config_apply_fmq(
    const toml_table_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "bind_host", "bind_port",
        "worker_heartbeat_ms", "worker_lease_ms", "dispatch_deadline_ms",
        "dialog_capacity",
        "use_tls", "allow_insecure_loopback", "ca_file", "cert_file",
        "key_file", "key_password",
        "workers"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "fmq", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(table, "fmq", "bind_host", storage,
                                  ROOM_CONFIG_STRING_FMQ_BIND_HOST,
                                  &config->fmq_bind_host) != 0 ||
        rtc_app_toml_apply_string(table, "fmq", "ca_file", storage,
                                  ROOM_CONFIG_STRING_FMQ_CA_FILE,
                                  &config->fmq_ca_file) != 0 ||
        rtc_app_toml_apply_string(table, "fmq", "cert_file", storage,
                                  ROOM_CONFIG_STRING_FMQ_CERT_FILE,
                                  &config->fmq_cert_file) != 0 ||
        rtc_app_toml_apply_string(table, "fmq", "key_file", storage,
                                  ROOM_CONFIG_STRING_FMQ_KEY_FILE,
                                  &config->fmq_key_file) != 0 ||
        rtc_app_toml_apply_string(table, "fmq", "key_password", storage,
                                  ROOM_CONFIG_STRING_FMQ_KEY_PASSWORD,
                                  &config->fmq_key_password) != 0 ||
        rtc_app_toml_apply_int(table, "fmq", "bind_port",
                               &config->fmq_bind_port) != 0 ||
        rtc_app_toml_apply_int(table, "fmq", "worker_heartbeat_ms",
                               &config->fmq_worker_heartbeat_ms) != 0 ||
        rtc_app_toml_apply_int(table, "fmq", "worker_lease_ms",
                               &config->fmq_worker_lease_ms) != 0 ||
        rtc_app_toml_apply_int(table, "fmq", "dispatch_deadline_ms",
                               &config->fmq_dispatch_deadline_ms) != 0 ||
        rtc_app_toml_apply_int(table, "fmq", "dialog_capacity",
                               &config->fmq_dialog_capacity) != 0 ||
        rtc_app_toml_apply_bool(table, "fmq", "use_tls",
                                &config->fmq_use_tls) != 0 ||
        rtc_app_toml_apply_bool(table, "fmq", "allow_insecure_loopback",
                                &config->fmq_allow_insecure_loopback) != 0 ||
        room_service_config_apply_fmq_workers(table, config, storage) != 0) {
        return -1;
    }
    return 0;
}

static int room_service_config_apply_iris_provider(
    const toml_table_t *table,
    room_service_app_config_t *config,
    rtc_app_config_storage_t *storage) {
    static const char *const allowed[] = {
        "flowmq_host", "flowmq_port", "flowmq_topic",
        "provider_instance_id", "iris_identity",
        "iris_certificate_sha256", "flowmq_ca_file", "flowmq_cert_file",
        "flowmq_key_file", "flowmq_key_password", "flowmq_server_name",
        "flowmq_use_tls", "flowmq_allow_insecure_loopback",
        "correlation_capacity",
        "completion_queue_capacity", "reconcile_inventory_queue_capacity",
        "retry_max_attempts", "retry_backoff_ms",
        "ack_timeout_ms", "drain_timeout_ms", "event_store_config",
        "event_store_channel", "outbox_request_queue_capacity",
        "command_ledger_channel", "command_ledger_queue_capacity",
        "command_terminal_retention_seconds", "command_retention_batch_size",
        "allow_development_sqlite", "dead_retention_seconds",
        "archive_retention_seconds", "retention_sweep_interval_ms",
        "retention_sweep_batch_size"
    };

    if (!table) {
        return 0;
    }
    if (rtc_app_toml_table_keys_valid(
            table, "iris_provider", allowed,
            sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_host", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_HOST,
            &config->iris_flowmq_host) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_topic", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_TOPIC,
            &config->iris_flowmq_topic) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "provider_instance_id", storage,
            ROOM_CONFIG_STRING_IRIS_PROVIDER_INSTANCE_ID,
            &config->iris_provider_instance_id) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "iris_identity", storage,
            ROOM_CONFIG_STRING_IRIS_IDENTITY, &config->iris_identity) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "iris_certificate_sha256", storage,
            ROOM_CONFIG_STRING_IRIS_CERTIFICATE_SHA256,
            &config->iris_certificate_sha256) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_ca_file", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_CA_FILE,
            &config->iris_flowmq_ca_file) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_cert_file", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_CERT_FILE,
            &config->iris_flowmq_cert_file) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_key_file", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_KEY_FILE,
            &config->iris_flowmq_key_file) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_key_password", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_KEY_PASSWORD,
            &config->iris_flowmq_key_password) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "flowmq_server_name", storage,
            ROOM_CONFIG_STRING_IRIS_FLOWMQ_SERVER_NAME,
            &config->iris_flowmq_server_name) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "flowmq_port",
                               &config->iris_flowmq_port) != 0 ||
        rtc_app_toml_apply_bool(table, "iris_provider", "flowmq_use_tls",
                                &config->iris_flowmq_use_tls) != 0 ||
        rtc_app_toml_apply_bool(
            table, "iris_provider", "flowmq_allow_insecure_loopback",
            &config->iris_flowmq_allow_insecure_loopback) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "event_store_config", storage,
            ROOM_CONFIG_STRING_IRIS_EVENT_STORE_CONFIG,
            &config->iris_event_store_config) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "event_store_channel", storage,
            ROOM_CONFIG_STRING_IRIS_EVENT_STORE_CHANNEL,
            &config->iris_event_store_channel) != 0 ||
        rtc_app_toml_apply_string(
            table, "iris_provider", "command_ledger_channel", storage,
            ROOM_CONFIG_STRING_IRIS_COMMAND_LEDGER_CHANNEL,
            &config->iris_command_ledger_channel) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "correlation_capacity",
                               &config->iris_correlation_capacity) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "completion_queue_capacity",
                               &config->iris_completion_queue_capacity) != 0 ||
        rtc_app_toml_apply_int(
            table, "iris_provider", "reconcile_inventory_queue_capacity",
            &config->iris_reconcile_inventory_queue_capacity) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "outbox_request_queue_capacity",
                               &config->iris_outbox_request_queue_capacity) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "command_ledger_queue_capacity",
                               &config->iris_command_ledger_queue_capacity) != 0 ||
        rtc_app_toml_apply_int(
            table, "iris_provider", "command_terminal_retention_seconds",
            &config->iris_command_terminal_retention_seconds) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "command_retention_batch_size",
                               &config->iris_command_retention_batch_size) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "dead_retention_seconds",
                               &config->iris_dead_retention_seconds) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "archive_retention_seconds",
                               &config->iris_archive_retention_seconds) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "retention_sweep_interval_ms",
                               &config->iris_retention_sweep_interval_ms) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "retention_sweep_batch_size",
                               &config->iris_retention_sweep_batch_size) != 0 ||
        rtc_app_toml_apply_bool(table, "iris_provider",
                                "allow_development_sqlite",
                                &config->iris_allow_development_sqlite) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "retry_max_attempts",
                               &config->iris_retry_max_attempts) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "retry_backoff_ms",
                               &config->iris_retry_backoff_ms) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "ack_timeout_ms",
                               &config->iris_ack_timeout_ms) != 0 ||
        rtc_app_toml_apply_int(table, "iris_provider", "drain_timeout_ms",
                               &config->iris_drain_timeout_ms) != 0) {
        return -1;
    }
    return 0;
}

int room_service_app_config_load(room_service_app_config_t *config,
                                 const char *filename) {
    static const char *const root_keys[] = {
        "server", "control", "auth", "sfu", "sfu_auth", "capacity",
        "rooms", "runtime", "logging", "iris_provider", "fmq"
    };
    rtc_app_toml_document_t document;
    rtc_app_config_storage_t *storage = NULL;
    rtc_app_config_storage_t *old_storage;
    toml_table_t *server = NULL;
    toml_table_t *control = NULL;
    toml_table_t *auth = NULL;
    toml_table_t *sfu = NULL;
    toml_table_t *sfu_auth = NULL;
    toml_table_t *capacity = NULL;
    toml_table_t *rooms = NULL;
    toml_table_t *runtime = NULL;
    toml_table_t *logging = NULL;
    toml_table_t *iris_provider = NULL;
    toml_table_t *fmq = NULL;
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
        rtc_app_toml_get_optional_table(document.root, "iris_provider", &iris_provider) != 0 ||
        rtc_app_toml_get_optional_table(document.root, "fmq", &fmq) != 0 ||
        room_service_config_apply_server(server, &candidate, storage) != 0 ||
        room_service_config_apply_control(control, &candidate, storage) != 0 ||
        room_service_config_apply_auth(auth, &candidate, storage) != 0 ||
        room_service_config_apply_sfu(sfu, &candidate, storage) != 0 ||
        room_service_config_apply_sfu_auth(sfu_auth, &candidate, storage) != 0 ||
        room_service_config_apply_capacity(capacity, &candidate) != 0 ||
        room_service_config_apply_rooms(rooms, &candidate) != 0 ||
        room_service_config_apply_runtime(runtime, &candidate) != 0 ||
        room_service_config_apply_logging(logging, &candidate, storage) != 0 ||
        room_service_config_apply_iris_provider(iris_provider, &candidate, storage) != 0 ||
        room_service_config_apply_fmq(fmq, &candidate, storage) != 0 ||
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
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_FLOWMQ_HOST");
    if (value) config->iris_flowmq_host = value;
    config->iris_flowmq_port = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_FLOWMQ_PORT", config->iris_flowmq_port);
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_FLOWMQ_TOPIC");
    if (value) config->iris_flowmq_topic = value;
    value = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_PROVIDER_INSTANCE_ID");
    if (value) config->iris_provider_instance_id = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_IDENTITY");
    if (value) config->iris_identity = value;
    value = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_CERTIFICATE_SHA256");
    if (value) config->iris_certificate_sha256 = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_FLOWMQ_CA_FILE");
    if (value) config->iris_flowmq_ca_file = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_FLOWMQ_CERT_FILE");
    if (value) config->iris_flowmq_cert_file = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_FLOWMQ_KEY_FILE");
    if (value) config->iris_flowmq_key_file = value;
    value = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_FLOWMQ_KEY_PASSWORD");
    if (value) config->iris_flowmq_key_password = value;
    value = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_FLOWMQ_SERVER_NAME");
    if (value) config->iris_flowmq_server_name = value;
    config->iris_flowmq_use_tls = room_service_env_bool(
        "TURBO_ROOM_SERVICE_IRIS_FLOWMQ_USE_TLS",
        config->iris_flowmq_use_tls);
    config->iris_flowmq_allow_insecure_loopback = room_service_env_bool(
        "TURBO_ROOM_SERVICE_IRIS_FLOWMQ_ALLOW_INSECURE_LOOPBACK",
        config->iris_flowmq_allow_insecure_loopback);
    config->iris_ack_timeout_ms = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_ACK_TIMEOUT_MS",
        config->iris_ack_timeout_ms);
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_EVENT_STORE_CONFIG");
    if (value) config->iris_event_store_config = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_IRIS_EVENT_STORE_CHANNEL");
    if (value) config->iris_event_store_channel = value;
    value = room_service_env_value(
        "TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_CHANNEL");
    if (value) config->iris_command_ledger_channel = value;
    config->iris_allow_development_sqlite = room_service_env_bool(
        "TURBO_ROOM_SERVICE_IRIS_ALLOW_DEVELOPMENT_SQLITE",
        config->iris_allow_development_sqlite);
    config->iris_dead_retention_seconds = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_DEAD_RETENTION_SECONDS",
        config->iris_dead_retention_seconds);
    config->iris_archive_retention_seconds = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_ARCHIVE_RETENTION_SECONDS",
        config->iris_archive_retention_seconds);
    config->iris_retention_sweep_interval_ms = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS",
        config->iris_retention_sweep_interval_ms);
    config->iris_retention_sweep_batch_size = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE",
        config->iris_retention_sweep_batch_size);
    config->iris_command_ledger_queue_capacity = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_QUEUE_CAPACITY",
        config->iris_command_ledger_queue_capacity);
    config->iris_command_terminal_retention_seconds = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_COMMAND_TERMINAL_RETENTION_SECONDS",
        config->iris_command_terminal_retention_seconds);
    config->iris_command_retention_batch_size = room_service_env_int(
        "TURBO_ROOM_SERVICE_IRIS_COMMAND_RETENTION_BATCH_SIZE",
        config->iris_command_retention_batch_size);
    value = room_service_env_value("TURBO_ROOM_SERVICE_FMQ_BIND_HOST");
    if (value) {
        config->fmq_bind_host = value;
    }
    config->fmq_bind_port = room_service_env_int(
        "TURBO_ROOM_SERVICE_FMQ_BIND_PORT", config->fmq_bind_port);
    config->fmq_worker_heartbeat_ms = room_service_env_int(
        "TURBO_ROOM_SERVICE_FMQ_WORKER_HEARTBEAT_MS",
        config->fmq_worker_heartbeat_ms);
    config->fmq_worker_lease_ms = room_service_env_int(
        "TURBO_ROOM_SERVICE_FMQ_WORKER_LEASE_MS",
        config->fmq_worker_lease_ms);
    config->fmq_dispatch_deadline_ms = room_service_env_int(
        "TURBO_ROOM_SERVICE_FMQ_DISPATCH_DEADLINE_MS",
        config->fmq_dispatch_deadline_ms);
    config->fmq_dialog_capacity = room_service_env_int(
        "TURBO_ROOM_SERVICE_FMQ_DIALOG_CAPACITY",
        config->fmq_dialog_capacity);
    config->fmq_use_tls = room_service_env_bool(
        "TURBO_ROOM_SERVICE_FMQ_USE_TLS", config->fmq_use_tls);
    config->fmq_allow_insecure_loopback = room_service_env_bool(
        "TURBO_ROOM_SERVICE_FMQ_ALLOW_INSECURE_LOOPBACK",
        config->fmq_allow_insecure_loopback);
    value = room_service_env_value("TURBO_ROOM_SERVICE_FMQ_CA_FILE");
    if (value) config->fmq_ca_file = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_FMQ_CERT_FILE");
    if (value) config->fmq_cert_file = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_FMQ_KEY_FILE");
    if (value) config->fmq_key_file = value;
    value = room_service_env_value("TURBO_ROOM_SERVICE_FMQ_KEY_PASSWORD");
    if (value) config->fmq_key_password = value;
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
        config->max_rooms <= 0 ||
        config->fmq_bind_port < 0 || config->fmq_bind_port > 65535 ||
        config->fmq_worker_heartbeat_ms <= 0 ||
        config->fmq_worker_lease_ms < config->fmq_worker_heartbeat_ms ||
        config->fmq_worker_heartbeat_ms > INT_MAX / 3 ||
        config->fmq_worker_lease_ms <
            config->fmq_worker_heartbeat_ms * 3 ||
        config->fmq_dispatch_deadline_ms <= 0 ||
        config->fmq_dispatch_deadline_ms >= config->fmq_worker_lease_ms ||
        config->fmq_dialog_capacity < 1 ||
        config->fmq_dialog_capacity > 65536 ||
        (config->fmq_bind_port > 0 &&
         (!config->fmq_bind_host || config->fmq_bind_host[0] == 0)) ||
        (config->fmq_use_tls != 0 && config->fmq_use_tls != 1) ||
        (config->fmq_allow_insecure_loopback != 0 &&
         config->fmq_allow_insecure_loopback != 1)) {
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
        int has_flowmq = config->iris_flowmq_host &&
                         config->iris_flowmq_host[0] != '\0';
        int has_store_config = config->iris_event_store_config &&
                               config->iris_event_store_config[0] != '\0';
        int has_store_channel = config->iris_event_store_channel &&
                                config->iris_event_store_channel[0] != '\0';
        int has_ledger_channel = config->iris_command_ledger_channel &&
                                 config->iris_command_ledger_channel[0] != '\0';
        if (has_flowmq != has_store_config ||
            has_flowmq != has_store_channel ||
            has_flowmq != has_ledger_channel ||
            (has_flowmq && strcmp(config->iris_event_store_channel,
                               config->iris_command_ledger_channel) == 0) ||
            (has_flowmq &&
             (config->iris_flowmq_port < 1 ||
              config->iris_flowmq_port > 65535 ||
              !config->iris_flowmq_topic ||
              config->iris_flowmq_topic[0] == '\0' ||
              !config->iris_provider_instance_id ||
              config->iris_provider_instance_id[0] == '\0' ||
              !config->iris_identity || config->iris_identity[0] == '\0')) ||
            (config->iris_flowmq_use_tls != 0 &&
             config->iris_flowmq_use_tls != 1) ||
            (config->iris_flowmq_allow_insecure_loopback != 0 &&
             config->iris_flowmq_allow_insecure_loopback != 1) ||
            (has_flowmq && config->iris_flowmq_use_tls &&
             (!room_service_fingerprint_valid(
                  config->iris_certificate_sha256) ||
              !config->iris_flowmq_ca_file ||
              config->iris_flowmq_ca_file[0] == '\0' ||
              !config->iris_flowmq_cert_file ||
              config->iris_flowmq_cert_file[0] == '\0' ||
              !config->iris_flowmq_key_file ||
              config->iris_flowmq_key_file[0] == '\0' ||
              !config->iris_flowmq_server_name ||
              config->iris_flowmq_server_name[0] == '\0')) ||
            (has_flowmq && !config->iris_flowmq_use_tls &&
             (!config->iris_flowmq_allow_insecure_loopback ||
              !room_service_is_loopback(config->iris_flowmq_host) ||
              config->iris_certificate_sha256 ||
              config->iris_flowmq_ca_file ||
              config->iris_flowmq_cert_file ||
              config->iris_flowmq_key_file ||
              config->iris_flowmq_key_password ||
              config->iris_flowmq_server_name)) ||
            config->iris_correlation_capacity < 1 ||
            config->iris_correlation_capacity > 65536 ||
            config->iris_completion_queue_capacity < 1 ||
            config->iris_completion_queue_capacity > 65536 ||
            config->iris_reconcile_inventory_queue_capacity < 1 ||
            config->iris_reconcile_inventory_queue_capacity > 64 ||
            config->iris_outbox_request_queue_capacity < 1 ||
            config->iris_outbox_request_queue_capacity > 65536 ||
            config->iris_command_ledger_queue_capacity < 1 ||
            config->iris_command_ledger_queue_capacity > 65536 ||
            config->iris_command_terminal_retention_seconds < 1 ||
            config->iris_command_terminal_retention_seconds >
                ROOM_SERVICE_IRIS_RETENTION_SECONDS_MAX ||
            config->iris_command_retention_batch_size < 1 ||
            config->iris_command_retention_batch_size >
                ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE_MAX ||
            config->iris_dead_retention_seconds < 1 ||
            config->iris_dead_retention_seconds >
                ROOM_SERVICE_IRIS_RETENTION_SECONDS_MAX ||
            config->iris_archive_retention_seconds < 1 ||
            config->iris_archive_retention_seconds >
                ROOM_SERVICE_IRIS_RETENTION_SECONDS_MAX ||
            config->iris_retention_sweep_interval_ms <
                ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS_MIN ||
            config->iris_retention_sweep_interval_ms >
                ROOM_SERVICE_IRIS_RETENTION_SWEEP_INTERVAL_MS_MAX ||
            config->iris_retention_sweep_batch_size < 1 ||
            config->iris_retention_sweep_batch_size >
                ROOM_SERVICE_IRIS_RETENTION_SWEEP_BATCH_SIZE_MAX ||
            (config->iris_allow_development_sqlite != 0 &&
             config->iris_allow_development_sqlite != 1) ||
            config->iris_retry_max_attempts < 1 ||
            config->iris_retry_max_attempts > 100 ||
            config->iris_retry_backoff_ms < 1 ||
            config->iris_retry_backoff_ms > 60000 ||
            config->iris_ack_timeout_ms < 1 ||
            config->iris_ack_timeout_ms > 300000 ||
            config->iris_drain_timeout_ms < 1 ||
            config->iris_drain_timeout_ms > 600000 ||
            config->iris_drain_timeout_ms < config->iris_ack_timeout_ms ||
            (has_flowmq && config->fmq_bind_port == 0)) {
            return -1;
        }
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
    if (config->fmq_bind_port > 0) {
        if (config->fmq_use_tls) {
            if (config->fmq_allow_insecure_loopback || !config->fmq_ca_file ||
                !config->fmq_ca_file[0] || !config->fmq_cert_file ||
                !config->fmq_cert_file[0] || !config->fmq_key_file ||
                !config->fmq_key_file[0] ||
                !room_service_fmq_identities_valid(config)) {
                return -1;
            }
        } else if (!config->fmq_allow_insecure_loopback ||
                   !room_service_is_loopback(config->fmq_bind_host)) {
            return -1;
        }
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
           config->sfu_control_url ? "configured" : "disabled");
    printf("  sfu_nodes: %s\n", config->sfu_nodes ? "configured" : "none");
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
    printf("  iris_provider: %s\n",
           config->iris_flowmq_host ? "FlowMQ" : "disabled");
    if (config->iris_flowmq_host) {
        printf("  iris_flowmq: %s:%d topic=%s transport=%s\n",
               config->iris_flowmq_host, config->iris_flowmq_port,
               config->iris_flowmq_topic,
               config->iris_flowmq_use_tls ? "mTLS" : "trusted-loopback");
        printf("  iris_sqlite_development_opt_in: %s\n",
               config->iris_allow_development_sqlite ? "true" : "false");
        printf("  iris_provider_limits: correlation=%d queue=%d reconcile_queue=%d outbox_queue=%d retries=%d "
               "ack_timeout=%dms drain_timeout=%dms\n",
               config->iris_correlation_capacity,
                config->iris_completion_queue_capacity,
                config->iris_reconcile_inventory_queue_capacity,
                config->iris_outbox_request_queue_capacity,
               config->iris_retry_max_attempts,
               config->iris_ack_timeout_ms,
               config->iris_drain_timeout_ms);
    }
    printf("  fmq_bridge: %s (router %s:%d)\n",
           config->fmq_bind_port > 0 ? "enabled" : "disabled",
           config->fmq_bind_host ? config->fmq_bind_host : "127.0.0.1",
           config->fmq_bind_port);
    printf("  fmq_worker_timing: heartbeat=%dms lease=%dms dispatch=%dms dialogs=%d\n",
           config->fmq_worker_heartbeat_ms, config->fmq_worker_lease_ms,
           config->fmq_dispatch_deadline_ms, config->fmq_dialog_capacity);
    printf("  fmq_security: %s identities=%d\n",
           config->fmq_use_tls ? "mTLS" :
           (config->fmq_allow_insecure_loopback ? "trusted-loopback" :
                                                  "disabled"),
           config->fmq_worker_identity_count);
}
