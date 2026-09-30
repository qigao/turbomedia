#include "iris_orm_store.h"

#include <salts_error.h>
#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct live_snapshot_s {
    size_t count;
    int saw_a;
    int saw_b;
    uint64_t revision_a;
    uint64_t revision_b;
    uint8_t value_a[32];
    uint8_t value_b[32];
    size_t value_a_size;
    size_t value_b_size;
} live_snapshot_t;

static int live_visit(void *context, const iris_record_view_t *record) {
    live_snapshot_t *snapshot = (live_snapshot_t *)context;
    uint8_t *value;
    size_t *value_size;
    uint64_t *revision;
    int *seen;

    if (!snapshot || !record || record->key_size != 1u)
        return SALTS_EINVAL;
    if (record->key[0] == (uint8_t)'a') {
        value = snapshot->value_a;
        value_size = &snapshot->value_a_size;
        revision = &snapshot->revision_a;
        seen = &snapshot->saw_a;
    } else if (record->key[0] == (uint8_t)'b') {
        value = snapshot->value_b;
        value_size = &snapshot->value_b_size;
        revision = &snapshot->revision_b;
        seen = &snapshot->saw_b;
    } else {
        return SALTS_EINVAL;
    }
    if (*seen || record->value_size > 32u)
        return SALTS_EINVAL;
    *seen = 1;
    *revision = record->revision;
    *value_size = record->value_size;
    if (record->value_size != 0u)
        memcpy(value, record->value, record->value_size);
    snapshot->count++;
    return SALTS_OK;
}

static int live_scan(iris_record_store_t *store, live_snapshot_t *snapshot) {
    if (!store || !snapshot)
        return SALTS_EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    return store->scan(store->ctx, live_visit, snapshot);
}

static int live_value_equal(const uint8_t *actual, size_t actual_size,
                            const char *expected) {
    size_t expected_size = strlen(expected);
    return actual_size == expected_size &&
           memcmp(actual, expected, expected_size) == 0;
}

static void live_put(iris_record_mutation_t *mutation, uint8_t key,
                     uint64_t expected_revision, uint64_t next_revision,
                     const char *value) {
    *mutation = (iris_record_mutation_t)IRIS_RECORD_MUTATION_INIT;
    mutation->kind = IRIS_RECORD_PUT;
    mutation->key = &key; /* overwritten by caller with stable key storage */
    mutation->key_size = 1u;
    mutation->expected_revision = expected_revision;
    mutation->next_revision = next_revision;
    mutation->value = (const uint8_t *)value;
    mutation->value_size = strlen(value);
}

static void live_delete(iris_record_mutation_t *mutation, const uint8_t *key,
                        uint64_t expected_revision) {
    *mutation = (iris_record_mutation_t)IRIS_RECORD_MUTATION_INIT;
    mutation->kind = IRIS_RECORD_DELETE;
    mutation->key = key;
    mutation->key_size = 1u;
    mutation->expected_revision = expected_revision;
}

static char *live_profile(void) {
    const char *driver = getenv("TURBODB_POSTGRES_DRIVER_MODULE");
    const char *service = getenv("TURBO_MEDIA_TEST_POSTGRES_SERVICE");
    char *path = tt_make_temp_file("iris-orm-live", ".yaml");
    char yaml[4096];
    int written;

    check_not_null(driver);
    check_not_null(service);
    check_not_null(path);
    if (!driver || !service || !path)
        return path;
    check_null(strchr(driver, '\''));
    check_null(strchr(service, '\''));
    if (strchr(driver, '\'') || strchr(service, '\''))
        return path;

    written = snprintf(
        yaml, sizeof(yaml),
        "version: 1\n"
        "channels:\n"
        "  iris.live.gate:\n"
        "    kind: record_store\n"
        "    config:\n"
        "      backend: postgresql\n"
        "      driver_module: '%s'\n"
        "      service: '%s'\n"
        "      namespace_name: iris.live.gate\n"
        "      max_records: 32\n"
        "      max_bytes: 1048576\n"
        "      max_item_bytes: 32768\n"
        "      max_key_size: 128\n"
        "      max_value_size: 16384\n"
        "      max_batch_size: 8\n"
        "adapters: {}\n",
        driver, service);
    check_true(written > 0 && (size_t)written < sizeof(yaml));
    if (written <= 0 || (size_t)written >= sizeof(yaml))
        return path;
    check_equal(tt_write_file(path, yaml, (size_t)written), 0);
    return path;
}

