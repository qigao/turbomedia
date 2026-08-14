#include "iris_command_ledger.h"

#include <tinytest.h>
#include <turbo_error.h>
#include <turbo_thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_STORE_CAPACITY = 8 };

typedef struct test_record_s {
    uint8_t key[IRIS_COMMAND_ID_CAPACITY];
    size_t key_size;
    uint8_t *value;
    size_t value_size;
    uint64_t revision;
    int used;
} test_record_t;

typedef struct test_store_s {
    turbo_flow_record_store_t api;
    test_record_t records[TEST_STORE_CAPACITY];
    int commit_calls;
    int fail_commit_call;
} test_store_t;

static uint64_t g_now_ms;

static uint64_t test_now_ms(void *context) {
    (void)context;
    return g_now_ms;
}

static int find_record(test_store_t *store, const uint8_t *key,
                       size_t key_size) {
    size_t i;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        if (store->records[i].used &&
            store->records[i].key_size == key_size &&
            memcmp(store->records[i].key, key, key_size) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int test_scan(void *context, turbo_flow_record_visit_fn visit,
                     void *visit_context) {
    test_store_t *store = (test_store_t *)context;
    size_t i;
    if (!store || !visit) return TURBO_EINVAL;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        turbo_flow_record_view_t view = TURBO_FLOW_RECORD_VIEW_INIT;
        int rc;
        if (!store->records[i].used) continue;
        view.key = store->records[i].key;
        view.key_size = store->records[i].key_size;
        view.value = store->records[i].value;
        view.value_size = store->records[i].value_size;
        view.revision = store->records[i].revision;
        rc = visit(visit_context, &view);
        if (rc != TURBO_OK) return rc;
    }
    return TURBO_OK;
}

static int test_commit(void *context,
                       const turbo_flow_record_mutation_t *mutations,
                       size_t mutation_count) {
    test_store_t *store = (test_store_t *)context;
    const turbo_flow_record_mutation_t *mutation;
    int index;
    size_t i;
    uint8_t *copy;
    if (!store || !mutations || mutation_count != 1u) return TURBO_EINVAL;
    store->commit_calls++;
    if (store->fail_commit_call > 0 &&
        store->commit_calls == store->fail_commit_call) {
        return TURBO_EIO;
    }
    mutation = &mutations[0];
    index = find_record(store, mutation->key, mutation->key_size);
    if ((index < 0 && mutation->expected_revision !=
                        TURBO_FLOW_RECORD_REVISION_ABSENT) ||
        (index >= 0 && store->records[index].revision !=
                           mutation->expected_revision)) {
        return TURBO_EBUSY;
    }
    if (mutation->kind == TURBO_FLOW_RECORD_DELETE) {
        if (index < 0) return TURBO_EBUSY;
        free(store->records[index].value);
        memset(&store->records[index], 0, sizeof(store->records[index]));
        return TURBO_OK;
    }
    if (mutation->kind != TURBO_FLOW_RECORD_PUT || !mutation->value ||
        mutation->value_size == 0u ||
        mutation->value_size > store->api.max_value_size ||
        mutation->next_revision <= mutation->expected_revision) {
        return TURBO_EINVAL;
    }
    if (index < 0) {
        for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
            if (!store->records[i].used) {
                index = (int)i;
                break;
            }
        }
        if (index < 0) return TURBO_ENOSPC;
    }
    copy = (uint8_t *)malloc(mutation->value_size);
    if (!copy) return TURBO_ENOMEM;
    memcpy(copy, mutation->value, mutation->value_size);
    free(store->records[index].value);
    memset(&store->records[index], 0, sizeof(store->records[index]));
    memcpy(store->records[index].key, mutation->key, mutation->key_size);
    store->records[index].key_size = mutation->key_size;
    store->records[index].value = copy;
    store->records[index].value_size = mutation->value_size;
    store->records[index].revision = mutation->next_revision;
    store->records[index].used = 1;
    return TURBO_OK;
}

