#include "iris_orm_store.h"

#include <orm.h>
#include <salts_error.h>
#include <salts_fs.h>
#include <cyaml/cyaml.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IRIS_ORM_DEFAULT_BUSY_TIMEOUT_MS = 5000,
    IRIS_ORM_DEFAULT_MAX_KEY_SIZE = 128,
    IRIS_ORM_DEFAULT_MAX_VALUE_SIZE = 16384,
    IRIS_ORM_DEFAULT_MAX_BATCH_SIZE = 256,
    IRIS_ORM_DEFAULT_MAX_RECORDS = 10000
};

static const char IRIS_ORM_CREATE_SQLITE_SQL[] =
    "create table if not exists iris_record_store("
    "namespace text not null,record_key blob not null,revision integer not null,"
    "record_value blob not null,primary key(namespace,record_key))";

static const char IRIS_ORM_CREATE_POSTGRESQL_SQL[] =
    "create table if not exists iris_record_store("
    "namespace text not null,record_key bytea not null,revision bigint not null,"
    "record_value bytea not null,primary key(namespace,record_key))";

enum {
    IRIS_ORM_DRIVER_SQLITE = 1,
    IRIS_ORM_DRIVER_POSTGRESQL
};

typedef struct iris_orm_store_config_s {
    int driver;
    char *database_path;
    char *namespace_name;
    char *pg_host;
    char *pg_port;
    char *pg_user;
    char *pg_password;
    char *pg_dbname;
    char *pg_service;
    char *pg_connect_timeout;
    char *pg_sslmode;
    char *pg_sslrootcert;
    char *pg_sslcert;
    char *pg_sslkey;
    uint32_t busy_timeout_ms;
    size_t max_key_size;
    size_t max_value_size;
    size_t max_batch_size;
    size_t max_records;
    size_t max_bytes;
    size_t max_item_bytes;
} iris_orm_store_config_t;

struct iris_orm_store_owner_s {
    orm_connection_t *connection;
    char *namespace_name;
    int driver;
    size_t max_bytes;
    size_t max_item_bytes;
    iris_record_store_t store;
};

static void iris_orm_write_error(char *error, size_t capacity,
                                 const char *message) {
    if (error && capacity > 0u) {
        (void)snprintf(error, capacity, "%s", message ? message : "unknown error");
    }
}

static void iris_orm_write_orm_error(char *error, size_t capacity,
                                     const char *operation,
                                     const orm_error_t *orm_error) {
    if (error && capacity > 0u) {
        (void)snprintf(error, capacity, "%s: %s", operation,
                       orm_error && orm_error->message[0]
                           ? orm_error->message
                           : "TurboDB ORM failure");
    }
}

static int iris_orm_status(orm_status_t status) {
    switch (status) {
    case ORM_STATUS_OK:
        return SALTS_OK;
    case ORM_STATUS_INVALID_ARGUMENT:
    case ORM_STATUS_ABI_MISMATCH:
    case ORM_STATUS_TYPE_ERROR:
    case ORM_STATUS_OUT_OF_RANGE:
    case ORM_STATUS_INVALID_STATE:
    case ORM_STATUS_NULL_VALUE:
        return SALTS_EINVAL;
    case ORM_STATUS_OUT_OF_MEMORY:
        return SALTS_ENOMEM;
    case ORM_STATUS_BUSY:
        return SALTS_EBUSY;
    case ORM_STATUS_LIMIT_EXCEEDED:
        return SALTS_ENOSPC;
    case ORM_STATUS_UNSUPPORTED:
        return SALTS_ENOTSUP;
    default:
        return SALTS_EIO;
    }
}

static void iris_orm_config_cleanup(iris_orm_store_config_t *config) {
    if (!config) return;
    free(config->database_path);
    free(config->namespace_name);
    free(config->pg_host);
    free(config->pg_port);
    free(config->pg_user);
    free(config->pg_password);
    free(config->pg_dbname);
    free(config->pg_service);
    free(config->pg_connect_timeout);
    free(config->pg_sslmode);
    free(config->pg_sslrootcert);
    free(config->pg_sslcert);
    free(config->pg_sslkey);
    memset(config, 0, sizeof(*config));
}

static const char *iris_orm_sql(const iris_orm_store_owner_t *owner,
                                const char *sqlite_sql,
                                const char *postgresql_sql) {
    return owner && owner->driver == IRIS_ORM_DRIVER_POSTGRESQL
               ? postgresql_sql
               : sqlite_sql;
}

static cyaml_type_t iris_yaml_node_type(const cyaml_node_t *node) {
    return node ? node->type : CYAML_NONE;
}

static cyaml_node_t *iris_yaml_mapping_key(const cyaml_node_t *mapping,
                                           size_t index) {
    cyaml_pair_t *pair = cyaml_map_at(mapping, (uint32_t)index);
    return pair ? pair->key : NULL;
}

static int iris_yaml_mapping(const cyaml_node_t *node) {
    return node && iris_yaml_node_type(node) == CYAML_MAP;
}

