/* test_ivr_fmq_adapter.c - RoomService real-aggregate IVR FMQ adapter.
 * Two layers:
 *  1. Pure apply over the authoritative turbo_room_service_t (no FlowMQ):
 *     conference.join adds a CUSTOMER participant, idempotent replay keeps the
 *     same sequence, role conflict/unknown role fail fast, leave removes the
 *     participant, get_snapshot reports the authoritative version.
 *  2. Live DEALER -> ROUTER round trip over the real aggregate: a join is
 *     applied to the room, message_id retry replays, stale expected version is
 *     rejected (IVR_EVERSION) without mutating the room. */
#include "ivr_fmq_adapter.h"
#include "ivr_flowmq_gateway.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "platform.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest_compat.h"
#include <stdatomic.h>
#include <string.h>

#define TEST_FMQ_PORT 17715

static DataBind *g_codec = NULL;

/* ------------------------------------------------------------------ */
/* decoded result helpers (DEALER reply frames)                        */
/* ------------------------------------------------------------------ */

static uint8_t g_reply[8192];
static size_t g_reply_len = 0;
static uint8_t g_command[8192];
static size_t g_command_len = 0;
static ivr_mutex_t g_reply_lock;
static int g_reply_ready = 0;
static int g_command_ready = 0;

static void on_reply_cb(void *ctx, const uint8_t *frame, size_t len) {
    ivr_frame_info_t info;
    (void)ctx;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        return;
    }
    ivr_mutex_lock(&g_reply_lock);
    if (info.kind == IVR_KIND_RESULT && len <= sizeof(g_reply)) {
        memcpy(g_reply, frame, len);
        g_reply_len = len;
        g_reply_ready = 1;
    } else if (info.kind == IVR_KIND_COMMAND && len <= sizeof(g_command)) {
        memcpy(g_command, frame, len);
        g_command_len = len;
        g_command_ready = 1;
    }
    ivr_mutex_unlock(&g_reply_lock);
}

static int wait_reply(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_mutex_lock(&g_reply_lock);
        int ready = g_reply_ready;
        ivr_mutex_unlock(&g_reply_lock);
        if (ready) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

static int wait_command(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_mutex_lock(&g_reply_lock);
        int ready = g_command_ready;
        ivr_mutex_unlock(&g_reply_lock);
        if (ready) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

static uint64_t reply_u64(const char *field) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "IvrCommandResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return UINT64_MAX;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    uint64_t out = v ? data_bind_value_as_uint64(v) : UINT64_MAX;
    data_bind_object_free(obj);
    return out;
}

static int reply_i32(const char *field) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "IvrCommandResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return INT32_MAX;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    int out = v ? data_bind_value_as_int(v) : INT32_MAX;
    data_bind_object_free(obj);
    return out;
}

static int worker_reply_i32(const char *field) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "WorkerSyncResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return INT32_MAX;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    int out = v ? data_bind_value_as_int(v) : INT32_MAX;
    data_bind_object_free(obj);
    return out;
}

/* ------------------------------------------------------------------ */
/* command / aggregate helpers                                         */
/* ------------------------------------------------------------------ */

static void make_command(ivr_room_command_t *cmd, const char *command,
                         const char *call_id, uint64_t generation,
                         const char *participant_role) {
    memset(cmd, 0, sizeof(*cmd));
    snprintf(cmd->message_id, sizeof(cmd->message_id), "mid-%s", call_id);
    snprintf(cmd->worker_id, sizeof(cmd->worker_id), "ivr-worker-test");
    snprintf(cmd->room_id, sizeof(cmd->room_id), "%s", "room-42");
    snprintf(cmd->call_id, sizeof(cmd->call_id), "%s", call_id);
    cmd->call_generation = generation;
    cmd->expected_room_version = 1;
    snprintf(cmd->command, sizeof(cmd->command), "%s", command);
    snprintf(cmd->participant_role, sizeof(cmd->participant_role), "%s",
             participant_role);
}

static turbo_room_service_t *make_service_with_room(void) {
    turbo_room_service_t *service = turbo_room_service_create();
    if (!service) {
        return NULL;
    }
    turbo_room_config_t room_config;
    memset(&room_config, 0, sizeof(room_config));
    room_config.room_id = "room-42";
    room_config.room_type = TURBO_ROOM_TYPE_CONFERENCE;
    if (turbo_room_service_create_room(service, &room_config) != 0) {
        turbo_room_service_destroy(service);
        return NULL;
    }
    return service;
}

static int participant_role_of(turbo_room_service_t *service,
                               const char *call_id,
                               turbo_participant_role_t *role) {
    turbo_room_participant_summary_t summary;
    if (turbo_room_service_get_participant_summary(service, "room-42",
                                                   call_id, &summary) != 0) {
        return -1;
    }
    *role = summary.role;
    return 0;
}

/* ------------------------------------------------------------------ */
/* pure apply (no FlowMQ peer)                                         */
/* ------------------------------------------------------------------ */

void test_apply_join_leave_snapshot(void) {
    turbo_room_service_t *service = make_service_with_room();
    TEST_ASSERT_NOT_NULL(service);
    ivr_fmq_adapter_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_port = 0; /* host logic only */
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(service, &cfg, &adapter));
    TEST_ASSERT_NOT_NULL(adapter);

    ivr_room_command_t cmd;
    ivr_room_command_result_t result;

    /* conference.join adds a CUSTOMER participant; room created at version 1
       so the first join lands at version 2 with per-call sequence 1. */
    make_command(&cmd, "conference.join", "call-42", 1, "caller");
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_EQUAL_UINT64(2u, result.room_version);
    TEST_ASSERT_EQUAL_UINT64(1u, result.sequence);
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;
    TEST_ASSERT_EQUAL_INT(0, participant_role_of(service, "call-42", &role));
    TEST_ASSERT_EQUAL_INT(TURBO_PARTICIPANT_ROLE_CUSTOMER, role);

    /* idempotent replay: same participant/role, no state change, same seq */
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_EQUAL_UINT64(2u, result.room_version);
    TEST_ASSERT_EQUAL_UINT64(1u, result.sequence);

    /* role conflict on an existing participant fails fast */
    make_command(&cmd, "conference.join", "call-42", 1, "agent");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, result.status_code);

    /* unknown role is rejected, never guessed */
    make_command(&cmd, "conference.join", "call-99", 1, "hacker");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EINVAL, result.status_code);
    TEST_ASSERT_EQUAL_INT(-1, participant_role_of(service, "call-99", &role));

    /* get_snapshot is a read-only version/sequence report */
    make_command(&cmd, "get_snapshot", "call-42", 1, "");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_EQUAL_UINT64(2u, result.room_version);
    TEST_ASSERT_EQUAL_UINT64(1u, result.sequence);

    /* conference.leave removes the participant and advances the sequence */
    make_command(&cmd, "conference.leave", "call-42", 1, "");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_EQUAL_UINT64(2u, result.sequence);
    TEST_ASSERT_EQUAL_INT(-1, participant_role_of(service, "call-42", &role));

    /* leave of a participant that is not there fails fast */
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, result.status_code);

    /* unknown command is rejected */
    make_command(&cmd, "conference.transfer", "call-42", 1, "");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EINVAL, result.status_code);

    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