static void test_store_init(test_store_t *store) {
    memset(store, 0, sizeof(*store));
    store->api = (turbo_flow_record_store_t)TURBO_FLOW_RECORD_STORE_INIT;
    store->api.capabilities = TURBO_FLOW_RECORD_STORE_DURABLE |
                              TURBO_FLOW_RECORD_STORE_ATOMIC_BATCH;
    store->api.max_key_size = IRIS_COMMAND_ID_CAPACITY;
    store->api.max_value_size = 16384u;
    store->api.max_batch_size = 1u;
    store->api.max_records = TEST_STORE_CAPACITY;
    store->api.ctx = store;
    store->api.scan = test_scan;
    store->api.commit = test_commit;
    g_now_ms = UINT64_C(10000);
}

static void test_store_clear(test_store_t *store) {
    size_t i;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        free(store->records[i].value);
    }
}

static size_t test_store_count(const test_store_t *store) {
    size_t count = 0u;
    size_t i;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        if (store->records[i].used) count++;
    }
    return count;
}

static iris_command_ledger_t *test_create_ledger(test_store_t *store) {
    iris_command_ledger_config_t config;
    memset(&config, 0, sizeof(config));
    config.request_queue_capacity = 4u;
    config.retention_batch_size = 2u;
    config.terminal_retention_ms = UINT64_C(1000);
    config.retention_sweep_interval_ms = UINT64_C(3600000);
    config.store = &store->api;
    config.realtime_ms = test_now_ms;
    return iris_command_ledger_create(&config);
}

static iris_command_identity_t test_identity(const char *command_id) {
    iris_command_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    snprintf(identity.command_id, sizeof(identity.command_id), "%s",
             command_id);
    memset(identity.semantic_fingerprint, 'a', 64u);
    identity.semantic_fingerprint[64] = '\0';
    snprintf(identity.provider_session_id,
             sizeof(identity.provider_session_id), "session-a");
    snprintf(identity.command_type, sizeof(identity.command_type),
             "conference.create");
    snprintf(identity.resource_id, sizeof(identity.resource_id), "room-a");
    identity.resource_generation = 1u;
    return identity;
}

static iris_command_terminal_outcome_t test_terminal(void) {
    iris_command_terminal_outcome_t outcome;
    memset(&outcome, 0, sizeof(outcome));
    snprintf(outcome.terminal_status, sizeof(outcome.terminal_status),
             "succeeded");
    snprintf(outcome.event_type, sizeof(outcome.event_type),
             "provider.conference.created");
    snprintf(outcome.result_json, sizeof(outcome.result_json),
             "{\"roomId\":\"room-a\"}");
    return outcome;
}

static void normalize_yaml_path(char *path) {
    if (!path) return;
    for (; *path; ++path) {
        if (*path == '\\') *path = '/';
    }
}