static int iris_yaml_mapping_keys_valid(
    const cyaml_doc_t *document, const cyaml_node_t *mapping,
    const char *const *allowed, size_t allowed_count) {
    size_t count;
    size_t i;
    if (!document || !iris_yaml_mapping(mapping) || !allowed) return 0;
    count = cyaml_map_len(mapping);
    for (i = 0u; i < count; ++i) {
        cyaml_node_t *key_node = iris_yaml_mapping_key(mapping, i);
        char *key;
        size_t j;
        int matched = 0;
        if (!key_node || iris_yaml_node_type(key_node) !=
                             CYAML_SCALAR) {
            return 0;
        }
        key = cyaml_scalar_str(document, key_node);
        if (!key) return 0;
        for (j = 0u; j < allowed_count; ++j) {
            if (strcmp(key, allowed[j]) == 0) {
                matched = 1;
                break;
            }
        }
        free(key);
        if (!matched) return 0;
    }
    return 1;
}

static char *iris_yaml_string(const cyaml_doc_t *document,
                              const cyaml_node_t *mapping,
                              const char *key) {
    cyaml_node_t *node;
    if (!document || !mapping || !key) return NULL;
    node = cyaml_get(document, mapping, key);
    if (!node || iris_yaml_node_type(node) != CYAML_SCALAR) {
        return NULL;
    }
    return cyaml_scalar_str(document, node);
}

static int iris_parse_size(const cyaml_doc_t *document,
                           const cyaml_node_t *mapping, const char *key,
                           size_t default_value, size_t minimum,
                           size_t maximum, size_t *out) {
    char *text;
    unsigned long long parsed_value;
    char *end = NULL;
    if (!out) return 0;
    text = iris_yaml_string(document, mapping, key);
    if (!text) {
        *out = default_value;
        return 1;
    }
    errno = 0;
    parsed_value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed_value < minimum ||
        parsed_value > maximum || parsed_value > SIZE_MAX) {
        free(text);
        return 0;
    }
    free(text);
    *out = (size_t)parsed_value;
    return 1;
}