void test_apply_room_missing(void) {
    turbo_room_service_t *service = turbo_room_service_create();
    TEST_ASSERT_NOT_NULL(service);
    ivr_fmq_adapter_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(service, &cfg, &adapter));

    ivr_room_command_t cmd;
    ivr_room_command_result_t result;
    make_command(&cmd, "conference.join", "call-42", 1, "caller");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, result.status_code);

    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

void test_apply_worker_sync(void) {
    turbo_room_service_t *service = turbo_room_service_create();
    TEST_ASSERT_NOT_NULL(service);
    ivr_fmq_adapter_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_port = 0;
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(service, &cfg, &adapter));
    TEST_ASSERT_NOT_NULL(adapter);

    ivr_room_command_t cmd;
    ivr_room_command_result_t result;
    make_command(&cmd, "worker.sync", "call-42", 1, "");
    snprintf(cmd.worker_id, sizeof(cmd.worker_id), "ivr-worker-01");

    /* first registration records the worker */
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(adapter,
                                                       "ivr-worker-01"));

    /* re-registration is idempotent */
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(adapter,
                                                       "ivr-worker-01"));

    /* empty worker_id is rejected, never guessed */
    cmd.worker_id[0] = '\0';
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EINVAL, result.status_code);
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(adapter, ""));

    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

static uint64_t g_fake_now_ms;

static uint64_t fake_now_ms(void *context) {
    (void)context;
    return g_fake_now_ms;
}

static void make_worker_control(ivr_room_command_t *command,
                                const char *command_name,
                                const char *worker_id,
                                const char *instance_id,
                                uint64_t generation) {
    memset(command, 0, sizeof(*command));
    snprintf(command->message_id, sizeof(command->message_id), "status-%s",
             worker_id);
    snprintf(command->worker_id, sizeof(command->worker_id), "%s", worker_id);
    snprintf(command->instance_id, sizeof(command->instance_id), "%s",
             instance_id);
    snprintf(command->command, sizeof(command->command), "%s", command_name);
    command->connection_generation = generation;
    command->max_sessions = 2;
    command->lease_duration_ms = 300;
    snprintf(command->capabilities, sizeof(command->capabilities), "%s",
             "turboxml,flowmq,health.ready");
}

void test_worker_v2_lease_heartbeat_and_stale_generation(void) {
    turbo_room_service_t *service = turbo_room_service_create();
    TEST_ASSERT_NOT_NULL(service);
    ivr_fmq_adapter_config_t config;
    memset(&config, 0, sizeof(config));
    config.worker_capacity = 1;
    config.worker_lease_ms = 300;
    config.clock.now_ms = fake_now_ms;
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_create(service, &config,
                                                      &adapter));

    ivr_room_command_t command;
    ivr_room_command_result_t result;
    g_fake_now_ms = 10;
    make_worker_control(&command, "worker.sync.v2", "worker-a", "instance-a",
                        7);
    command.health_generation = 1;
    command.health_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);

    ivr_fmq_worker_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(adapter, "worker-a",
                                                         &snapshot));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_SYNCED, snapshot.state);
    TEST_ASSERT_EQUAL_UINT64(1u, snapshot.health_generation);
    TEST_ASSERT_FALSE(snapshot.health_ready);
    TEST_ASSERT_EQUAL_UINT64(310u, snapshot.lease_expires_at_ms);
    TEST_ASSERT_EQUAL_UINT32(2u, snapshot.max_sessions);

    g_fake_now_ms = 100;
    snprintf(command.command, sizeof(command.command), "%s",
             "worker.heartbeat");
    command.health_generation = 2;
    command.health_ready = 1;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(adapter, "worker-a",
                                                         &snapshot));
    TEST_ASSERT_EQUAL_UINT64(400u, snapshot.lease_expires_at_ms);
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_READY, snapshot.state);
    TEST_ASSERT_EQUAL_UINT64(2u, snapshot.health_generation);
    TEST_ASSERT_TRUE(snapshot.health_ready);

    command.connection_generation = 6;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EVERSION, result.status_code);
    command.connection_generation = 7;

    g_fake_now_ms = 401;
    ivr_fmq_adapter_poll(adapter);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(adapter, "worker-a",
                                                         &snapshot));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_EXPIRED, snapshot.state);
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(adapter, "worker-a"));
    ivr_fmq_adapter_stats_t stats;
    ivr_fmq_adapter_get_stats(adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(1u, stats.lease_expired_total);
    ivr_fmq_adapter_poll(adapter);
    ivr_fmq_adapter_get_stats(adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(1u, stats.lease_expired_total);

    g_fake_now_ms = 450;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    g_fake_now_ms = 751;
    ivr_fmq_adapter_poll(adapter);
    ivr_fmq_adapter_get_stats(adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(2u, stats.lease_expired_total);

    make_worker_control(&command, "worker.sync.v2", "worker-b", "instance-b",
                        1);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_worker(
                                      adapter, "worker-a", &snapshot));
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(adapter, "worker-b"));

    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

void test_fresh_registry_requires_fenced_reconcile_for_active_worker(void) {
    turbo_room_service_t *service = turbo_room_service_create();
    TEST_ASSERT_NOT_NULL(service);
    ivr_fmq_adapter_config_t config;
    memset(&config, 0, sizeof(config));
    config.worker_lease_ms = 300;
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_create(service, &config,
                                                      &adapter));

    ivr_room_command_t command;
    ivr_room_command_result_t result;
    ivr_fmq_worker_snapshot_t worker;
    ivr_fmq_worker_snapshot_t workers[1];
    ivr_worker_inventory_record_t record;
    ivr_media_command_t media_command;
    iris_resource_observation_t observation;
    uint32_t count = 99;
    uint32_t total = 99;
    make_worker_control(&command, "worker.sync.v2", "worker-restart",
                        "instance-restart", 4);
    command.active_sessions = 1;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                      adapter, "worker-restart", &worker));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_RECONCILING, worker.state);
    TEST_ASSERT_TRUE(worker.requires_reconcile);
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(
        adapter, "worker-restart"));

    TEST_ASSERT_EQUAL(IVR_ENOSPC, ivr_fmq_adapter_list_workers(
                                         adapter, NULL, 0, &count, &total));
    TEST_ASSERT_EQUAL_UINT32(0, count);
    TEST_ASSERT_EQUAL_UINT32(1, total);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_list_workers(
                                      adapter, workers, 1, &count, &total));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_RECONCILING, workers[0].state);

    memset(&media_command, 0, sizeof(media_command));
    media_command.kind = IVR_MEDIA_COMMAND_CANCEL;
    snprintf(media_command.message_id, sizeof(media_command.message_id),
             "cancel-before-inventory");
    snprintf(media_command.tenant_id, sizeof(media_command.tenant_id),
             "tenant-a");
    snprintf(media_command.provider_session_id,
             sizeof(media_command.provider_session_id), "session-restart");
    snprintf(media_command.dialog_id, sizeof(media_command.dialog_id),
             "dialog-restart");
    snprintf(media_command.room_id, sizeof(media_command.room_id),
             "room-restart");
    snprintf(media_command.call_id, sizeof(media_command.call_id),
             "call-restart");
    media_command.call_generation = 3;
    media_command.operation_generation = 8;
    snprintf(media_command.input_id, sizeof(media_command.input_id),
             "input-restart");
    media_command.input_generation = 2;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_observe_media_command(
                                  adapter, &media_command, &observation));
    TEST_ASSERT_EQUAL_INT(IRIS_RESOURCE_OBSERVATION_UNKNOWN,
                          observation.state);

    memset(&record, 0, sizeof(record));
    snprintf(record.worker_id, sizeof(record.worker_id), "worker-restart");
    snprintf(record.worker_instance_id, sizeof(record.worker_instance_id),
             "instance-restart");
    record.worker_epoch = 4;
    snprintf(record.tenant_id, sizeof(record.tenant_id), "tenant-a");
    snprintf(record.provider_session_id, sizeof(record.provider_session_id),
             "session-restart");
    snprintf(record.dialog_id, sizeof(record.dialog_id), "dialog-restart");
    snprintf(record.room_id, sizeof(record.room_id), "room-restart");
    snprintf(record.call_id, sizeof(record.call_id), "call-restart");
    record.call_generation = 3;
    record.operation_generation = 7;
    record.state = IVR_WORKER_RESOURCE_ACTIVE;
    record.rebindable = 1;
    TEST_ASSERT_EQUAL(IVR_EVERSION,
                      ivr_fmq_adapter_complete_worker_reconcile(
                          adapter, "worker-restart", "wrong-instance", 4));
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_fmq_adapter_complete_worker_reconcile(
                          adapter, "worker-restart", "instance-restart", 4));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_rebind_dialog(adapter, &record));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_rebind_dialog(adapter, &record));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_complete_worker_reconcile(
                          adapter, "worker-restart", "instance-restart", 4));
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(
        adapter, "worker-restart"));

    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