spec("Iris PostgreSQL ORM live record store") {
    it("preserves scan, CAS, atomic batch and rollback semantics") {
        static const uint8_t key_a[] = {'a'};
        static const uint8_t key_b[] = {'b'};
        char error[256] = {0};
        char *yaml_path = live_profile();
        iris_orm_store_owner_t *owner = NULL;
        iris_record_store_t *store;
        iris_record_mutation_t mutations[2];
        live_snapshot_t snapshot;

        check_not_null(yaml_path);
        if (!yaml_path)
            return;

        owner = iris_orm_store_owner_create(
            yaml_path, "iris.live.gate", error, sizeof(error));
        check_not_null(owner);
        if (!owner) {
            fprintf(stderr, "live PostgreSQL owner create failed: %s\n", error);
            check_equal(tt_remove_file(yaml_path), 0);
            free(yaml_path);
            return;
        }
        store = iris_orm_store_owner_store(owner);
        check_not_null(store);
        check_true((store->capabilities & IRIS_RECORD_STORE_DURABLE) != 0u);
        check_true((store->capabilities & IRIS_RECORD_STORE_ATOMIC_BATCH) != 0u);

        check_equal(live_scan(store, &snapshot), SALTS_OK);
        check_equal(snapshot.count, (size_t)0u);

        live_put(&mutations[0], key_a[0], IRIS_RECORD_REVISION_ABSENT, 1u,
                 "a1");
        mutations[0].key = key_a;
        live_put(&mutations[1], key_b[0], IRIS_RECORD_REVISION_ABSENT, 1u,
                 "b1");
        mutations[1].key = key_b;
        check_equal(store->commit(store->ctx, mutations, 2u), SALTS_OK);

        check_equal(live_scan(store, &snapshot), SALTS_OK);
        check_equal(snapshot.count, (size_t)2u);
        check_true(snapshot.saw_a);
        check_true(snapshot.saw_b);
        check_equal(snapshot.revision_a, UINT64_C(1));
        check_equal(snapshot.revision_b, UINT64_C(1));
        check_true(live_value_equal(snapshot.value_a, snapshot.value_a_size, "a1"));
        check_true(live_value_equal(snapshot.value_b, snapshot.value_b_size, "b1"));

        live_put(&mutations[0], key_a[0], IRIS_RECORD_REVISION_ABSENT, 2u,
                 "stale");
        mutations[0].key = key_a;
        check_equal(store->commit(store->ctx, mutations, 1u), SALTS_EBUSY);
        check_equal(live_scan(store, &snapshot), SALTS_OK);
        check_equal(snapshot.revision_a, UINT64_C(1));
        check_true(live_value_equal(snapshot.value_a, snapshot.value_a_size, "a1"));

        live_put(&mutations[0], key_a[0], 1u, 2u, "a2");
        mutations[0].key = key_a;
        check_equal(store->commit(store->ctx, mutations, 1u), SALTS_OK);

        live_put(&mutations[0], key_a[0], 2u, 3u, "a3-rolled-back");
        mutations[0].key = key_a;
        live_put(&mutations[1], key_b[0], 99u, 100u, "b-stale");
        mutations[1].key = key_b;
        check_equal(store->commit(store->ctx, mutations, 2u), SALTS_EBUSY);

        check_equal(live_scan(store, &snapshot), SALTS_OK);
        check_equal(snapshot.revision_a, UINT64_C(2));
        check_equal(snapshot.revision_b, UINT64_C(1));
        check_true(live_value_equal(snapshot.value_a, snapshot.value_a_size, "a2"));
        check_true(live_value_equal(snapshot.value_b, snapshot.value_b_size, "b1"));

        live_put(&mutations[0], key_a[0], 2u, 3u, "a3");
        mutations[0].key = key_a;
        live_put(&mutations[1], key_b[0], 1u, 2u, "b2");
        mutations[1].key = key_b;
        check_equal(store->commit(store->ctx, mutations, 2u), SALTS_OK);

        check_equal(live_scan(store, &snapshot), SALTS_OK);
        check_equal(snapshot.revision_a, UINT64_C(3));
        check_equal(snapshot.revision_b, UINT64_C(2));
        check_true(live_value_equal(snapshot.value_a, snapshot.value_a_size, "a3"));
        check_true(live_value_equal(snapshot.value_b, snapshot.value_b_size, "b2"));

        live_delete(&mutations[0], key_a, 3u);
        live_delete(&mutations[1], key_b, 2u);
        check_equal(store->commit(store->ctx, mutations, 2u), SALTS_OK);
        check_equal(live_scan(store, &snapshot), SALTS_OK);
        check_equal(snapshot.count, (size_t)0u);

        iris_orm_store_owner_destroy(owner);
        check_equal(tt_remove_file(yaml_path), 0);
        free(yaml_path);
    }
}