static int iris_orm_parse_config(const char *yaml_path,
                                 const char *channel_name,
                                 int allow_development_sqlite,
                                 iris_orm_store_config_t *config,
                                 char *error, size_t error_capacity) {
    salts_fs_buf_t yaml = {0};
    cyaml_doc_t *document = NULL;
    cyaml_error_t yaml_error;
    cyaml_node_t *root;
    cyaml_node_t *channels;
    cyaml_node_t *channel;
    cyaml_node_t *channel_config;
    char *kind = NULL;
    char *backend = NULL;
    size_t busy_timeout;
    size_t default_item_bytes;
    size_t default_total_bytes;
    int ok = 0;
    static const char *const root_keys[] = {
        "version", "channels", "adapters"};
    static const char *const channel_keys[] = {"kind", "config"};
    static const char *const sqlite_config_keys[] = {
        "backend", "database_path", "namespace_name", "busy_timeout_ms",
        "max_records", "max_bytes", "max_item_bytes", "max_key_size",
        "max_value_size", "max_batch_size"};
    static const char *const postgresql_config_keys[] = {
        "backend", "namespace_name", "host", "port", "user", "password",
        "dbname", "service", "connect_timeout", "sslmode", "sslrootcert",
        "sslcert", "sslkey", "max_records", "max_bytes", "max_item_bytes",
        "max_key_size", "max_value_size", "max_batch_size"};

    memset(&yaml_error, 0, sizeof(yaml_error));
    if (!yaml_path || !yaml_path[0] || !channel_name || !channel_name[0] ||
        !config) {
        iris_orm_write_error(error, error_capacity,
                             "invalid ORM store configuration");
        return 0;
    }
    memset(config, 0, sizeof(*config));
    if (salts_fs_read_file(yaml_path, &yaml) != SALTS_OK) {
        iris_orm_write_error(error, error_capacity,
                             "cannot read ORM store YAML");
        goto cleanup;
    }
    document = cyaml_parse(yaml.base, yaml.len, NULL, &yaml_error);
    if (!document) {
        iris_orm_write_error(error, error_capacity,
                             yaml_error.msg[0] ? yaml_error.msg
                                               : "invalid ORM store YAML");
        goto cleanup;
    }
    root = cyaml_root(document);
    if (!iris_yaml_mapping_keys_valid(
            document, root, root_keys,
            sizeof(root_keys) / sizeof(root_keys[0]))) {
        iris_orm_write_error(error, error_capacity,
                             "unknown ORM store root field");
        goto cleanup;
    }
    channels = iris_yaml_mapping(root)
                   ? cyaml_get(document, root, "channels")
                   : NULL;
    channel = iris_yaml_mapping(channels)
                  ? cyaml_get(document, channels, channel_name)
                  : NULL;
    channel_config = iris_yaml_mapping(channel)
                         ? cyaml_get(document, channel, "config")
                         : NULL;
    if (!iris_yaml_mapping(channel_config)) {
        iris_orm_write_error(error, error_capacity,
                             "missing ORM record-store channel");
        goto cleanup;
    }
    if (!iris_yaml_mapping_keys_valid(
            document, channel, channel_keys,
            sizeof(channel_keys) / sizeof(channel_keys[0]))) {
        iris_orm_write_error(error, error_capacity,
                             "unknown ORM channel field");
        goto cleanup;
    }
    kind = iris_yaml_string(document, channel, "kind");
    backend = iris_yaml_string(document, channel_config, "backend");
    if (!kind || strcmp(kind, "record_store") != 0 || !backend) {
        iris_orm_write_error(error, error_capacity,
                             "invalid ORM record-store channel");
        goto cleanup;
    }
    if (strcmp(backend, "redis") == 0) {
        iris_orm_write_error(error, error_capacity,
                             "Redis ORM cannot preserve atomic revision CAS");
        goto cleanup;
    }
    if (strcmp(backend, "sqlite") != 0 &&
        strcmp(backend, "postgresql") != 0) {
        iris_orm_write_error(error, error_capacity,
                             "unsupported TurboDB ORM backend");
        goto cleanup;
    }
    config->namespace_name =
        iris_yaml_string(document, channel_config, "namespace_name");
    if (!config->namespace_name || !config->namespace_name[0]) {
        iris_orm_write_error(error, error_capacity,
                             "ORM store requires namespace_name");
        goto cleanup;
    }
    if (strcmp(backend, "sqlite") == 0) {
        config->driver = IRIS_ORM_DRIVER_SQLITE;
        if (!iris_yaml_mapping_keys_valid(
                document, channel_config, sqlite_config_keys,
                sizeof(sqlite_config_keys) / sizeof(sqlite_config_keys[0]))) {
            iris_orm_write_error(error, error_capacity,
                                 "unknown SQLite ORM configuration field");
            goto cleanup;
        }
        if (!allow_development_sqlite) {
            iris_orm_write_error(error, error_capacity,
                                 "SQLite ORM requires explicit development opt-in");
            goto cleanup;
        }
        config->database_path =
            iris_yaml_string(document, channel_config, "database_path");
        if (!config->database_path || !config->database_path[0] ||
            !iris_parse_size(document, channel_config, "busy_timeout_ms",
                             IRIS_ORM_DEFAULT_BUSY_TIMEOUT_MS, 1u,
                             UINT32_MAX, &busy_timeout)) {
            iris_orm_write_error(error, error_capacity,
                                 "SQLite ORM requires a valid database_path and busy_timeout_ms");
            goto cleanup;
        }
        config->busy_timeout_ms = (uint32_t)busy_timeout;
    } else {
        config->driver = IRIS_ORM_DRIVER_POSTGRESQL;
        if (!iris_yaml_mapping_keys_valid(
                document, channel_config, postgresql_config_keys,
                sizeof(postgresql_config_keys) /
                    sizeof(postgresql_config_keys[0]))) {
            iris_orm_write_error(error, error_capacity,
                                 "unknown PostgreSQL ORM configuration field");
            goto cleanup;
        }
        config->pg_host = iris_yaml_string(document, channel_config, "host");
        config->pg_port = iris_yaml_string(document, channel_config, "port");
        config->pg_user = iris_yaml_string(document, channel_config, "user");
        config->pg_password =
            iris_yaml_string(document, channel_config, "password");
        config->pg_dbname =
            iris_yaml_string(document, channel_config, "dbname");
        config->pg_service =
            iris_yaml_string(document, channel_config, "service");
        config->pg_connect_timeout =
            iris_yaml_string(document, channel_config, "connect_timeout");
        config->pg_sslmode =
            iris_yaml_string(document, channel_config, "sslmode");
        config->pg_sslrootcert =
            iris_yaml_string(document, channel_config, "sslrootcert");
        config->pg_sslcert =
            iris_yaml_string(document, channel_config, "sslcert");
        config->pg_sslkey =
            iris_yaml_string(document, channel_config, "sslkey");
        if ((!config->pg_host && !config->pg_port && !config->pg_user &&
             !config->pg_password && !config->pg_dbname &&
             !config->pg_service && !config->pg_connect_timeout &&
             !config->pg_sslmode && !config->pg_sslrootcert &&
             !config->pg_sslcert && !config->pg_sslkey) ||
            (config->pg_host && !config->pg_host[0]) ||
            (config->pg_port && !config->pg_port[0]) ||
            (config->pg_user && !config->pg_user[0]) ||
            (config->pg_password && !config->pg_password[0]) ||
            (config->pg_dbname && !config->pg_dbname[0]) ||
            (config->pg_service && !config->pg_service[0]) ||
            (config->pg_connect_timeout && !config->pg_connect_timeout[0]) ||
            (config->pg_sslmode && !config->pg_sslmode[0]) ||
            (config->pg_sslrootcert && !config->pg_sslrootcert[0]) ||
            (config->pg_sslcert && !config->pg_sslcert[0]) ||
            (config->pg_sslkey && !config->pg_sslkey[0])) {
            iris_orm_write_error(error, error_capacity,
                                 "PostgreSQL ORM requires non-empty libpq connection options");
            goto cleanup;
        }
    }
    if (!iris_parse_size(document, channel_config, "max_key_size",
                         IRIS_ORM_DEFAULT_MAX_KEY_SIZE, 1u, UINT32_MAX,
                         &config->max_key_size) ||
        !iris_parse_size(document, channel_config, "max_value_size",
                         IRIS_ORM_DEFAULT_MAX_VALUE_SIZE, 1u, UINT32_MAX,
                         &config->max_value_size) ||
        !iris_parse_size(document, channel_config, "max_batch_size",
                         IRIS_ORM_DEFAULT_MAX_BATCH_SIZE, 1u, UINT32_MAX,
                         &config->max_batch_size) ||
        !iris_parse_size(document, channel_config, "max_records",
                         IRIS_ORM_DEFAULT_MAX_RECORDS, 1u, UINT32_MAX,
                         &config->max_records)) {
        iris_orm_write_error(error, error_capacity,
                             "invalid ORM store capacity limit");
        goto cleanup;
    }
    if (config->max_key_size > SIZE_MAX - config->max_value_size) {
        iris_orm_write_error(error, error_capacity,
                             "ORM store item capacity overflows size_t");
        goto cleanup;
    }
    default_item_bytes = config->max_key_size + config->max_value_size;
    default_total_bytes = config->max_records > SIZE_MAX / default_item_bytes
                              ? SIZE_MAX
                              : config->max_records * default_item_bytes;
    if (!iris_parse_size(document, channel_config, "max_item_bytes",
                         default_item_bytes, 1u, SIZE_MAX,
                         &config->max_item_bytes) ||
        !iris_parse_size(document, channel_config, "max_bytes",
                         default_total_bytes, 1u, SIZE_MAX,
                         &config->max_bytes)) {
        iris_orm_write_error(error, error_capacity,
                             "invalid ORM store byte limit");
        goto cleanup;
    }
    ok = 1;
cleanup:
    free(kind);
    free(backend);
    cyaml_free(document);
    salts_fs_buf_free(&yaml);
    if (!ok) iris_orm_config_cleanup(config);
    return ok;
}