/* ------------------------------------------------------------------ */
/* live DEALER -> ROUTER over the real aggregate                       */
/* ------------------------------------------------------------------ */

static ivr_room_bridge_t *g_bridge = NULL;
static ivr_flowmq_gateway_t *g_gateway = NULL;
static ivr_command_gateway_ops_t g_ops;
static turbo_room_service_t *g_service = NULL;
static ivr_fmq_adapter_t *g_adapter = NULL;

void test_v2_worker_without_active_health_capability_is_not_ready(void) {
    ivr_room_command_t command;
    ivr_room_command_result_t result;
    ivr_fmq_worker_snapshot_t snapshot;

    make_worker_control(&command, "worker.sync.v2", "worker-unready",
                        "instance-unready", 1);
    command.lease_duration_ms = 15000;
    snprintf(command.capabilities, sizeof(command.capabilities),
             "turboxml,flowmq,tts,asr,health.shadow");
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_apply(g_adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_get_worker(g_adapter, "worker-unready",
                                                 &snapshot));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_SYNCED, snapshot.state);
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                        "worker-unready"));
}
static ivr_status_t g_prepare_result = IVR_OK;
static int g_prepare_calls = 0;
static int g_release_calls = 0;
static atomic_int g_media_result_calls;
static atomic_int g_media_event_calls;
static atomic_int g_media_event_result;
static atomic_int g_inventory_page_calls;
static atomic_uint_fast64_t g_live_clock_offset_ms;
static ivr_media_event_t g_last_media_event;
static ivr_worker_inventory_envelope_t g_last_inventory_page;

static uint64_t live_now_ms(void *context) {
    (void)context;
    return turbo_monotonic_ms() +
           atomic_load(&g_live_clock_offset_ms);
}

static ivr_status_t test_observe_media_result(
    void *context, const ivr_media_command_result_t *result) {
    (void)context;
    (void)result;
    atomic_fetch_add(&g_media_result_calls, 1);
    return IVR_OK;
}

static ivr_status_t test_observe_media_event(
    void *context, const ivr_media_event_t *event) {
    (void)context;
    g_last_media_event = *event;
    atomic_fetch_add(&g_media_event_calls, 1);
    return (ivr_status_t)atomic_load(&g_media_event_result);
}

static ivr_status_t test_observe_inventory_page(
    void *context, const ivr_worker_inventory_envelope_t *result) {
    (void)context;
    g_last_inventory_page = *result;
    atomic_fetch_add(&g_inventory_page_calls, 1);
    return IVR_OK;
}

static ivr_status_t test_prepare_caller_audio(
    void *context, const char *room_id, const char *call_id, char *error,
    size_t error_capacity) {
    (void)context;
    (void)room_id;
    (void)call_id;
    g_prepare_calls++;
    if (g_prepare_result != IVR_OK && error && error_capacity > 0) {
        snprintf(error, error_capacity, "injected media preparation failure");
    }
    return g_prepare_result;
}

static ivr_status_t test_release_caller_audio(
    void *context, const char *room_id, const char *call_id, char *error,
    size_t error_capacity) {
    (void)context;
    (void)room_id;
    (void)call_id;
    (void)error;
    (void)error_capacity;
    g_release_calls++;
    return IVR_OK;
}

static int sync_worker(const char *message_prefix) {
    for (int attempt = 0; attempt < 20; attempt++) {
        char message_id[64];
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindObject *obj = NULL;
        snprintf(message_id, sizeof(message_id), "%s-%d", message_prefix,
                 attempt);
        ivr_mutex_lock(&g_reply_lock);
        g_reply_ready = 0;
        ivr_mutex_unlock(&g_reply_lock);
        if (ivr_flowmq_gateway_send_worker_sync(g_gateway, message_id) !=
                IVR_OK ||
            !wait_reply(8000) ||
            data_bind_object_from_bin(
                g_codec, "WorkerSyncResultV1",
                g_reply + IVR_FRAME_HEADER_SIZE,
                g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                &err) != DATA_BIND_OK) {
            continue;
        }
        const DataBindValue *root = data_bind_object_value(obj);
        const DataBindValue *status =
            data_bind_value_get(root, "status_code");
        int synced = status && data_bind_value_as_int(status) == 0;
        data_bind_object_free(obj);
        if (synced) {
            return 1;
        }
    }
    return 0;
}

static int sync_media_worker_v2(const char *message_id) {
    ivr_worker_status_view_t status;
    memset(&status, 0, sizeof(status));
    status.instance_id = "media-instance";
    status.connection_generation = 1;
    status.max_sessions = 2;
    status.lease_duration_ms = 15000;
    status.capabilities = "flowmq,ivr,dispatch-v2,health.ready";
    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    return ivr_flowmq_gateway_send_worker_sync_v2(
               g_gateway, message_id, &status) == IVR_OK &&
           wait_reply(8000) && worker_reply_i32("status_code") == IVR_OK;
}

static ivr_status_t send_join(const char *message_id, uint64_t expected_version) {
    static ivr_bytes_view_t type = {"conference.join", 15};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    ivr_command_view_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.message_id.data = message_id;
    cmd.message_id.size = strlen(message_id);
    cmd.command_type = type;
    cmd.call.room_id = room;
    cmd.call.call_id = call;
    cmd.call.call_generation = 1;
    cmd.call.expected_room_version = expected_version;
    cmd.args_json = args;
    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    return g_ops.submit_copy(g_ops.context, &cmd);
}