spec("Iris durable provider command ledger") {
    it("rejects a volatile RecordStore") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        test_store_init(&store);
        store.api.capabilities = TURBO_FLOW_RECORD_STORE_ATOMIC_BATCH;
        ledger = test_create_ledger(&store);
        check_ptr_eq(ledger, NULL);
        test_store_clear(&store);
    }

    it("claims once and distinguishes duplicate from semantic conflict") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t identity = test_identity("command-a");
        iris_command_identity_t conflict = identity;
        iris_command_claim_result_t result;
        test_store_init(&store);
        ledger = test_create_ledger(&store);
        check_ptr_ne(ledger, NULL);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(result.disposition, IRIS_COMMAND_CLAIM_EXECUTE);
        check_int_eq((int)result.store_revision, 1);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(result.disposition, IRIS_COMMAND_CLAIM_IN_PROGRESS);
        conflict.semantic_fingerprint[0] = 'b';
        check_int_eq(iris_command_ledger_claim(ledger, &conflict, &result),
                     IVR_OK);
        check_int_eq(result.disposition, IRIS_COMMAND_CLAIM_CONFLICT);
        check_int_eq((int)test_store_count(&store), 1);
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("replays a durable accepted provider resource") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t identity = test_identity("command-b");
        iris_command_claim_result_t result;
        test_store_init(&store);
        ledger = test_create_ledger(&store);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(iris_command_ledger_commit_accepted(
                         ledger, &identity, "media-worker-a"),
                     IVR_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(result.disposition,
                     IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED);
        check_str_eq(result.provider_resource_id, "media-worker-a");
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("replays terminal outcome and removes it only after retention") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t identity = test_identity("command-c");
        iris_command_terminal_outcome_t outcome = test_terminal();
        iris_command_claim_result_t result;
        iris_command_retention_result_t retention;
        test_store_init(&store);
        ledger = test_create_ledger(&store);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(iris_command_ledger_commit_terminal(
                         ledger, &identity, &outcome),
                     IVR_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(result.disposition,
                     IRIS_COMMAND_CLAIM_REPLAY_TERMINAL);
        check_str_eq(result.terminal_status, "succeeded");
        check_str_eq(result.event_type, "provider.conference.created");
        check_str_eq(result.result_json, "{\"roomId\":\"room-a\"}");
        check_int_eq(iris_command_ledger_run_retention(ledger, &retention),
                     IVR_OK);
        check_int_eq((int)retention.deleted, 0);
        g_now_ms += UINT64_C(1000);
        check_int_eq(iris_command_ledger_run_retention(ledger, &retention),
                     IVR_OK);
        check_int_eq((int)retention.selected, 1);
        check_int_eq((int)retention.deleted, 1);
        check_int_eq((int)test_store_count(&store), 0);
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("normalizes an interrupted intent to unknown on restart") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t identity = test_identity("command-d");
        iris_command_claim_result_t result;
        iris_command_ledger_stats_t stats;
        test_store_init(&store);
        ledger = test_create_ledger(&store);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        iris_command_ledger_destroy(ledger);
        ledger = test_create_ledger(&store);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(result.disposition,
                     IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN);
        iris_command_ledger_get_stats(ledger, &stats);
        check_int_eq((int)stats.recovered_unknown_total, 1);
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("reconciles unknown only from explicit accepted or terminal evidence") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t accepted =
            test_identity("command-reconcile-accepted");
        iris_command_identity_t terminal =
            test_identity("command-reconcile-terminal");
        iris_command_terminal_outcome_t outcome = test_terminal();
        iris_command_claim_result_t result;
        test_store_init(&store);
        ledger = test_create_ledger(&store);
        check_not_null(ledger);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);

        check_int_eq(iris_command_ledger_claim(ledger, &accepted, &result),
                     IVR_OK);
        check_int_eq(iris_command_ledger_mark_unknown(ledger, &accepted),
                     IVR_OK);
        check_int_eq(iris_command_ledger_commit_accepted(
                         ledger, &accepted, "media-worker-reconciled"),
                     IVR_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &accepted, &result),
                     IVR_OK);
        check_int_eq(result.disposition,
                     IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED);
        check_str_eq(result.provider_resource_id,
                     "media-worker-reconciled");

        check_int_eq(iris_command_ledger_claim(ledger, &terminal, &result),
                     IVR_OK);
        check_int_eq(iris_command_ledger_mark_unknown(ledger, &terminal),
                     IVR_OK);
        check_int_eq(iris_command_ledger_commit_terminal(
                         ledger, &terminal, &outcome),
                     IVR_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &terminal, &result),
                     IVR_OK);
        check_int_eq(result.disposition,
                     IRIS_COMMAND_CLAIM_REPLAY_TERMINAL);
        check_str_eq(result.terminal_status, "succeeded");
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("proves resource history only for the exact session and generation") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t prior = test_identity("command-resource-a");
        iris_command_identity_t query = test_identity("command-resource-b");
        iris_command_identity_t other_generation = query;
        iris_command_identity_t other_session = query;
        iris_command_terminal_outcome_t outcome = test_terminal();
        iris_command_claim_result_t result;
        iris_command_ledger_stats_t stats;
        int seen = -1;
        test_store_init(&store);
        ledger = test_create_ledger(&store);
        check_not_null(ledger);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);

        check_int_eq(iris_command_ledger_claim(ledger, &prior, &result),
                     IVR_OK);
        check_int_eq(iris_command_ledger_resource_seen(
                         ledger, &query, &seen),
                     IVR_OK);
        check_false(seen);
        check_int_eq(iris_command_ledger_commit_terminal(
                         ledger, &prior, &outcome),
                     IVR_OK);
        check_int_eq(iris_command_ledger_resource_seen(
                         ledger, &query, &seen),
                     IVR_OK);
        check_true(seen);

        other_generation.resource_generation = 2u;
        check_int_eq(iris_command_ledger_resource_seen(
                         ledger, &other_generation, &seen),
                     IVR_OK);
        check_false(seen);
        snprintf(other_session.provider_session_id,
                 sizeof(other_session.provider_session_id), "session-b");
        check_int_eq(iris_command_ledger_resource_seen(
                         ledger, &other_session, &seen),
                     IVR_OK);
        check_false(seen);
        iris_command_ledger_get_stats(ledger, &stats);
        check_int_eq((int)stats.resource_queries_total, 4);
        check_int_eq((int)stats.resource_seen_total, 1);
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("does not authorize execution when durable intent commit fails") {
        test_store_t store;
        iris_command_ledger_t *ledger;
        iris_command_identity_t identity = test_identity("command-e");
        iris_command_claim_result_t result;
        iris_command_ledger_stats_t stats;
        test_store_init(&store);
        store.fail_commit_call = 1;
        ledger = test_create_ledger(&store);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_ESTATE);
        check_int_eq((int)test_store_count(&store), 0);
        iris_command_ledger_get_stats(ledger, &stats);
        check_int_eq((int)stats.storage_failures_total, 1);
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("automatically removes expired terminal records in bounded sweeps") {
        test_store_t store;
        iris_command_ledger_config_t config;
        iris_command_ledger_t *ledger;
        iris_command_identity_t identity = test_identity("command-sweep");
        iris_command_terminal_outcome_t outcome = test_terminal();
        iris_command_claim_result_t result;
        iris_command_ledger_stats_t stats;
        int observed = 0;
        test_store_init(&store);
        memset(&config, 0, sizeof(config));
        config.request_queue_capacity = 4u;
        config.retention_batch_size = 1u;
        config.terminal_retention_ms = UINT64_C(100);
        config.retention_sweep_interval_ms = UINT64_C(10);
        config.store = &store.api;
        config.realtime_ms = test_now_ms;
        ledger = iris_command_ledger_create(&config);
        check_not_null(ledger);
        check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
        check_int_eq(iris_command_ledger_claim(ledger, &identity, &result),
                     IVR_OK);
        check_int_eq(iris_command_ledger_commit_terminal(
                         ledger, &identity, &outcome),
                     IVR_OK);
        g_now_ms += UINT64_C(100);
        for (int i = 0; i < 100; ++i) {
            iris_command_ledger_get_stats(ledger, &stats);
            if (stats.retained_deleted_total == 1u) {
                observed = 1;
                break;
            }
            turbo_sleep_ms(5u);
        }
        check_true(observed);
        check_int_eq((int)test_store_count(&store), 0);
        check_true(stats.retention_sweeps_total >= 2u);
        check_int_eq((int)stats.retention_failures_total, 0);
        iris_command_ledger_destroy(ledger);
        test_store_clear(&store);
    }

    it("persists intent accepted and terminal states through SQLite restart") {
        char *database_path = tt_make_temp_file("iris-ledger", ".sqlite3");
        char *yaml_path = tt_make_temp_file("iris-ledger", ".yaml");
        char yaml[2048];
        char error[256] = {0};
        iris_command_ledger_t *ledger = NULL;
        iris_command_identity_t intent = test_identity("command-sql-intent");
        iris_command_identity_t accepted =
            test_identity("command-sql-accepted");
        iris_command_identity_t terminal =
            test_identity("command-sql-terminal");
        iris_command_terminal_outcome_t outcome = test_terminal();
        iris_command_claim_result_t result;

        check_not_null(database_path);
        check_not_null(yaml_path);
        if (database_path && yaml_path) {
            normalize_yaml_path(database_path);
            normalize_yaml_path(yaml_path);
            snprintf(yaml, sizeof(yaml),
                     "version: 1\n"
                     "channels:\n"
                     "  iris.provider_commands:\n"
                     "    kind: record_store\n"
                     "    config:\n"
                     "      backend: sqlite\n"
                     "      database_path: '%s'\n"
                     "      namespace_name: iris.provider_commands\n"
                     "      busy_timeout_ms: 1000\n"
                     "      max_records: 8\n"
                     "      max_bytes: 1048576\n"
                     "      max_item_bytes: 32768\n"
                     "      max_key_size: 128\n"
                     "      max_value_size: 16384\n"
                     "      max_batch_size: 2\n"
                     "adapters: {}\n",
                     database_path);
            check_int_eq(tt_write_file(yaml_path, yaml, strlen(yaml)), 0);
            ledger = iris_command_ledger_create_flowstore(
                yaml_path, "iris.provider_commands", 0, 4u, 2u,
                UINT64_C(86400000), UINT64_C(1000), error, sizeof(error));
            check_null(ledger);
            check_str_contains(error, "development opt-in");
            memset(error, 0, sizeof(error));
            ledger = iris_command_ledger_create_flowstore(
                yaml_path, "iris.provider_commands", 1, 4u, 2u,
                UINT64_C(86400000), UINT64_C(1000), error, sizeof(error));
            check_not_null(ledger);
        }
        if (ledger) {
            check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
            check_int_eq(iris_command_ledger_claim(ledger, &intent, &result),
                         IVR_OK);
            check_int_eq(iris_command_ledger_claim(
                             ledger, &accepted, &result),
                         IVR_OK);
            check_int_eq(iris_command_ledger_commit_accepted(
                             ledger, &accepted, "media-worker-sql"),
                         IVR_OK);
            check_int_eq(iris_command_ledger_claim(
                             ledger, &terminal, &result),
                         IVR_OK);
            check_int_eq(iris_command_ledger_commit_terminal(
                             ledger, &terminal, &outcome),
                         IVR_OK);
            iris_command_ledger_destroy(ledger);
            ledger = iris_command_ledger_create_flowstore(
                yaml_path, "iris.provider_commands", 1, 4u, 2u,
                UINT64_C(86400000), UINT64_C(1000), error, sizeof(error));
            check_not_null(ledger);
        }
        if (ledger) {
            check_int_eq(iris_command_ledger_start(ledger), TURBO_OK);
            check_int_eq(iris_command_ledger_claim(ledger, &intent, &result),
                         IVR_OK);
            check_int_eq(result.disposition,
                         IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN);
            check_int_eq(iris_command_ledger_claim(
                             ledger, &accepted, &result),
                         IVR_OK);
            check_int_eq(result.disposition,
                         IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED);
            check_str_eq(result.provider_resource_id, "media-worker-sql");
            check_int_eq(iris_command_ledger_claim(
                             ledger, &terminal, &result),
                         IVR_OK);
            check_int_eq(result.disposition,
                         IRIS_COMMAND_CLAIM_REPLAY_TERMINAL);
            check_str_eq(result.result_json, "{\"roomId\":\"room-a\"}");
            iris_command_ledger_destroy(ledger);
        }
        if (yaml_path) check_int_eq(tt_remove_file(yaml_path), 0);
        if (database_path) check_int_eq(tt_remove_file(database_path), 0);
        free(yaml_path);
        free(database_path);
    }
}