static int iris_orm_execute_raw(iris_orm_store_owner_t *owner,
                                orm_transaction_t *transaction,
                                const char *sql, const orm_value_t *values,
                                size_t value_count, orm_result_t **out_result) {
    orm_error_t error;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    orm_status_t status;
    size_t i;
    int rc;
    if (!owner || !sql || (value_count > 0u && !values)) return SALTS_EINVAL;
    orm_error_init(&error);
    status = orm_raw(owner->connection, orm_view(sql), &query, &error);
    if (status != ORM_STATUS_OK) return iris_orm_status(status);
    for (i = 0u; i < value_count; ++i) {
        status = orm_query_bind(query, values[i], &error);
        if (status != ORM_STATUS_OK) {
            orm_query_destroy(query);
            return iris_orm_status(status);
        }
    }
    status = transaction
                 ? orm_query_execute_in_transaction(query, transaction,
                                                    &result, &error)
                 : orm_query_execute(query, &result, &error);
    rc = iris_orm_status(status);
    orm_query_destroy(query);
    if (rc != SALTS_OK) {
        orm_result_destroy(result);
        return rc;
    }
    if (out_result) {
        *out_result = result;
    } else {
        orm_result_destroy(result);
    }
    return SALTS_OK;
}

static int iris_orm_current_revision(iris_orm_store_owner_t *owner,
                                     orm_transaction_t *transaction,
                                     const uint8_t *key, size_t key_size,
                                     uint64_t *revision, int *exists,
                                     uint64_t *item_bytes) {
    static const char sqlite_sql[] =
        "select revision,length(record_key)+length(record_value) "
        "from iris_record_store where namespace=? and record_key=?";
    static const char postgresql_sql[] =
        "select revision,length(record_key)+length(record_value) "
        "from iris_record_store where namespace=$1 and record_key=$2";
    orm_value_t values[2];
    orm_result_t *result = NULL;
    orm_error_t error;
    uint64_t rows = 0u;
    int64_t stored_revision = 0;
    int64_t stored_bytes = 0;
    int rc;
    if (!revision || !exists || !item_bytes) return SALTS_EINVAL;
    values[0] = orm_text(owner->namespace_name);
    values[1] = orm_blob(key, key_size);
    rc = iris_orm_execute_raw(
        owner, transaction,
        iris_orm_sql(owner, sqlite_sql, postgresql_sql), values, 2u, &result);
    if (rc != SALTS_OK) return rc;
    orm_error_init(&error);
    if (orm_result_row_count(result, &rows, &error) != ORM_STATUS_OK ||
        rows > 1u) {
        orm_result_destroy(result);
        return SALTS_EIO;
    }
    if (rows == 0u) {
        *exists = 0;
        *revision = IRIS_RECORD_REVISION_ABSENT;
        *item_bytes = 0u;
    } else if (orm_result_get_int64(result, 0u, 0u, &stored_revision, &error) !=
                   ORM_STATUS_OK ||
               orm_result_get_int64(result, 0u, 1u, &stored_bytes, &error) !=
                   ORM_STATUS_OK ||
               stored_revision <= 0 || stored_bytes <= 0) {
        orm_result_destroy(result);
        return SALTS_EIO;
    } else {
        *exists = 1;
        *revision = (uint64_t)stored_revision;
        *item_bytes = (uint64_t)stored_bytes;
    }
    orm_result_destroy(result);
    return SALTS_OK;
}

