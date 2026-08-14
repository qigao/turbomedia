#include "iris_flowstore.h"

#include "turbo_error.h"
#include "turbo_flow_config.h"
#include "turbo_flow_pgsql_storage_backend.h"
#include "turbo_flow_redis_storage_backend.h"
#include "turbo_flow_sqlite_storage_backend.h"
#include "turbo_flow_storage_backend.h"
#include "turbo_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct iris_flowstore_owner_s {
    turbo_flow_resolved_config_t *resolved;
    turbo_flow_storage_backend_registry_t *registry;
    turbo_flow_storage_backend_owner_t *backend_owner;
    turbo_flow_record_store_t *store;
};

static void write_error(char *error, size_t capacity, const char *message) {
    if (error && capacity > 0u) (void)snprintf(error, capacity, "%s", message);
}

iris_flowstore_owner_t *iris_flowstore_owner_create(
    const char *yaml_path, const char *channel_name,
    int allow_development_sqlite, char *error_text, size_t error_capacity) {
    turbo_fs_buf_t yaml = {0};
    turbo_flow_resolved_config_t *resolved = NULL;
    turbo_flow_config_error_t config_error = TURBO_FLOW_CONFIG_ERROR_INIT;
    turbo_flow_resolved_channel_view_t channel =
        TURBO_FLOW_RESOLVED_CHANNEL_VIEW_INIT;
    turbo_flow_storage_backend_registry_t *registry = NULL;
    turbo_flow_storage_backend_owner_t *backend_owner = NULL;
    turbo_flow_storage_backend_open_request_t request =
        TURBO_FLOW_STORAGE_BACKEND_OPEN_REQUEST_INIT;
    turbo_flow_record_store_t *store = NULL;
    iris_flowstore_owner_t *result = NULL;
    const char *backend = NULL;
    int rc;
    if (!yaml_path || !yaml_path[0] || !channel_name || !channel_name[0]) {
        write_error(error_text, error_capacity,
                    "invalid FlowStore configuration");
        return NULL;
    }
    if (turbo_fs_read_file(yaml_path, &yaml) != TURBO_OK) {
        write_error(error_text, error_capacity, "cannot read FlowStore YAML");
        goto cleanup;
    }
    rc = turbo_flow_config_resolve_yaml(yaml.base, yaml.len, &resolved,
                                        &config_error);
    if (rc != TURBO_OK ||
        turbo_flow_resolved_config_channel(resolved, channel_name, &channel) !=
            TURBO_OK ||
        strcmp(channel.kind, "record_store") != 0 ||
        turbo_flow_resolved_channel_get_string(&channel, "backend", &backend) !=
            TURBO_OK) {
        write_error(error_text, error_capacity,
                    config_error.message[0] ? config_error.message
                                            : "invalid FlowStore channel");
        goto cleanup;
    }
    if (strcmp(backend, "sqlite") != 0 && strcmp(backend, "redis") != 0 &&
        strcmp(backend, "postgresql") != 0) {
        write_error(error_text, error_capacity,
                    "unsupported FlowStore backend");
        goto cleanup;
    }
    if (strcmp(backend, "sqlite") == 0 && !allow_development_sqlite) {
        write_error(error_text, error_capacity,
                    "sqlite FlowStore requires explicit development opt-in");
        goto cleanup;
    }
    if (turbo_flow_storage_backend_registry_create(3u, &registry) != TURBO_OK ||
        turbo_flow_storage_backend_registry_register(
            registry, turbo_flow_sqlite_storage_backend_api()) != TURBO_OK ||
        turbo_flow_storage_backend_registry_register(
            registry, turbo_flow_redis_storage_backend_api()) != TURBO_OK ||
        turbo_flow_storage_backend_registry_register(
            registry, turbo_flow_pgsql_storage_backend_api()) != TURBO_OK) {
        write_error(error_text, error_capacity,
                    "cannot register FlowStore backends");
        goto cleanup;
    }
    request.model = TURBO_FLOW_STORAGE_MODEL_RECORD;
    request.resolved = resolved;
    request.channel_name = channel_name;
    rc = turbo_flow_storage_backend_owner_create_registered(
        registry, backend, &request, &backend_owner, &config_error);
    if (rc != TURBO_OK ||
        turbo_flow_storage_backend_owner_service(
            backend_owner, TURBO_FLOW_STORAGE_MODEL_RECORD,
            (void **)&store) != TURBO_OK) {
        write_error(error_text, error_capacity,
                    config_error.message[0] ? config_error.message
                                            : "cannot open FlowStore backend");
        goto cleanup;
    }
    result = (iris_flowstore_owner_t *)calloc(1u, sizeof(*result));
    if (!result) {
        write_error(error_text, error_capacity,
                    "cannot allocate FlowStore owner");
        goto cleanup;
    }
    result->resolved = resolved;
    result->registry = registry;
    result->backend_owner = backend_owner;
    result->store = store;
    resolved = NULL;
    registry = NULL;
    backend_owner = NULL;
cleanup:
    turbo_flow_storage_backend_owner_destroy(backend_owner);
    (void)turbo_flow_storage_backend_registry_destroy(registry);
    turbo_flow_resolved_config_destroy(resolved);
    turbo_fs_buf_free(&yaml);
    return result;
}

turbo_flow_record_store_t *iris_flowstore_owner_store(
    iris_flowstore_owner_t *owner) {
    return owner ? owner->store : NULL;
}

void iris_flowstore_owner_destroy(iris_flowstore_owner_t *owner) {
    if (!owner) return;
    turbo_flow_storage_backend_owner_destroy(owner->backend_owner);
    (void)turbo_flow_storage_backend_registry_destroy(owner->registry);
    turbo_flow_resolved_config_destroy(owner->resolved);
    free(owner);
}
