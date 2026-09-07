#include "iris_orm_store.h"

#include <tinytest.h>
#include <salts_error.h>

#include <stdio.h>
#include <string.h>

typedef struct scan_result_s {
    size_t count;
    uint64_t revisions[4];
    char keys[4][32];
    char values[4][32];
} scan_result_t;

static int collect_record(void *context, const iris_record_view_t *record) {
    scan_result_t *result = (scan_result_t *)context;
    size_t index;
    if (!result || !record || result->count >= 4u ||
        record->key_size >= sizeof(result->keys[0]) ||
        record->value_size >= sizeof(result->values[0])) {
        return SALTS_EINVAL;
    }
    index = result->count++;
    memcpy(result->keys[index], record->key, record->key_size);
    result->keys[index][record->key_size] = '\0';
    memcpy(result->values[index], record->value, record->value_size);
    result->values[index][record->value_size] = '\0';
    result->revisions[index] = record->revision;
    return SALTS_OK;
}

static int write_sqlite_yaml(const char *yaml_path, const char *db_path) {
    FILE *file = fopen(yaml_path, "wb");
    if (!file) return -1;
    (void)fprintf(file,
                  "version: 1\n"
                  "channels:\n"
                  "  iris.test:\n"
                  "    kind: record_store\n"
                  "    config:\n"
                  "      backend: sqlite\n"
                  "      database_path: '%s'\n"
                  "      namespace_name: iris.test\n"
                  "      busy_timeout_ms: 5000\n"
                  "      max_records: 4\n"
                  "      max_key_size: 31\n"
                  "      max_value_size: 31\n"
                  "      max_batch_size: 2\n"
                  "adapters: {}\n",
                  db_path);
    return fclose(file);
}

static int write_redis_yaml(const char *yaml_path) {
    FILE *file = fopen(yaml_path, "wb");
    if (!file) return -1;
    (void)fputs("version: 1\n"
                "channels:\n"
                "  iris.test:\n"
                "    kind: record_store\n"
                "    config:\n"
                "      backend: redis\n"
                "      host: 127.0.0.1\n"
                "      port: 6379\n"
                "adapters: {}\n",
                file);
    return fclose(file);
}

static int write_postgresql_yaml(const char *yaml_path) {
    FILE *file = fopen(yaml_path, "wb");
    if (!file) return -1;
    (void)fputs("version: 1\n"
                "channels:\n"
                "  iris.test:\n"
                "    kind: record_store\n"
                "    config:\n"
                "      backend: postgresql\n"
                "      service: iris\n"
                "      namespace_name: iris.test\n"
                "adapters: {}\n",
                file);
    return fclose(file);
}

static int write_bounded_sqlite_yaml(const char *yaml_path,
                                     const char *db_path) {
    FILE *file = fopen(yaml_path, "wb");
    if (!file) return -1;
    (void)fprintf(file,
                  "version: 1\n"
                  "channels:\n"
                  "  iris.test:\n"
                  "    kind: record_store\n"
                  "    config:\n"
                  "      backend: sqlite\n"
                  "      database_path: '%s'\n"
                  "      namespace_name: iris.test\n"
                  "      max_records: 4\n"
                  "      max_bytes: 8\n"
                  "      max_item_bytes: 5\n"
                  "      max_key_size: 31\n"
                  "      max_value_size: 31\n"
                  "      max_batch_size: 2\n"
                  "adapters: {}\n",
                  db_path);
    return fclose(file);
}

static int write_unknown_sqlite_yaml(const char *yaml_path,
                                     const char *db_path) {
    FILE *file = fopen(yaml_path, "wb");
    if (!file) return -1;
    (void)fprintf(file,
                  "version: 1\n"
                  "channels:\n"
                  "  iris.test:\n"
                  "    kind: record_store\n"
                  "    config:\n"
                  "      backend: sqlite\n"
                  "      database_path: '%s'\n"
                  "      namespace_name: iris.test\n"
                  "      silent_fallback: true\n"
                  "adapters: {}\n",
                  db_path);
    return fclose(file);
}