static int iris_orm_record_count(iris_orm_store_owner_t *owner,
                                 orm_transaction_t *transaction,
                                 uint64_t *count) {
    static const char sqlite_sql[] =
        "select count(*) from iris_record_store where namespace=?";
    static const char postgresql_sql[] =
        "select count(*) from iris_record_store where namespace=$1";
    orm_value_t namespace_value = orm_text(owner->namespace_name);
    orm_result_t *result = NULL;
    orm_error_t error;
    int64_t signed_count = 0;
    int rc = iris_orm_execute_raw(
        owner, transaction,
        iris_orm_sql(owner, sqlite_sql, postgresql_sql), &namespace_value, 1u,
        &result);
    if (rc != SALTS_OK) return rc;
    orm_error_init(&error);
    if (orm_result_get_int64(result, 0u, 0u, &signed_count, &error) !=
            ORM_STATUS_OK ||
        signed_count < 0) {
        orm_result_destroy(result);
        return SALTS_EIO;
    }
    *count = (uint64_t)signed_count;
    orm_result_destroy(result);
    return SALTS_OK;
}

static int iris_orm_total_bytes(iris_orm_store_owner_t *owner,
                                orm_transaction_t *transaction,
                                uint64_t *total_bytes) {
    static const char sqlite_sql[] =
        "select coalesce(sum(length(record_key)+length(record_value)),0) "
        "from iris_record_store where namespace=?";
    static const char postgresql_sql[] =
        "select coalesce(sum(length(record_key)+length(record_value)),0) "
        "from iris_record_store where namespace=$1";
    orm_value_t namespace_value = orm_text(owner->namespace_name);
    orm_result_t *result = NULL;
    orm_error_t error;
    int64_t signed_total = 0;
    int rc;
    if (!total_bytes) return SALTS_EINVAL;
    rc = iris_orm_execute_raw(
        owner, transaction,
        iris_orm_sql(owner, sqlite_sql, postgresql_sql), &namespace_value, 1u,
        &result);
    if (rc != SALTS_OK) return rc;
    orm_error_init(&error);
    if (orm_result_get_int64(result, 0u, 0u, &signed_total, &error) !=
            ORM_STATUS_OK ||
        signed_total < 0) {
        orm_result_destroy(result);
        return SALTS_EIO;
    }
    *total_bytes = (uint64_t)signed_total;
    orm_result_destroy(result);
    return SALTS_OK;
}

static int iris_orm_scan(void *context, iris_record_visit_fn visit,
                         void *visit_context) {
    static const char sqlite_sql[] =
        "select record_key,revision,record_value from iris_record_store "
        "where namespace=? order by record_key";
    static const char postgresql_sql[] =
        "select record_key,revision,record_value from iris_record_store "
        "where namespace=$1 order by record_key";
    iris_orm_store_owner_t *owner = (iris_orm_store_owner_t *)context;
    orm_value_t namespace_value;
    orm_result_t *result = NULL;
    orm_error_t error;
    uint64_t rows = 0u;
    uint64_t row;
    size_t scanned_bytes = 0u;
    int rc;
    if (!owner || !visit) return SALTS_EINVAL;
    namespace_value = orm_text(owner->namespace_name);
    rc = iris_orm_execute_raw(
        owner, NULL, iris_orm_sql(owner, sqlite_sql, postgresql_sql),
        &namespace_value, 1u, &result);
    if (rc != SALTS_OK) return rc;
    orm_error_init(&error);
    if (orm_result_row_count(result, &rows, &error) != ORM_STATUS_OK ||
        rows > owner->store.max_records) {
        orm_result_destroy(result);
        return SALTS_EIO;
    }
    for (row = 0u; row < rows; ++row) {
        iris_record_view_t record = IRIS_RECORD_VIEW_INIT;
        orm_blob_t key = {0};
        orm_blob_t payload = {0};
        int64_t revision = 0;
        if (orm_result_get_blob(result, row, 0u, &key, &error) !=
                ORM_STATUS_OK ||
            orm_result_get_int64(result, row, 1u, &revision, &error) !=
                ORM_STATUS_OK ||
            orm_result_get_blob(result, row, 2u, &payload, &error) !=
                ORM_STATUS_OK ||
            revision <= 0 || key.size == 0u ||
            key.size > owner->store.max_key_size ||
            payload.size > owner->store.max_value_size ||
            key.size > SIZE_MAX - payload.size ||
            key.size + payload.size > owner->max_item_bytes ||
            key.size + payload.size > owner->max_bytes ||
            scanned_bytes > owner->max_bytes - (key.size + payload.size)) {
            orm_result_destroy(result);
            return SALTS_ENOSPC;
        }
        scanned_bytes += key.size + payload.size;
        record.key = (const uint8_t *)key.data;
        record.key_size = key.size;
        record.revision = (uint64_t)revision;
        record.value = (const uint8_t *)payload.data;
        record.value_size = payload.size;
        rc = visit(visit_context, &record);
        if (rc != SALTS_OK) {
            orm_result_destroy(result);
            return rc;
        }
    }
    orm_result_destroy(result);
    return SALTS_OK;
}