void setUp(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    TEST_ASSERT_EQUAL(DATA_BIND_OK, TurboMediaIvrV1_codec_create(&g_codec, &err));
    ivr_mutex_init(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    g_command_ready = 0;
    g_command_len = 0;

    g_service = make_service_with_room();
    TEST_ASSERT_NOT_NULL(g_service);
    ivr_fmq_adapter_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_host = "127.0.0.1";
    cfg.bind_port = TEST_FMQ_PORT;
    cfg.pub_port = 0;
    cfg.timeout_ms = 5000;
    cfg.queue_capacity = 8;
    cfg.dedup_capacity = 8;
    cfg.worker_capacity = 1;
    cfg.dispatch_deadline_ms = 500;
    cfg.clock.now_ms = live_now_ms;
    cfg.media.prepare_caller_audio = test_prepare_caller_audio;
    cfg.media.release_caller_audio = test_release_caller_audio;
    cfg.media_observer.on_media_result = test_observe_media_result;
    cfg.media_observer.on_media_event = test_observe_media_event;
    cfg.inventory_observer.on_inventory_page =
        test_observe_inventory_page;
    g_prepare_result = IVR_OK;
    g_prepare_calls = 0;
    g_release_calls = 0;
    atomic_store(&g_media_result_calls, 0);
    atomic_store(&g_media_event_calls, 0);
    atomic_store(&g_media_event_result, IVR_OK);
    atomic_store(&g_inventory_page_calls, 0);
    atomic_store(&g_live_clock_offset_ms, 0);
    memset(&g_last_media_event, 0, sizeof(g_last_media_event));
    memset(&g_last_inventory_page, 0, sizeof(g_last_inventory_page));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(g_service, &cfg, &g_adapter));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_start(g_adapter));

    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = "ivr-worker-test";
    gcfg.host = "127.0.0.1";
    gcfg.port = TEST_FMQ_PORT;
    gcfg.timeout_ms = 5000;
    gcfg.on_reply = on_reply_cb;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(&gcfg, &g_ops,
                                                        &g_gateway));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(g_gateway));
    ivr_thread_sleep_ms(800); /* let the DEALER connect to the ROUTER */
}

void tearDown(void) {
    if (g_gateway) {
        ivr_flowmq_gateway_destroy(g_gateway);
        g_gateway = NULL;
    }
    if (g_adapter) {
        ivr_fmq_adapter_stop(g_adapter);
        ivr_fmq_adapter_destroy(g_adapter);
        g_adapter = NULL;
    }
    if (g_service) {
        turbo_room_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_codec) {
        data_bind_free(g_codec);
        g_codec = NULL;
    }
    ivr_mutex_destroy(&g_reply_lock);
}

static void make_live_media_command(ivr_media_command_t *command,
                                    ivr_media_command_kind_t kind,
                                    const char *message_id,
                                    uint64_t operation_generation) {
    memset(command, 0, sizeof(*command));
    command->kind = kind;
    snprintf(command->message_id, sizeof(command->message_id), "%s",
             message_id);
    snprintf(command->tenant_id, sizeof(command->tenant_id), "tenant-a");
    snprintf(command->provider_session_id,
             sizeof(command->provider_session_id),
             "iris-session-input-fence");
    snprintf(command->dialog_id, sizeof(command->dialog_id),
             "dialog-input-fence");
    snprintf(command->room_id, sizeof(command->room_id), "room-42");
    snprintf(command->call_id, sizeof(command->call_id), "call-42");
    command->call_generation = 1;
    command->operation_generation = operation_generation;
    command->deadline_timeout_ms = 5000;
}

static ivr_status_t send_and_decode_media_command(
    ivr_media_command_t *command, ivr_media_command_t *received) {
    char worker_id[128];
    ivr_status_t status;
    ivr_mutex_lock(&g_reply_lock);
    g_command_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    status = ivr_fmq_adapter_send_media_command(
        g_adapter, command, worker_id, sizeof(worker_id));
    if (status != IVR_OK) return status;
    if (!wait_command(8000)) return IVR_ESTATE;
    return ivr_flowmq_gateway_decode_media_command(
        g_codec, g_command, g_command_len, received);
}

void test_live_join_reply_idempotent_stale(void) {
    TEST_ASSERT_TRUE(sync_worker("ws-join"));
    /* fresh room is version 1; join with the correct expected version */
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-live-1", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_UINT64(2u, reply_u64("room_version"));
    TEST_ASSERT_EQUAL_UINT64(1u, reply_u64("sequence"));
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;
    TEST_ASSERT_EQUAL_INT(0, participant_role_of(g_service, "call-42", &role));
    TEST_ASSERT_EQUAL_INT(TURBO_PARTICIPANT_ROLE_CUSTOMER, role);

    /* same message_id retransmitted: dedup replays, never re-applies */
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-live-1", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_UINT64(2u, reply_u64("room_version"));
    TEST_ASSERT_EQUAL_UINT64(1u, reply_u64("sequence"));

    /* stale expected version: rejected without mutating the room */
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-live-2", 99));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_EVERSION, reply_i32("status_code"));
    turbo_room_summary_t summary;
    TEST_ASSERT_EQUAL_INT(0, turbo_room_service_get_room_summary(
                                 g_service, "room-42", &summary));
    TEST_ASSERT_EQUAL_UINT64(2u, (uint64_t)summary.version);
}

