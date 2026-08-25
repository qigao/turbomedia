#ifndef TURBO_ROOM_SERVICE_IRIS_RECORD_STORE_H
#define TURBO_ROOM_SERVICE_IRIS_RECORD_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_RECORD_STORE_API_VERSION 1u
#define IRIS_RECORD_REVISION_ABSENT 0u
#define IRIS_RECORD_REVISION_MAX INT64_MAX

typedef enum iris_record_store_capability_e {
    IRIS_RECORD_STORE_DURABLE = 1u << 0,
    IRIS_RECORD_STORE_ATOMIC_BATCH = 1u << 1
} iris_record_store_capability_t;

typedef struct iris_record_view_s {
    size_t size;
    const uint8_t *key;
    size_t key_size;
    uint64_t revision;
    const uint8_t *value;
    size_t value_size;
} iris_record_view_t;

#define IRIS_RECORD_VIEW_INIT                                                \
    { sizeof(iris_record_view_t), NULL, 0u, 0u, NULL, 0u }

typedef enum iris_record_mutation_kind_e {
    IRIS_RECORD_PUT = 1,
    IRIS_RECORD_DELETE
} iris_record_mutation_kind_t;

typedef struct iris_record_mutation_s {
    size_t size;
    iris_record_mutation_kind_t kind;
    const uint8_t *key;
    size_t key_size;
    uint64_t expected_revision;
    uint64_t next_revision;
    const uint8_t *value;
    size_t value_size;
} iris_record_mutation_t;

#define IRIS_RECORD_MUTATION_INIT                                            \
    { sizeof(iris_record_mutation_t), IRIS_RECORD_PUT, NULL, 0u, 0u, 0u,     \
      NULL, 0u }

typedef int (*iris_record_visit_fn)(void *ctx,
                                    const iris_record_view_t *record);

typedef struct iris_record_store_s {
    size_t size;
    uint32_t api_version;
    uint32_t capabilities;
    size_t max_key_size;
    size_t max_value_size;
    size_t max_batch_size;
    size_t max_records;
    void *ctx;
    int (*scan)(void *ctx, iris_record_visit_fn visit, void *visit_ctx);
    int (*commit)(void *ctx, const iris_record_mutation_t *mutations,
                  size_t mutation_count);
} iris_record_store_t;

#define IRIS_RECORD_STORE_INIT                                               \
    { sizeof(iris_record_store_t), IRIS_RECORD_STORE_API_VERSION, 0u, 0u,    \
      0u, 0u, 0u, NULL, NULL, NULL }

#ifdef __cplusplus
}
#endif

#endif