static int iris_orm_validate_mutations(
    const iris_orm_store_owner_t *owner,
    const iris_record_mutation_t *mutations, size_t mutation_count) {
    size_t i;
    size_t j;
    if (!owner || !mutations || mutation_count == 0u ||
        mutation_count > owner->store.max_batch_size) {
        return SALTS_EINVAL;
    }
    for (i = 0u; i < mutation_count; ++i) {
        const iris_record_mutation_t *mutation = &mutations[i];
        if (mutation->size < sizeof(*mutation) || !mutation->key ||
            mutation->key_size == 0u ||
            mutation->key_size > owner->store.max_key_size ||
            mutation->expected_revision > IRIS_RECORD_REVISION_MAX) {
            return SALTS_EINVAL;
        }
        if (mutation->kind == IRIS_RECORD_PUT) {
            if ((!mutation->value && mutation->value_size > 0u) ||
                mutation->value_size > owner->store.max_value_size ||
                mutation->next_revision <= mutation->expected_revision ||
                mutation->next_revision > IRIS_RECORD_REVISION_MAX) {
                return SALTS_EINVAL;
            }
            if (mutation->key_size > SIZE_MAX - mutation->value_size ||
                mutation->key_size + mutation->value_size >
                    owner->max_item_bytes) {
                return SALTS_ENOSPC;
            }
        } else if (mutation->kind == IRIS_RECORD_DELETE) {
            if (mutation->expected_revision == IRIS_RECORD_REVISION_ABSENT ||
                mutation->next_revision != IRIS_RECORD_REVISION_ABSENT ||
                mutation->value || mutation->value_size != 0u) {
                return SALTS_EINVAL;
            }
        } else {
            return SALTS_EINVAL;
        }
        for (j = i + 1u; j < mutation_count; ++j) {
            if (mutation->key_size == mutations[j].key_size &&
                memcmp(mutation->key, mutations[j].key,
                       mutation->key_size) == 0) {
                return SALTS_EINVAL;
            }
        }
    }
    return SALTS_OK;
}

static int iris_orm_mutate(iris_orm_store_owner_t *owner,
                           orm_transaction_t *transaction,
                           const iris_record_mutation_t *mutation) {
    static const char insert_sqlite_sql[] =
        "insert into iris_record_store(namespace,record_key,revision,record_value) "
        "values(?,?,?,?)";
    static const char insert_postgresql_sql[] =
        "insert into iris_record_store(namespace,record_key,revision,record_value) "
        "values($1,$2,$3,$4)";
    static const char update_sqlite_sql[] =
        "update iris_record_store set revision=?,record_value=? "
        "where namespace=? and record_key=? and revision=?";
    static const char update_postgresql_sql[] =
        "update iris_record_store set revision=$1,record_value=$2 "
        "where namespace=$3 and record_key=$4 and revision=$5";
    static const char delete_sqlite_sql[] =
        "delete from iris_record_store where namespace=? and record_key=? and revision=?";
    static const char delete_postgresql_sql[] =
        "delete from iris_record_store where namespace=$1 and record_key=$2 and revision=$3";
    orm_value_t values[5];
    orm_result_t *result = NULL;
    orm_error_t error;
    uint64_t affected = 0u;
    int rc;
    if (mutation->kind == IRIS_RECORD_DELETE) {
        values[0] = orm_text(owner->namespace_name);
        values[1] = orm_blob(mutation->key, mutation->key_size);
        values[2] = orm_u64(mutation->expected_revision);
        rc = iris_orm_execute_raw(
            owner, transaction,
            iris_orm_sql(owner, delete_sqlite_sql, delete_postgresql_sql),
            values, 3u, &result);
    } else if (mutation->expected_revision == IRIS_RECORD_REVISION_ABSENT) {
        values[0] = orm_text(owner->namespace_name);
        values[1] = orm_blob(mutation->key, mutation->key_size);
        values[2] = orm_u64(mutation->next_revision);
        values[3] = orm_blob(mutation->value, mutation->value_size);
        rc = iris_orm_execute_raw(
            owner, transaction,
            iris_orm_sql(owner, insert_sqlite_sql, insert_postgresql_sql),
            values, 4u, &result);
    } else {
        values[0] = orm_u64(mutation->next_revision);
        values[1] = orm_blob(mutation->value, mutation->value_size);
        values[2] = orm_text(owner->namespace_name);
        values[3] = orm_blob(mutation->key, mutation->key_size);
        values[4] = orm_u64(mutation->expected_revision);
        rc = iris_orm_execute_raw(
            owner, transaction,
            iris_orm_sql(owner, update_sqlite_sql, update_postgresql_sql),
            values, 5u, &result);
    }
    if (rc != SALTS_OK) return rc;
    orm_error_init(&error);
    if (orm_result_affected_rows(result, &affected, &error) != ORM_STATUS_OK) {
        orm_result_destroy(result);
        return SALTS_EIO;
    }
    orm_result_destroy(result);
    return affected == 1u ? SALTS_OK : SALTS_EBUSY;
}

