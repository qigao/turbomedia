#include <orm_driver_interface.h>
#include <salts/plugin.h>

#include <stdlib.h>
#include <string.h>

#define FIXTURE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define FIXTURE_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}
#define FIXTURE_EXPORT_ID "driver"

#ifndef TURBOMEDIA_ORM_TEST_DRIVER_ID
#define TURBOMEDIA_ORM_TEST_DRIVER_ID "postgresql"
#endif

typedef struct fixture_module_s {
    uint32_t started;
    uint32_t live_connections;
} fixture_module_t;

typedef struct fixture_connection_s {
    fixture_module_t *module;
    uint32_t live;
} fixture_connection_t;

static fixture_module_t g_module;
static fixture_connection_t g_connections[4];
static const char g_driver_id[] = TURBOMEDIA_ORM_TEST_DRIVER_ID;

static void fixture_error(orm_error_t *error, orm_status_t status) {
    if (!error) return;
    memset(error, 0, sizeof(*error));
    error->struct_size = (uint32_t)sizeof(*error);
    error->status = status;
}

static int fixture_env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static salts_plugin_status SALTS_PLUGIN_CALL fixture_start(void *self) {
    fixture_module_t *module = (fixture_module_t *)self;
    if (module != &g_module) return SALTS_PLUGIN_INVALID_ARGUMENT;
    module->started = 1u;
    return SALTS_PLUGIN_OK;
}

static salts_plugin_status SALTS_PLUGIN_CALL fixture_request_stop(void *self) {
    fixture_module_t *module = (fixture_module_t *)self;
    if (module != &g_module) return SALTS_PLUGIN_INVALID_ARGUMENT;
    if (module->live_connections != 0u)
        return SALTS_PLUGIN_BUSY;
    module->started = 0u;
    return SALTS_PLUGIN_OK;
}

static bool SALTS_PLUGIN_CALL fixture_is_quiescent(const void *self) {
    const fixture_module_t *module = (const fixture_module_t *)self;
    return module == &g_module && module->started == 0u &&
           module->live_connections == 0u &&
           !fixture_env_enabled("TURBOMEDIA_ORM_TEST_BUSY_CLOSE");
}

static void SALTS_PLUGIN_CALL fixture_destroy(void *self) {
    if (self == &g_module) memset(&g_module, 0, sizeof(g_module));
}

static void ORM_DRIVER_CALL fixture_destroy_connection(void *context) {
    fixture_connection_t *connection = (fixture_connection_t *)context;
    if (!connection || !connection->live || !connection->module) return;
    if (connection->module->live_connections > 0u)
        --connection->module->live_connections;
    memset(connection, 0, sizeof(*connection));
}

static orm_status_t ORM_DRIVER_CALL fixture_execute_command(
    void *context, const orm_driver_plan_view_v1 *plan,
    const orm_driver_limits_v1 *limits, uint64_t *affected_rows,
    orm_error_t *error) {
    fixture_connection_t *connection = (fixture_connection_t *)context;
    if (affected_rows) *affected_rows = 0u;
    if (!connection || !connection->live || !plan || !limits ||
        !affected_rows) {
        fixture_error(error, ORM_STATUS_INVALID_ARGUMENT);
        return ORM_STATUS_INVALID_ARGUMENT;
    }
    *affected_rows = 0u;
    fixture_error(error, ORM_STATUS_OK);
    return ORM_STATUS_OK;
}

static const orm_driver_connection_ops_v1 g_connection_ops = {
    FIXTURE_HEADER(orm_driver_connection_ops_v1),
    fixture_destroy_connection,
    NULL,
    fixture_execute_command,
    NULL};

static orm_status_t ORM_DRIVER_CALL fixture_create_connection(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error) {
    fixture_module_t *module = (fixture_module_t *)self;
    size_t i;
    if (out) memset(out, 0, sizeof(*out));
    if (module != &g_module || !module->started || !config || !limits || !out) {
        fixture_error(error, ORM_STATUS_INVALID_ARGUMENT);
        return ORM_STATUS_INVALID_ARGUMENT;
    }
    if (fixture_env_enabled("TURBOMEDIA_ORM_TEST_FAIL_CONNECT")) {
        fixture_error(error, ORM_STATUS_INTERNAL_ERROR);
        return ORM_STATUS_INTERNAL_ERROR;
    }
    if (!config->driver.data ||
        config->driver.len != sizeof("postgresql") - 1u ||
        memcmp(config->driver.data, "postgresql", sizeof("postgresql") - 1u) != 0) {
        fixture_error(error, ORM_STATUS_DRIVER_NOT_REGISTERED);
        return ORM_STATUS_DRIVER_NOT_REGISTERED;
    }
    for (i = 0u; i < sizeof(g_connections) / sizeof(g_connections[0]); ++i) {
        if (!g_connections[i].live) {
            g_connections[i].module = module;
            g_connections[i].live = 1u;
            ++module->live_connections;
            out->header =
                (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_connection_v1);
            out->context = &g_connections[i];
            out->ops = (orm_driver_table_v1)FIXTURE_TABLE(&g_connection_ops);
            fixture_error(error, ORM_STATUS_OK);
            return ORM_STATUS_OK;
        }
    }
    fixture_error(error, ORM_STATUS_LIMIT_EXCEEDED);
    return ORM_STATUS_LIMIT_EXCEEDED;
}

static const orm_driver_storage_capabilities_v1 g_storage_capabilities =
    ORM_DRIVER_STORAGE_CAPABILITIES_NONE_INIT;

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
fixture_storage_capabilities(void *self) {
    return self == &g_module ? &g_storage_capabilities : NULL;
}

static uint64_t ORM_DRIVER_CALL fixture_execution_models(void *self) {
    return self == &g_module ? ORM_DRIVER_EXEC_CALLER_BLOCKING : UINT64_C(0);
}

#define FIXTURE_CAPABILITIES ORM_DRIVER_CAP_RAW_SQL

static const TurboDb_Driver_vtable g_driver_vtable = {
    .implementation = g_driver_id,
    .capabilities = FIXTURE_CAPABILITIES,
    .create = fixture_create_connection,
    .execution_models = fixture_execution_models,
    .storage_capabilities = fixture_storage_capabilities};

static TurboDb_Driver g_driver = {&g_module, &g_driver_vtable};

static const salts_plugin_export g_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = FIXTURE_CAPABILITIES,
    .export_id = FIXTURE_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &g_driver}}};

static const salts_plugin_manifest g_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = g_driver_id,
    .version = {1u, 0u, 0u},
    .exports = g_exports,
    .export_count = 1u,
    .self = &g_module,
    .start = fixture_start,
    .request_stop = fixture_request_stop,
    .is_quiescent = fixture_is_quiescent,
    .destroy = fixture_destroy};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
    return host_abi == SALTS_PLUGIN_ABI_VERSION ? &g_manifest : NULL;
}