spec("Iris TurboDB ORM record store") {
    const char *yaml_path = "test-iris-orm-store.yaml";
    const char *redis_yaml_path = "test-iris-orm-store-redis.yaml";
    const char *postgresql_yaml_path = "test-iris-orm-store-postgresql.yaml";
    const char *unknown_yaml_path = "test-iris-orm-store-unknown.yaml";
    const char *db_path = "test-iris-orm-store.sqlite3";

    before_each() {
        (void)remove(yaml_path);
        (void)remove(redis_yaml_path);
        (void)remove(postgresql_yaml_path);
        (void)remove(unknown_yaml_path);
        (void)remove(db_path);
    }

    after_each() {
        (void)remove(yaml_path);
        (void)remove(redis_yaml_path);
        (void)remove(postgresql_yaml_path);
        (void)remove(unknown_yaml_path);
        (void)remove(db_path);
    }

    it("commits a durable atomic batch and restores it after reopen") {
        static const uint8_t key_a[] = "a";
        static const uint8_t key_b[] = "b";
        static const uint8_t value_a[] = "alpha";
        static const uint8_t value_b[] = "beta";
        iris_record_mutation_t mutations[2] = {
            IRIS_RECORD_MUTATION_INIT, IRIS_RECORD_MUTATION_INIT};
        iris_orm_store_owner_t *owner;
        iris_record_store_t *store;
        scan_result_t scan = {0};
        char error[256] = {0};

        check_equal(write_sqlite_yaml(yaml_path, db_path), 0);
        owner = iris_orm_store_owner_create(yaml_path, "iris.test", 1,
                                            error, sizeof(error));
        check_not_null(owner);
        store = iris_orm_store_owner_store(owner);
        check_not_null(store);
        check((store->capabilities & IRIS_RECORD_STORE_DURABLE) != 0u);
        check((store->capabilities & IRIS_RECORD_STORE_ATOMIC_BATCH) != 0u);

        mutations[0].key = key_b;
        mutations[0].key_size = sizeof(key_b) - 1u;
        mutations[0].next_revision = 1u;
        mutations[0].value = value_b;
        mutations[0].value_size = sizeof(value_b) - 1u;
        mutations[1].key = key_a;
        mutations[1].key_size = sizeof(key_a) - 1u;
        mutations[1].next_revision = 1u;
        mutations[1].value = value_a;
        mutations[1].value_size = sizeof(value_a) - 1u;
        check_equal(store->commit(store->ctx, mutations, 2u), SALTS_OK);
        iris_orm_store_owner_destroy(owner);

        owner = iris_orm_store_owner_create(yaml_path, "iris.test", 1,
                                            error, sizeof(error));
        check_not_null(owner);
        store = iris_orm_store_owner_store(owner);
        check_equal(store->scan(store->ctx, collect_record, &scan), SALTS_OK);
        check_equal(scan.count, 2u);
        check_equal(scan.keys[0], "a");
        check_equal(scan.keys[1], "b");
        check_equal(scan.values[0], "alpha");
        check_equal(scan.values[1], "beta");
        iris_orm_store_owner_destroy(owner);
    }

    it("rolls back every mutation when one revision conflicts") {
        static const uint8_t key_a[] = "a";
        static const uint8_t key_b[] = "b";
        static const uint8_t value[] = "value";
        iris_record_mutation_t initial = IRIS_RECORD_MUTATION_INIT;
        iris_record_mutation_t mutations[2] = {
            IRIS_RECORD_MUTATION_INIT, IRIS_RECORD_MUTATION_INIT};
        iris_orm_store_owner_t *owner;
        iris_record_store_t *store;
        scan_result_t scan = {0};
        char error[256] = {0};

        check_equal(write_sqlite_yaml(yaml_path, db_path), 0);
        owner = iris_orm_store_owner_create(yaml_path, "iris.test", 1,
                                            error, sizeof(error));
        check_not_null(owner);
        store = iris_orm_store_owner_store(owner);
        initial.key = key_a;
        initial.key_size = 1u;
        initial.next_revision = 1u;
        initial.value = value;
        initial.value_size = sizeof(value) - 1u;
        check_equal(store->commit(store->ctx, &initial, 1u), SALTS_OK);

        mutations[0] = initial;
        mutations[0].expected_revision = 9u;
        mutations[0].next_revision = 10u;
        mutations[1].key = key_b;
        mutations[1].key_size = 1u;
        mutations[1].next_revision = 1u;
        mutations[1].value = value;
        mutations[1].value_size = sizeof(value) - 1u;
        check_equal(store->commit(store->ctx, mutations, 2u), SALTS_EBUSY);
        check_equal(store->scan(store->ctx, collect_record, &scan), SALTS_OK);
        check_equal(scan.count, 1u);
        check_equal(scan.keys[0], "a");
        check_equal(scan.revisions[0], 1u);
        iris_orm_store_owner_destroy(owner);
    }

    it("rejects Redis because ORM cannot preserve atomic revision CAS") {
        iris_orm_store_owner_t *owner;
        char error[256] = {0};
        check_equal(write_redis_yaml(redis_yaml_path), 0);
        owner = iris_orm_store_owner_create(redis_yaml_path, "iris.test", 0,
                                            error, sizeof(error));
        check_null(owner);
        check_not_null(strstr(error, "Redis"));
        check_not_null(strstr(error, "atomic"));
    }

    it("passes PostgreSQL to the installed TurboDB ORM driver") {
        iris_orm_store_owner_t *owner;
        char error[256] = {0};
        check_equal(write_postgresql_yaml(postgresql_yaml_path), 0);
        owner = iris_orm_store_owner_create(
            postgresql_yaml_path, "iris.test", 0, error, sizeof(error));
        check_null(owner);
        check_not_null(strstr(error, "cannot connect TurboDB ORM"));
        check_null(strstr(error, "not exported by TurboMedia"));
    }

    it("enforces per-item and total byte limits atomically") {
        static const uint8_t key_a[] = "a";
        static const uint8_t key_b[] = "b";
        static const uint8_t value_four[] = "1234";
        static const uint8_t value_five[] = "12345";
        static const uint8_t value_three[] = "123";
        iris_record_mutation_t mutation = IRIS_RECORD_MUTATION_INIT;
        iris_orm_store_owner_t *owner;
        iris_record_store_t *store;
        scan_result_t scan = {0};
        char error[256] = {0};

        check_equal(write_bounded_sqlite_yaml(yaml_path, db_path), 0);
        owner = iris_orm_store_owner_create(yaml_path, "iris.test", 1,
                                            error, sizeof(error));
        check_not_null(owner);
        store = iris_orm_store_owner_store(owner);
        check_not_null(store);

        mutation.key = key_a;
        mutation.key_size = sizeof(key_a) - 1u;
        mutation.next_revision = 1u;
        mutation.value = value_five;
        mutation.value_size = sizeof(value_five) - 1u;
        check_equal(store->commit(store->ctx, &mutation, 1u), SALTS_ENOSPC);

        mutation.value = value_four;
        mutation.value_size = sizeof(value_four) - 1u;
        check_equal(store->commit(store->ctx, &mutation, 1u), SALTS_OK);

        mutation.key = key_b;
        mutation.next_revision = 1u;
        mutation.value = value_three;
        mutation.value_size = sizeof(value_three) - 1u;
        check_equal(store->commit(store->ctx, &mutation, 1u), SALTS_ENOSPC);
        check_equal(store->scan(store->ctx, collect_record, &scan), SALTS_OK);
        check_equal(scan.count, 1u);
        check_equal(scan.keys[0], "a");
        iris_orm_store_owner_destroy(owner);
    }

    it("rejects unknown SQLite ORM configuration fields") {
        iris_orm_store_owner_t *owner;
        char error[256] = {0};
        check_equal(write_unknown_sqlite_yaml(unknown_yaml_path, db_path), 0);
        owner = iris_orm_store_owner_create(
            unknown_yaml_path, "iris.test", 1, error, sizeof(error));
        check_null(owner);
        check_not_null(strstr(error, "unknown"));
    }
}