static int iris_orm_commit(void *context,
                           const iris_record_mutation_t *mutations,
                           size_t mutation_count) {
    iris_orm_store_owner_t *owner = (iris_orm_store_owner_t *)context;
    orm_transaction_t *transaction = NULL;
    orm_error_t error;
    uint64_t record_count = 0u;
    uint64_t total_bytes = 0u;
    size_t inserts = 0u;
    size_t deletes = 0u;
    size_t i;
    int rc;
    orm_status_t status;
    rc = iris_orm_validate_mutations(owner, mutations, mutation_count);
    if (rc != SALTS_OK) return rc;
    orm_error_init(&error);
    status = orm_transaction_begin(owner->connection,
                                   ORM_ISOLATION_SERIALIZABLE,
                                   &transaction, &error);
    if (status != ORM_STATUS_OK) return iris_orm_status(status);
    rc = iris_orm_record_count(owner, transaction, &record_count);
    if (rc != SALTS_OK) goto rollback;
    rc = iris_orm_total_bytes(owner, transaction, &total_bytes);
    if (rc != SALTS_OK) goto rollback;
    if (total_bytes > owner->max_bytes) {
        rc = SALTS_ENOSPC;
        goto rollback;
    }
    for (i = 0u; i < mutation_count; ++i) {
        uint64_t revision = 0u;
        uint64_t old_item_bytes = 0u;
        uint64_t new_item_bytes = 0u;
        int exists = 0;
        rc = iris_orm_current_revision(owner, transaction, mutations[i].key,
                                       mutations[i].key_size, &revision, &exists,
                                       &old_item_bytes);
        if (rc != SALTS_OK) goto rollback;
        if ((exists && revision != mutations[i].expected_revision) ||
            (!exists && mutations[i].expected_revision !=
                            IRIS_RECORD_REVISION_ABSENT)) {
            rc = SALTS_EBUSY;
            goto rollback;
        }
        if (!exists && mutations[i].kind == IRIS_RECORD_PUT) inserts++;
        if (exists && mutations[i].kind == IRIS_RECORD_DELETE) deletes++;
        if (mutations[i].kind == IRIS_RECORD_PUT) {
            new_item_bytes = (uint64_t)mutations[i].key_size +
                             (uint64_t)mutations[i].value_size;
        }
        if (total_bytes < old_item_bytes ||
            new_item_bytes > UINT64_MAX - (total_bytes - old_item_bytes) ||
            total_bytes - old_item_bytes + new_item_bytes > owner->max_bytes) {
            rc = SALTS_ENOSPC;
            goto rollback;
        }
        total_bytes = total_bytes - old_item_bytes + new_item_bytes;
    }
    if (record_count + inserts < deletes ||
        record_count + inserts - deletes > owner->store.max_records) {
        rc = SALTS_ENOSPC;
        goto rollback;
    }
    for (i = 0u; i < mutation_count; ++i) {
        rc = iris_orm_mutate(owner, transaction, &mutations[i]);
        if (rc != SALTS_OK) goto rollback;
    }
    status = orm_transaction_commit(transaction, &error);
    orm_transaction_destroy(transaction);
    return iris_orm_status(status);
rollback:
    (void)orm_transaction_rollback(transaction, &error);
    orm_transaction_destroy(transaction);
    return rc;
}

static void iris_orm_add_option(orm_option_t *options, size_t capacity,
                                size_t *count, const char *keyword,
                                const char *value) {
    if (!options || !count || *count >= capacity || !keyword || !value) return;
    options[*count].keyword = orm_view(keyword);
    options[*count].value = orm_view(value);
    (*count)++;
}

