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
#include "turbomedia_ivr_v1.h"
#include "tinytest_compat.h"
#include <string.h>

#define TEST_FMQ_PORT 17715

static DataBind *g_codec = NULL;

/* ------------------------------------------------------------------ */
/* decoded result helpers (DEALER reply frames)                        */
/* ------------------------------------------------------------------ */

static uint8_t g_reply[8192];
static size_t g_reply_len = 0;
static ivr_mutex_t g_reply_lock;
static int g_reply_ready = 0;

static void on_reply_cb(void *ctx, const uint8_t *frame, size_t len) {
    ivr_frame_info_t info;
    (void)ctx;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.kind != IVR_KIND_RESULT) {
        return;
    }
    ivr_mutex_lock(&g_reply_lock);
    if (len <= sizeof(g_reply)) {
        memcpy(g_reply, frame, len);
        g_reply_len = len;
        g_reply_ready = 1;
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
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);

    ivr_fmq_worker_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(adapter, "worker-a",
                                                         &snapshot));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_READY, snapshot.state);
    TEST_ASSERT_EQUAL_UINT64(310u, snapshot.lease_expires_at_ms);
    TEST_ASSERT_EQUAL_UINT32(2u, snapshot.max_sessions);

    g_fake_now_ms = 100;
    snprintf(command.command, sizeof(command.command), "%s",
             "worker.heartbeat");
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(adapter, "worker-a",
                                                         &snapshot));
    TEST_ASSERT_EQUAL_UINT64(400u, snapshot.lease_expires_at_ms);

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

void test_fresh_registry_rejects_active_worker_without_recovery_snapshot(void) {
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
    make_worker_control(&command, "worker.sync.v2", "worker-restart",
                        "instance-restart", 4);
    command.active_sessions = 1;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, result.status_code);
    TEST_ASSERT_FALSE(
        ivr_fmq_adapter_worker_registered(adapter, "worker-restart"));

    command.active_sessions = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_apply(adapter, &command, &result));
    TEST_ASSERT_EQUAL_INT(IVR_OK, result.status_code);
    TEST_ASSERT_TRUE(
        ivr_fmq_adapter_worker_registered(adapter, "worker-restart"));

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
    cfg.media.prepare_caller_audio = test_prepare_caller_audio;
    cfg.media.release_caller_audio = test_release_caller_audio;
    g_prepare_result = IVR_OK;
    g_prepare_calls = 0;
    g_release_calls = 0;
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

void test_live_join_requires_available_worker(void) {
    turbo_participant_role_t role = TURBO_PARTICIPANT_ROLE_GUEST;

    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-no-worker", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_ESTATE, reply_i32("status_code"));
    TEST_ASSERT_EQUAL_INT(-1, participant_role_of(g_service, "call-42", &role));
    turbo_room_summary_t summary;
    TEST_ASSERT_EQUAL_INT(0, turbo_room_service_get_room_summary(
                                 g_service, "room-42", &summary));
    TEST_ASSERT_EQUAL_UINT64(1u, (uint64_t)summary.version);
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
  TT_TEST(test_fresh_registry_rejects_active_worker_without_recovery_snapshot);
  TT_TEST(test_live_join_requires_available_worker);
  TT_TEST(test_pending_dispatch_reservation_blocks_capacity);
  TT_TEST(test_heartbeat_updates_active_without_overwriting_room_reservation);
  TT_TEST(test_dispatch_deadline_releases_pending_reservation);
  TT_TEST(test_media_prepare_failure_compensates_join_and_sequence);
  TT_TEST(test_live_join_reply_idempotent_stale);
  TT_TEST(test_live_worker_sync_registration);
  TT_TEST(test_worker_disconnect_invalidates_live_registration);
  TT_TEST(test_worker_sync_unconnected_identity_rejected);
  TT_TEST(test_apply_acl_scope_denies_cross_tenant_room_call);
  TT_TEST(test_live_acl_blocks_out_of_scope_join);
}