void test_dialog_start_creates_media_route_for_following_commands(void) {
    ivr_media_command_t command;
    ivr_media_command_t received;
    ivr_fmq_worker_snapshot_t worker;
    char worker_id[128];

    TEST_ASSERT_TRUE(sync_media_worker_v2("ws-media-route"));

    memset(&command, 0, sizeof(command));
    command.kind = IVR_MEDIA_COMMAND_SESSION_OPEN;
    snprintf(command.message_id, sizeof(command.message_id),
             "iris-command-media-route");
    snprintf(command.tenant_id, sizeof(command.tenant_id), "tenant-a");
    snprintf(command.provider_session_id, sizeof(command.provider_session_id),
             "iris-session-media-route");
    snprintf(command.dialog_id, sizeof(command.dialog_id),
             "dialog-media-route");
    snprintf(command.room_id, sizeof(command.room_id), "room-42");
    snprintf(command.call_id, sizeof(command.call_id), "call-42");
    command.call_generation = 1;
    command.operation_generation = 7;
    command.deadline_timeout_ms = 5000;

    ivr_mutex_lock(&g_reply_lock);
    g_command_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_send_media_command(
                                  g_adapter, &command, worker_id,
                                  sizeof(worker_id)));
    TEST_ASSERT_EQUAL_STRING("ivr-worker-test", worker_id);
    TEST_ASSERT_EQUAL_STRING("", command.worker_id);
    TEST_ASSERT_TRUE(wait_command(8000));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_decode_media_command(
                                  g_codec, g_command, g_command_len, &received));
    TEST_ASSERT_EQUAL_STRING("ivr-worker-test", received.worker_id);
    TEST_ASSERT_EQUAL_STRING(command.message_id, received.message_id);
    TEST_ASSERT_EQUAL_UINT64(command.operation_generation,
                             received.operation_generation);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_media_result(
                                  g_gateway, &received, IVR_OK, "", ""));
    for (int i = 0; i < 200 && atomic_load(&g_media_result_calls) == 0; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_media_result_calls));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(1u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);

    command.kind = IVR_MEDIA_COMMAND_PLAY;
    snprintf(command.message_id, sizeof(command.message_id),
             "iris-command-media-play");
    command.operation_generation = 8;
    snprintf(command.text, sizeof(command.text), "Welcome");
    ivr_mutex_lock(&g_reply_lock);
    g_command_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_send_media_command(
                                  g_adapter, &command, worker_id,
                                  sizeof(worker_id)));
    TEST_ASSERT_TRUE(wait_command(8000));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_decode_media_command(
                                  g_codec, g_command, g_command_len, &received));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_COMMAND_PLAY, received.kind);
    TEST_ASSERT_EQUAL_STRING("dialog-media-route", received.dialog_id);

    snprintf(command.worker_id, sizeof(command.worker_id), "spoofed-worker");
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_fmq_adapter_send_media_command(
                                     g_adapter, &command, worker_id,
                                     sizeof(worker_id)));
    command.worker_id[0] = '\0';
    snprintf(command.call_id, sizeof(command.call_id), "unknown-call");
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_fmq_adapter_send_media_command(
                                     g_adapter, &command, worker_id,
                                     sizeof(worker_id)));

    snprintf(command.call_id, sizeof(command.call_id), "call-42");
    command.kind = IVR_MEDIA_COMMAND_SESSION_CLOSE;
    snprintf(command.message_id, sizeof(command.message_id),
             "iris-command-media-close");
    command.operation_generation = 9;
    command.text[0] = '\0';
    snprintf(command.reason, sizeof(command.reason), "completed");
    ivr_mutex_lock(&g_reply_lock);
    g_command_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_send_media_command(
                                  g_adapter, &command, worker_id,
                                  sizeof(worker_id)));
    TEST_ASSERT_TRUE(wait_command(8000));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_decode_media_command(
                                  g_codec, g_command, g_command_len, &received));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_COMMAND_SESSION_CLOSE, received.kind);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_media_result(
                                  g_gateway, &received, IVR_OK, "", ""));
    for (int i = 0;
         i < 200 && atomic_load(&g_media_result_calls) < 2; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&g_media_result_calls));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    ivr_fmq_adapter_stats_t stats;
    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.dialogs);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.dialog_high_water);
}

void test_media_cancel_tracks_the_exact_active_input(void) {
    ivr_media_command_t command;
    ivr_media_command_t received;
    iris_resource_observation_t observation;

    TEST_ASSERT_TRUE(sync_media_worker_v2("ws-input-fence"));

    make_live_media_command(&command, IVR_MEDIA_COMMAND_SESSION_OPEN,
                            "input-fence-open", 1);
    TEST_ASSERT_EQUAL(IVR_OK,
                      send_and_decode_media_command(&command, &received));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_media_result(
                                  g_gateway, &received, IVR_OK, "", ""));
    for (int i = 0; i < 200 && atomic_load(&g_media_result_calls) < 1; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_media_result_calls));

    make_live_media_command(&command, IVR_MEDIA_COMMAND_INPUT_START,
                            "input-fence-start", 2);
    snprintf(command.input_id, sizeof(command.input_id), "input-current");
    command.input_generation = 7;
    TEST_ASSERT_EQUAL(IVR_OK,
                      send_and_decode_media_command(&command, &received));
    TEST_ASSERT_EQUAL_STRING("input-current", received.input_id);
    TEST_ASSERT_EQUAL_UINT64(7, received.input_generation);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_media_result(
                                  g_gateway, &received, IVR_OK, "", ""));
    for (int i = 0; i < 200 && atomic_load(&g_media_result_calls) < 2; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&g_media_result_calls));

    make_live_media_command(&command, IVR_MEDIA_COMMAND_CANCEL,
                            "input-fence-cancel", 3);
    snprintf(command.input_id, sizeof(command.input_id), "input-current");
    command.input_generation = 7;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_observe_media_command(
                                  g_adapter, &command, &observation));
    TEST_ASSERT_EQUAL_INT(IRIS_RESOURCE_OBSERVATION_ACTIVE,
                          observation.state);
    TEST_ASSERT_EQUAL_STRING("ivr-worker-test",
                             observation.provider_resource_id);

    snprintf(command.input_id, sizeof(command.input_id), "input-old");
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_fmq_adapter_send_media_command(
                                     g_adapter, &command,
                                     observation.provider_resource_id,
                                     sizeof(observation.provider_resource_id)));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_observe_media_command(
                                  g_adapter, &command, &observation));
    TEST_ASSERT_EQUAL_INT(IRIS_RESOURCE_OBSERVATION_ABSENT,
                          observation.state);

    snprintf(command.input_id, sizeof(command.input_id), "input-current");
    TEST_ASSERT_EQUAL(IVR_OK,
                      send_and_decode_media_command(&command, &received));
    TEST_ASSERT_EQUAL_STRING("input-current", received.input_id);
    TEST_ASSERT_EQUAL_UINT64(7, received.input_generation);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_media_result(
                                  g_gateway, &received, IVR_OK, "", ""));
    for (int i = 0; i < 200 && atomic_load(&g_media_result_calls) < 3; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(3, atomic_load(&g_media_result_calls));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_observe_media_command(
                                  g_adapter, &command, &observation));
    TEST_ASSERT_EQUAL_INT(IRIS_RESOURCE_OBSERVATION_ABSENT,
                          observation.state);
    TEST_ASSERT_EQUAL(IVR_ENOTFOUND, ivr_fmq_adapter_send_media_command(
                                        g_adapter, &command,
                                        observation.provider_resource_id,
                                        sizeof(observation.provider_resource_id)));

    snprintf(command.dialog_id, sizeof(command.dialog_id), "dialog-missing");
    TEST_ASSERT_EQUAL(IVR_ENOTFOUND, ivr_fmq_adapter_send_media_command(
                                        g_adapter, &command,
                                        observation.provider_resource_id,
                                        sizeof(observation.provider_resource_id)));
}