iris_orm_store_owner_t *iris_orm_store_owner_create(
    const char *yaml_path, const char *channel_name,
    int allow_development_sqlite, char *error, size_t error_capacity) {
    iris_orm_store_config_t parsed;
    iris_orm_store_owner_t *owner = NULL;
    orm_config_t orm_configuration;
    orm_option_t options[11];
    orm_error_t orm_error;
    orm_result_t *result = NULL;
    char timeout[16];
    size_t option_count = 0u;
    uint64_t parameter_bytes;
    uint64_t parameter_overhead;
    uint64_t result_bytes_per_record;
    orm_status_t status;

    if (!iris_orm_parse_config(yaml_path, channel_name,
                               allow_development_sqlite, &parsed,
                               error, error_capacity)) {
        return NULL;
    }
    owner = (iris_orm_store_owner_t *)calloc(1u, sizeof(*owner));
    if (!owner) {
        iris_orm_write_error(error, error_capacity,
                             "cannot allocate ORM store owner");
        iris_orm_config_cleanup(&parsed);
        return NULL;
    }
    owner->namespace_name = parsed.namespace_name;
    parsed.namespace_name = NULL;
    owner->driver = parsed.driver;
    owner->max_bytes = parsed.max_bytes;
    owner->max_item_bytes = parsed.max_item_bytes;
    orm_config(&orm_configuration);
    if (parsed.driver == IRIS_ORM_DRIVER_SQLITE) {
        (void)snprintf(timeout, sizeof(timeout), "%u", parsed.busy_timeout_ms);
        iris_orm_add_option(options, 11u, &option_count, "filename",
                            parsed.database_path);
        iris_orm_add_option(options, 11u, &option_count, "open_mode",
                            "read_write_create");
        iris_orm_add_option(options, 11u, &option_count, "busy_timeout_ms",
                            timeout);
        orm_configuration.driver = orm_view("sqlite");
    } else {
        iris_orm_add_option(options, 11u, &option_count, "host",
                            parsed.pg_host);
        iris_orm_add_option(options, 11u, &option_count, "port",
                            parsed.pg_port);
        iris_orm_add_option(options, 11u, &option_count, "user",
                            parsed.pg_user);
        iris_orm_add_option(options, 11u, &option_count, "password",
                            parsed.pg_password);
        iris_orm_add_option(options, 11u, &option_count, "dbname",
                            parsed.pg_dbname);
        iris_orm_add_option(options, 11u, &option_count, "service",
                            parsed.pg_service);
        iris_orm_add_option(options, 11u, &option_count, "connect_timeout",
                            parsed.pg_connect_timeout);
        iris_orm_add_option(options, 11u, &option_count, "sslmode",
                            parsed.pg_sslmode);
        iris_orm_add_option(options, 11u, &option_count, "sslrootcert",
                            parsed.pg_sslrootcert);
        iris_orm_add_option(options, 11u, &option_count, "sslcert",
                            parsed.pg_sslcert);
        iris_orm_add_option(options, 11u, &option_count, "sslkey",
                            parsed.pg_sslkey);
        orm_configuration.driver = orm_view("postgresql");
    }
    orm_configuration.options = options;
    orm_configuration.option_count = (uint32_t)option_count;
    orm_configuration.max_parameters =
        (uint32_t)(parsed.max_batch_size > UINT32_MAX / 5u
                       ? UINT32_MAX
                       : parsed.max_batch_size * 5u);
    orm_configuration.max_result_rows = parsed.max_records;
    parameter_bytes = (uint64_t)parsed.max_item_bytes;
    parameter_overhead = (uint64_t)parsed.max_key_size +
                         (uint64_t)strlen(owner->namespace_name) + 128u;
    orm_configuration.max_parameter_bytes =
        parameter_bytes > UINT64_MAX - parameter_overhead
            ? UINT64_MAX
            : parameter_bytes + parameter_overhead;
    result_bytes_per_record =
        (uint64_t)parsed.max_key_size + (uint64_t)parsed.max_value_size + 64u;
    orm_configuration.max_result_bytes =
        parsed.max_records > UINT64_MAX / result_bytes_per_record
            ? UINT64_MAX
            : (uint64_t)parsed.max_records * result_bytes_per_record;
    orm_error_init(&orm_error);
    status = orm_connect(&orm_configuration, &owner->connection, &orm_error);
    if (status != ORM_STATUS_OK) {
        iris_orm_write_orm_error(error, error_capacity,
                                 "cannot connect TurboDB ORM", &orm_error);
        goto fail;
    }
    if (iris_orm_execute_raw(
            owner, NULL,
            iris_orm_sql(owner, IRIS_ORM_CREATE_SQLITE_SQL,
                         IRIS_ORM_CREATE_POSTGRESQL_SQL),
            NULL, 0u, &result) != SALTS_OK) {
        iris_orm_write_error(error, error_capacity,
                             "cannot initialize ORM record-store schema");
        goto fail;
    }
    orm_result_destroy(result);
    result = NULL;
    owner->store = (iris_record_store_t)IRIS_RECORD_STORE_INIT;
    owner->store.capabilities =
        IRIS_RECORD_STORE_DURABLE | IRIS_RECORD_STORE_ATOMIC_BATCH;
    owner->store.max_key_size = parsed.max_key_size;
    owner->store.max_value_size = parsed.max_value_size;
    owner->store.max_batch_size = parsed.max_batch_size;
    owner->store.max_records = parsed.max_records;
    owner->store.ctx = owner;
    owner->store.scan = iris_orm_scan;
    owner->store.commit = iris_orm_commit;
    iris_orm_config_cleanup(&parsed);
    return owner;
fail:
    orm_result_destroy(result);
    iris_orm_store_owner_destroy(owner);
    iris_orm_config_cleanup(&parsed);
    return NULL;
}

iris_record_store_t *iris_orm_store_owner_store(
    iris_orm_store_owner_t *owner) {
    return owner ? &owner->store : NULL;
}

void iris_orm_store_owner_destroy(iris_orm_store_owner_t *owner) {
    if (!owner) return;
    orm_disconnect(owner->connection);
    free(owner->namespace_name);
    free(owner);
}
