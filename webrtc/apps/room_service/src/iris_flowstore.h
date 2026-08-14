#ifndef TURBO_ROOM_SERVICE_IRIS_FLOWSTORE_H
#define TURBO_ROOM_SERVICE_IRIS_FLOWSTORE_H

#include "turbo_flow_record_store.h"

#include <stddef.h>

typedef struct iris_flowstore_owner_s iris_flowstore_owner_t;

/**
 * Resolve and own one durable RecordStore channel from FlowStore YAML.
 *
 * SQLite is development-only and requires explicit opt-in. Redis and
 * PostgreSQL are accepted production backends. No volatile fallback exists.
 */
iris_flowstore_owner_t *iris_flowstore_owner_create(
    const char *yaml_path, const char *channel_name,
    int allow_development_sqlite, char *error, size_t error_capacity);

turbo_flow_record_store_t *iris_flowstore_owner_store(
    iris_flowstore_owner_t *owner);

void iris_flowstore_owner_destroy(iris_flowstore_owner_t *owner);

#endif