void test_worker_inventory_page_is_epoch_fenced_by_adapter(void) {
    ivr_worker_inventory_request_t request;
    ivr_worker_inventory_request_t received;
    ivr_worker_inventory_envelope_t page;
    ivr_fmq_adapter_stats_t stats;

    TEST_ASSERT_TRUE(sync_media_worker_v2("ws-inventory-route"));
    memset(&request, 0, sizeof(request));
    snprintf(request.message_id, sizeof(request.message_id),
             "inventory-adapter-1");
    snprintf(request.worker_id, sizeof(request.worker_id),
             "ivr-worker-test");
    request.query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    request.query.limit = 1;
    ivr_mutex_lock(&g_reply_lock);
    g_command_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_request_inventory(g_adapter,
                                                        &request));
    TEST_ASSERT_TRUE(wait_command(8000));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_decode_inventory_query(
                          g_codec, g_command, g_command_len, &received));

    memset(&page, 0, sizeof(page));
    snprintf(page.message_id, sizeof(page.message_id), "%s",
             received.message_id);
    snprintf(page.worker_id, sizeof(page.worker_id), "%s",
             received.worker_id);
    page.status_code = IVR_OK;
    page.page.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    page.page.revision = 4;
    page.page.total_active = 1;
    page.page.count = 1;
    snprintf(page.page.records[0].worker_id,
             sizeof(page.page.records[0].worker_id), "ivr-worker-test");
    snprintf(page.page.records[0].worker_instance_id,
             sizeof(page.page.records[0].worker_instance_id),
             "stale-instance");
    page.page.records[0].worker_epoch = 1;
    snprintf(page.page.records[0].tenant_id,
             sizeof(page.page.records[0].tenant_id), "tenant-a");
    snprintf(page.page.records[0].provider_session_id,
             sizeof(page.page.records[0].provider_session_id), "session-a");
    snprintf(page.page.records[0].dialog_id,
             sizeof(page.page.records[0].dialog_id), "dialog-a");
    snprintf(page.page.records[0].room_id,
             sizeof(page.page.records[0].room_id), "room-42");
    snprintf(page.page.records[0].call_id,
             sizeof(page.page.records[0].call_id), "call-42");
    page.page.records[0].call_generation = 1;
    page.page.records[0].state = IVR_WORKER_RESOURCE_ACTIVE;
    page.page.records[0].rebindable = 1;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_inventory_page(g_gateway,
                                                             &page));
    ivr_thread_sleep_ms(200);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_inventory_page_calls));
    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(1, stats.bridge.inventory_page_rejects);

    snprintf(page.page.records[0].worker_instance_id,
             sizeof(page.page.records[0].worker_instance_id),
             "media-instance");
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_inventory_page(g_gateway,
                                                             &page));
    for (int i = 0; i < 200 &&
                    atomic_load(&g_inventory_page_calls) == 0;
         ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_inventory_page_calls));
    TEST_ASSERT_EQUAL_STRING("session-a",
                             g_last_inventory_page.page.records[0]
                                 .provider_session_id);
    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(1, stats.bridge.inventory_pages);
}

void test_worker_loss_persists_fact_before_releasing_dialog_route(void) {
    ivr_media_command_t command;
    ivr_media_command_t received;
    ivr_fmq_worker_snapshot_t worker;
    ivr_fmq_adapter_stats_t stats;
    char worker_id[128];
    char first_event_id[128];

    TEST_ASSERT_TRUE(sync_media_worker_v2("ws-dialog-worker-loss"));
    memset(&command, 0, sizeof(command));
    command.kind = IVR_MEDIA_COMMAND_SESSION_OPEN;
    snprintf(command.message_id, sizeof(command.message_id),
             "iris-command-worker-loss-open");
    snprintf(command.tenant_id, sizeof(command.tenant_id), "tenant-a");
    snprintf(command.provider_session_id,
             sizeof(command.provider_session_id), "iris-session-worker-loss");
    snprintf(command.dialog_id, sizeof(command.dialog_id),
             "dialog-worker-loss");
    snprintf(command.room_id, sizeof(command.room_id), "room-42");
    snprintf(command.call_id, sizeof(command.call_id), "call-42");
    command.call_generation = 1;
    command.operation_generation = 1;
    command.deadline_timeout_ms = 5000;

    ivr_mutex_lock(&g_reply_lock);
    g_command_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_send_media_command(
                                  g_adapter, &command, worker_id,
                                  sizeof(worker_id)));
    TEST_ASSERT_TRUE(wait_command(8000));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_decode_media_command(
                                  g_codec, g_command, g_command_len, &received));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_media_result(
                                  g_gateway, &received, IVR_OK, "", ""));
    for (int i = 0; i < 200 && atomic_load(&g_media_result_calls) == 0; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_media_result_calls));

    atomic_store(&g_media_event_result, IVR_ENOSPC);
    atomic_store(&g_live_clock_offset_ms, 20000);
    ivr_fmq_adapter_poll(g_adapter);
    for (int i = 0; i < 200 && atomic_load(&g_media_event_calls) == 0; ++i) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_TRUE(atomic_load(&g_media_event_calls) >= 1);
    TEST_ASSERT_EQUAL_STRING("provider.media.worker_lost",
                             g_last_media_event.event_type);
    TEST_ASSERT_EQUAL_STRING("tenant-a", g_last_media_event.tenant_id);
    TEST_ASSERT_EQUAL_UINT64(1u, g_last_media_event.sequence);
    TEST_ASSERT_EQUAL_STRING("iris-session-worker-loss",
                             g_last_media_event.provider_session_id);
    TEST_ASSERT_EQUAL_STRING("dialog-worker-loss",
                             g_last_media_event.dialog_id);
    TEST_ASSERT_TRUE(g_last_media_event.event_id[0] != '\0');
    TEST_ASSERT_TRUE(g_last_media_event.occurred_at_ms != 0);
    snprintf(first_event_id, sizeof(first_event_id), "%s",
             g_last_media_event.event_id);

    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.dialogs);
    atomic_store(&g_media_event_result, IVR_OK);
    ivr_fmq_adapter_poll(g_adapter);
    for (int i = 0; i < 200; ++i) {
        ivr_fmq_adapter_get_stats(g_adapter, &stats);
        if (stats.dialogs == 0) break;
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL_STRING(first_event_id, g_last_media_event.event_id);

    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.dialogs);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
}

void test_live_join_is_independent_from_ivr_worker_availability(void) {
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;

    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-no-worker", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_INT(0, participant_role_of(g_service, "call-42", &role));
    TEST_ASSERT_EQUAL_INT(TURBO_PARTICIPANT_ROLE_CUSTOMER, role);
    turbo_room_summary_t summary;
    TEST_ASSERT_EQUAL_INT(0, turbo_room_service_get_room_summary(
                                 g_service, "room-42", &summary));
    TEST_ASSERT_EQUAL_UINT64(2u, (uint64_t)summary.version);
    TEST_ASSERT_EQUAL_INT(0, g_prepare_calls);
}

void test_pending_dispatch_reservation_blocks_capacity(void) {
    TEST_ASSERT_TRUE(sync_worker("ws-capacity"));
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-capacity-1", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_i32("status_code"));

    /* No worker ACK is sent in this adapter test, so the single V1 slot stays
       reserved and a second attempt cannot overbook it. */
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-capacity-2", 2));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, reply_i32("status_code"));
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, worker.reserved_sessions);
}

void test_heartbeat_updates_active_without_overwriting_room_reservation(void) {
    ivr_worker_status_view_t status;
    memset(&status, 0, sizeof(status));
    status.instance_id = "heartbeat-instance";
    status.connection_generation = 9;
    status.max_sessions = 3;
    status.lease_duration_ms = 15000;
    status.capabilities = "turboxml,flowmq,dispatch-v2,health.ready";

    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_worker_sync_v2(
                                  g_gateway, "ws-v2-heartbeat", &status));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, worker_reply_i32("status_code"));

    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-heartbeat-reservation", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_i32("status_code"));

    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, worker.reserved_sessions);

    status.active_sessions = 1;
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_worker_heartbeat(
                                  g_gateway, "heartbeat-valid", &status));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, worker_reply_i32("status_code"));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(1u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, worker.reserved_sessions);

    status.reserved_sessions = 1;
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_worker_heartbeat(
                                  g_gateway, "heartbeat-invalid", &status));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_EINVAL, worker_reply_i32("status_code"));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(1u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, worker.reserved_sessions);
}

