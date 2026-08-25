#ifndef TURBO_ROOM_SERVICE_IRIS_ORM_STORE_H
#define TURBO_ROOM_SERVICE_IRIS_ORM_STORE_H

#include "iris_record_store.h"

#include <stddef.h>

typedef struct iris_orm_store_owner_s iris_orm_store_owner_t;

iris_orm_store_owner_t *iris_orm_store_owner_create(
    const char *yaml_path, const char *channel_name,
    int allow_development_sqlite, char *error, size_t error_capacity);

iris_record_store_t *iris_orm_store_owner_store(
    iris_orm_store_owner_t *owner);

void iris_orm_store_owner_destroy(iris_orm_store_owner_t *owner);

#endif