void test_dispatch_deadline_releases_pending_reservation(void) {
    TEST_ASSERT_TRUE(sync_worker("ws-deadline"));
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-deadline-1", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_i32("status_code"));

    ivr_fmq_adapter_stats_t stats;
    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.assignments);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.assignment_high_water);
    TEST_ASSERT_EQUAL_UINT32(8u, stats.bridge.request_queue_capacity);

    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-deadline-1",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_PENDING, assignment.state);
    ivr_thread_sleep_ms(800);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-deadline-1",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_REJECTED, assignment.state);
    TEST_ASSERT_EQUAL_INT(IVR_ECLOSED, assignment.status_code);
    TEST_ASSERT_EQUAL_STRING("dispatch_timeout", assignment.error_code);

    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);
    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(1u, stats.dispatch_timeout_total);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.assignment_high_water);
    ivr_fmq_adapter_poll(g_adapter);
    ivr_fmq_adapter_get_stats(g_adapter, &stats);
    TEST_ASSERT_EQUAL_UINT64(1u, stats.dispatch_timeout_total);
}

void test_media_prepare_failure_compensates_join_and_sequence(void) {
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;
    TEST_ASSERT_TRUE(sync_worker("ws-media-fail"));
    g_prepare_result = IVR_ESTATE;

    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-media-retry", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_INT(-1, participant_role_of(g_service, "call-42", &role));
    TEST_ASSERT_EQUAL_INT(1, g_prepare_calls);

    g_prepare_result = IVR_OK;
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-media-retry", 0));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_UINT64(1u, reply_u64("sequence"));
    TEST_ASSERT_EQUAL_INT(0, participant_role_of(g_service, "call-42", &role));
    TEST_ASSERT_EQUAL_INT(TURBO_PARTICIPANT_ROLE_CUSTOMER, role);
    TEST_ASSERT_EQUAL_INT(2, g_prepare_calls);
}

void test_live_worker_sync_registration(void) {
    TEST_ASSERT_TRUE(sync_worker("ws"));
    /* the worker is now registered with the authoritative aggregate */
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                       "ivr-worker-test"));
}

void test_worker_disconnect_invalidates_live_registration(void) {
    TEST_ASSERT_TRUE(sync_worker("ws-disconnect"));
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                       "ivr-worker-test"));

    ivr_flowmq_gateway_destroy(g_gateway);
    g_gateway = NULL;
    for (int i = 0; i < 200 &&
                    ivr_fmq_adapter_worker_registered(g_adapter,
                                                      "ivr-worker-test");
         i++) {
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                        "ivr-worker-test"));

    ivr_flowmq_gateway_config_t config;
    memset(&config, 0, sizeof(config));
    config.worker_id = "ivr-worker-replacement";
    config.host = "127.0.0.1";
    config.port = TEST_FMQ_PORT;
    config.timeout_ms = 5000;
    config.on_reply = on_reply_cb;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_create(&config, &g_ops, &g_gateway));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(g_gateway));
    ivr_thread_sleep_ms(800);
    TEST_ASSERT_TRUE(sync_worker("ws-reconnect"));
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                        "ivr-worker-test"));
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(
        g_adapter, "ivr-worker-replacement"));
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-reconnect", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));
}

void test_worker_sync_unconnected_identity_rejected(void) {
    /* a DEALER claiming an identity with no live connection is rejected and
       never appears in the authoritative worker registry */
    ivr_flowmq_gateway_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    acfg.worker_id = "attacker-1";
    acfg.host = "127.0.0.1";
    acfg.port = TEST_FMQ_PORT;
    acfg.timeout_ms = 5000;
    acfg.on_reply = on_reply_cb;
    ivr_command_gateway_ops_t aops;
    ivr_flowmq_gateway_t *attacker = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_create(&acfg, &aops, &attacker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(attacker));
    ivr_thread_sleep_ms(800);

    uint8_t frame[4096];
    size_t len = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_encode_worker_sync(
                          g_codec, "ws-spoof", "ghost-worker", frame,
                          sizeof(frame), &len));
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_frame(attacker, frame, len));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_FALSE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                        "ghost-worker"));

    /* the real worker (connected as its claimed id) registers fine */
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_worker_sync(g_gateway, "ws-ok"));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(g_adapter,
                                                       "ivr-worker-test"));

    ivr_flowmq_gateway_destroy(attacker);
}


/* P0-04.4: command authorization over tenant/room/call scope. A connected
   worker cannot bypass the configured ACL; a denied command never changes the
   authoritative room version. */
void test_apply_acl_scope_denies_cross_tenant_room_call(void) {
    turbo_room_service_t *service = turbo_room_service_create();
    TEST_ASSERT_NOT_NULL(service);
    turbo_room_config_t rcfg;
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.room_id = "acme/room-1";
    rcfg.room_type = TURBO_ROOM_TYPE_CONFERENCE;
    TEST_ASSERT_EQUAL_INT(0,
                          turbo_room_service_create_room(service, &rcfg));
    rcfg.room_id = "acme/room-2";
    TEST_ASSERT_EQUAL_INT(0,
                          turbo_room_service_create_room(service, &rcfg));
    rcfg.room_id = "other/room-2";
    TEST_ASSERT_EQUAL_INT(0,
                          turbo_room_service_create_room(service, &rcfg));

    ivr_fmq_worker_acl_entry_t acl = {
        "ivr-worker-test", "acme", "acme/room-1,acme/room-2", "call-1,call-2",
        "conference-greeting"};
    ivr_fmq_adapter_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_port = 0; /* host logic only */
    cfg.worker_acls = &acl;
    cfg.worker_acl_count = 1;
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(service, &cfg, &adapter));

    ivr_room_command_t cmd;
    ivr_room_command_result_t result;

    /* in-scope tenant + room + call: applied (room version advances) */
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.message_id, sizeof(cmd.message_id), "mid-ok");
    snprintf(cmd.worker_id, sizeof(cmd.worker_id), "ivr-worker-test");
    snprintf(cmd.room_id, sizeof(cmd.room_id), "acme/room-1");
    snprintf(cmd.call_id, sizeof(cmd.call_id), "call-1");
    cmd.call_generation = 1;
    snprintf(cmd.command, sizeof(cmd.command), "conference.join");
    snprintf(cmd.participant_role, sizeof(cmd.participant_role), "caller");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status_code);
    TEST_ASSERT_EQUAL_UINT64(2u, result.room_version);

    /* cross-tenant room: denied, room version unchanged */
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.message_id, sizeof(cmd.message_id), "mid-cross");
    snprintf(cmd.worker_id, sizeof(cmd.worker_id), "ivr-worker-test");
    snprintf(cmd.room_id, sizeof(cmd.room_id), "other/room-2");
    snprintf(cmd.call_id, sizeof(cmd.call_id), "call-9");
    cmd.call_generation = 1;
    snprintf(cmd.command, sizeof(cmd.command), "conference.join");
    snprintf(cmd.participant_role, sizeof(cmd.participant_role), "caller");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EAUTH, result.status_code);
    TEST_ASSERT_EQUAL_UINT64(1u, result.room_version);
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;
    TEST_ASSERT_EQUAL_INT(-1, participant_role_of(service, "call-9", &role));

    /* out-of-scope room (same tenant, not listed): denied */
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.message_id, sizeof(cmd.message_id), "mid-room");
    snprintf(cmd.worker_id, sizeof(cmd.worker_id), "ivr-worker-test");
    snprintf(cmd.room_id, sizeof(cmd.room_id), "acme/room-3");
    snprintf(cmd.call_id, sizeof(cmd.call_id), "call-1");
    cmd.call_generation = 1;
    snprintf(cmd.command, sizeof(cmd.command), "conference.join");
    snprintf(cmd.participant_role, sizeof(cmd.participant_role), "caller");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EAUTH, result.status_code);

    /* out-of-scope call: denied */
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.message_id, sizeof(cmd.message_id), "mid-call");
    snprintf(cmd.worker_id, sizeof(cmd.worker_id), "ivr-worker-test");
    snprintf(cmd.room_id, sizeof(cmd.room_id), "acme/room-1");
    snprintf(cmd.call_id, sizeof(cmd.call_id), "call-99");
    cmd.call_generation = 1;
    snprintf(cmd.command, sizeof(cmd.command), "conference.join");
    snprintf(cmd.participant_role, sizeof(cmd.participant_role), "caller");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EAUTH, result.status_code);

    /* unlisted worker: denied (fail closed when an ACL is configured) */
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.message_id, sizeof(cmd.message_id), "mid-unlisted");
    snprintf(cmd.worker_id, sizeof(cmd.worker_id), "ghost-worker");
    snprintf(cmd.room_id, sizeof(cmd.room_id), "acme/room-1");
    snprintf(cmd.call_id, sizeof(cmd.call_id), "call-1");
    cmd.call_generation = 1;
    snprintf(cmd.command, sizeof(cmd.command), "conference.join");
    snprintf(cmd.participant_role, sizeof(cmd.participant_role), "caller");
    memset(&result, 0, sizeof(result));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &cmd, &result));
    TEST_ASSERT_EQUAL_INT(IVR_EAUTH, result.status_code);

    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

/* P0-04.4: live command path also enforces the ACL before any dispatch or
   mutation; the worker cannot register under a scope it is not granted. */
void test_live_acl_blocks_out_of_scope_join(void) {
    /* Self-contained: this test creates its own service/adapter/gateway, so
       it must use a port distinct from the setUp adapter on TEST_FMQ_PORT. */
    turbo_room_service_t *service = make_service_with_room();
    TEST_ASSERT_NOT_NULL(service);
    ivr_fmq_worker_acl_entry_t acl = {
        "ivr-worker-test", "acme", "acme/room-1", "*", "conference-greeting"};
    ivr_fmq_adapter_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_host = "127.0.0.1";
    cfg.bind_port = TEST_FMQ_PORT + 1;
    cfg.pub_port = 0;
    cfg.timeout_ms = 5000;
    cfg.queue_capacity = 8;
    cfg.dedup_capacity = 8;
    cfg.worker_capacity = 1;
    cfg.dispatch_deadline_ms = 500;
    cfg.worker_acls = &acl;
    cfg.worker_acl_count = 1;
    cfg.media.prepare_caller_audio = test_prepare_caller_audio;
    cfg.media.release_caller_audio = test_release_caller_audio;
    ivr_fmq_adapter_t *adapter = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(service, &cfg, &adapter));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_start(adapter));

    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = "ivr-worker-test";
    gcfg.host = "127.0.0.1";
    gcfg.port = TEST_FMQ_PORT + 1;
    gcfg.timeout_ms = 5000;
    gcfg.on_reply = on_reply_cb;
    ivr_command_gateway_ops_t ops;
    ivr_flowmq_gateway_t *gateway = NULL;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(&gcfg, &ops,
                                                        &gateway));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(gateway));
    ivr_thread_sleep_ms(800);

    /* register the worker (connected identity == claimed identity) */
    {
        int synced = 0;
        for (int attempt = 0; attempt < 20 && !synced; attempt++) {
            char mid[64];
            snprintf(mid, sizeof(mid), "ws-acl-%d", attempt);
            ivr_mutex_lock(&g_reply_lock);
            g_reply_ready = 0;
            ivr_mutex_unlock(&g_reply_lock);
            if (ivr_flowmq_gateway_send_worker_sync(gateway, mid) != IVR_OK) {
                continue;
            }
            if (wait_reply(8000)) {
                synced = worker_reply_i32("status_code") == 0;
            }
        }
        TEST_ASSERT_TRUE(synced);
    }
    TEST_ASSERT_TRUE(ivr_fmq_adapter_worker_registered(adapter,
                                                       "ivr-worker-test"));

    /* join "room-42" is outside the worker scope: EAUTH, room unchanged */
    static ivr_bytes_view_t type = {"conference.join", 15};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    ivr_command_view_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.message_id.data = "mid-acl-1";
    cmd.message_id.size = strlen(cmd.message_id.data);
    cmd.command_type = type;
    cmd.call.room_id = room;
    cmd.call.call_id = call;
    cmd.call.call_generation = 1;
    cmd.call.expected_room_version = 1;
    cmd.args_json = args;
    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, ops.submit_copy(ops.context, &cmd));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_EAUTH, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_UINT64(1u, reply_u64("room_version"));
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;
    TEST_ASSERT_EQUAL_INT(-1, participant_role_of(service, "call-42", &role));

    ivr_flowmq_gateway_destroy(gateway);
    ivr_fmq_adapter_stop(adapter);
    ivr_fmq_adapter_destroy(adapter);
    turbo_room_service_destroy(service);
}

spec("test_ivr_fmq_adapter") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_apply_join_leave_snapshot);
  TT_TEST(test_apply_room_missing);
  TT_TEST(test_apply_worker_sync);
  TT_TEST(test_worker_v2_lease_heartbeat_and_stale_generation);
  TT_TEST(test_v2_worker_without_active_health_capability_is_not_ready);
  TT_TEST(test_fresh_registry_requires_fenced_reconcile_for_active_worker);
  TT_TEST(test_live_join_is_independent_from_ivr_worker_availability);
  TT_TEST(test_live_join_reply_idempotent_stale);
  TT_TEST(test_dialog_start_creates_media_route_for_following_commands);
  TT_TEST(test_media_cancel_tracks_the_exact_active_input);
  TT_TEST(test_worker_inventory_page_is_epoch_fenced_by_adapter);
  TT_TEST(test_worker_loss_persists_fact_before_releasing_dialog_route);
  TT_TEST(test_live_worker_sync_registration);
  TT_TEST(test_worker_disconnect_invalidates_live_registration);
  TT_TEST(test_worker_sync_unconnected_identity_rejected);
  TT_TEST(test_apply_acl_scope_denies_cross_tenant_room_call);
  TT_TEST(test_live_acl_blocks_out_of_scope_join);
}
