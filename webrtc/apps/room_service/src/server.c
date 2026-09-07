#include "room_service/server.h"
#include "room_service/http_api.h"
#include "turbo_transport.h"
#include "turbo_media_auth.h"
#include <json_parser.h>
#include "turbo_room_service.h"
#include "salts_thread.h"
#include "turbo_crypto.h"
#include "salts_uuid.h"
#include "tlog.h"
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
#include "iris_command_ledger.h"
#include "iris_event_outbox.h"
#include "iris_completion_dispatcher.h"
#include "iris_control_provider.h"
#include "iris_media_bridge.h"
#include "iris_media_reconciler.h"
#include "iris_room_bridge.h"
#include "ivr_certificate_identity.h"
#include "ivr_control_adapter.h"
#include "room_service_media.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdatomic.h>
#include <time.h>

#define ROOM_SERVICE_IVR_WORKER_CAPACITY 64u
#define ROOM_SERVICE_RECONCILE_INVENTORY_PAGE_SIZE 32u

#ifdef _WIN32
#include <windows.h>
static void room_service_sleep_ms(unsigned int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void room_service_sleep_ms(unsigned int ms) { usleep(ms * 1000); }
#endif

struct room_service_app_server_s {
    room_service_app_config_t config;
    turbo_room_service_t *service;
    room_service_http_api_t *http_api;
    struct room_service_sfu_node_entry_s *sfu_nodes;
    int sfu_node_count;
    int sfu_node_capacity;
    int next_sfu_node_index;
    room_service_room_sync_diagnostic_t *room_sync_diagnostics;
    int room_sync_diagnostic_count;
    int room_sync_diagnostic_capacity;
    room_service_call_center_event_t *call_center_events;
    int call_center_event_count;
    int call_center_event_capacity;
    room_service_conference_policy_t *conference_policies;
    int conference_policy_count;
    int conference_policy_capacity;
    int64_t room_sync_sequence;
    int64_t call_center_event_sequence;
    int64_t conference_policy_sequence;
    salts_mutex_t mutex;
    atomic_int running;
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    ivr_control_adapter_t *ivr_control; /* NULL only when the IVR feature is disabled */
    ivr_certificate_identity_t *ivr_control_identity;
    iris_command_ledger_t *iris_command_ledger;
    iris_media_bridge_t *iris_media_bridge;
    iris_room_bridge_t *iris_room_bridge;
    iris_control_provider_t *iris_control_provider;
    iris_completion_dispatcher_t *iris_completion_dispatcher;
    iris_event_outbox_t *iris_event_outbox;
    iris_media_reconciler_t *iris_media_reconciler;
#endif
};

#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
static int room_service_room_completion_id(
    const char *command_id, char out[SALTS_UUID_STRING_SIZE]) {
    static const char domain[] = "turbomedia.room.terminal.v1";
    turbo_crypto_sha256_ctx_t hash;
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    salts_uuid_t uuid;
    if (!command_id || !command_id[0] ||
        turbo_crypto_sha256_init(&hash) != 0 ||
        turbo_crypto_sha256_update(&hash, domain, sizeof(domain) - 1u) != 0 ||
        turbo_crypto_sha256_update(&hash, command_id, strlen(command_id)) != 0 ||
        turbo_crypto_sha256_final(&hash, digest) != 0) {
        return 0;
    }
    memcpy(uuid.bytes, digest, sizeof(uuid.bytes));
    uuid.bytes[6] = (uint8_t)((uuid.bytes[6] & 0x0fu) | 0x80u);
    uuid.bytes[8] = (uint8_t)((uuid.bytes[8] & 0x3fu) | 0x80u);
    return salts_uuid_format(&uuid, out, SALTS_UUID_STRING_SIZE) == SALTS_OK;
}

static const char *room_service_provider_json_string(
    const json_value_t *object, const char *name) {
    json_value_t *value = object ? json_object_get(object, name) : NULL;
    return value && json_type(value) == JSON_STRING
               ? json_string(value)
               : NULL;
}

static int room_service_provider_copy(char *out, size_t capacity,
                                      const char *value) {
    size_t size;
    if (!out || capacity == 0u || !value) return 0;
    size = strlen(value);
    if (size >= capacity) return 0;
    memcpy(out, value, size + 1u);
    return 1;
}

static iris_media_bridge_status_t room_service_room_receipt_status(
    iris_room_bridge_status_t status) {
    switch (status) {
        case IRIS_ROOM_BRIDGE_TERMINAL:
            return IRIS_MEDIA_BRIDGE_TERMINAL;
        case IRIS_ROOM_BRIDGE_DUPLICATE:
            return IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY;
        case IRIS_ROOM_BRIDGE_IN_PROGRESS:
            return IRIS_MEDIA_BRIDGE_DUPLICATE;
        case IRIS_ROOM_BRIDGE_INVALID:
            return IRIS_MEDIA_BRIDGE_INVALID;
        case IRIS_ROOM_BRIDGE_CONFLICT:
            return IRIS_MEDIA_BRIDGE_CONFLICT;
        case IRIS_ROOM_BRIDGE_EXPIRED:
            return IRIS_MEDIA_BRIDGE_EXPIRED;
        case IRIS_ROOM_BRIDGE_FULL:
            return IRIS_MEDIA_BRIDGE_FULL;
        case IRIS_ROOM_BRIDGE_UNAVAILABLE:
            return IRIS_MEDIA_BRIDGE_UNAVAILABLE;
        case IRIS_ROOM_BRIDGE_INTERNAL:
        default:
            return IRIS_MEDIA_BRIDGE_INTERNAL;
    }
}

static int room_service_is_room_provider_command(const char *body,
                                                 size_t body_size) {
    json_value_t *root = NULL;
    json_value_t *data;
    json_value_t *capability;
    int is_room = 0;
    if (body && body_size > 0u &&
        ((root = json_parse((const char *)((const uint8_t *)body), body_size)) ? 0 : -1) == 0 &&
        root && json_type(root) == JSON_OBJECT) {
        data = json_object_get(root, "data");
        capability = data && json_type(data) == JSON_OBJECT
                         ? json_object_get(data, "capability")
                         : NULL;
        is_room = capability &&
                  json_type(capability) == JSON_STRING &&
                  strcmp(json_string(capability), "room") == 0;
    }
    json_free(root);
    root = NULL;
    return is_room;
}

static int room_service_build_provider_completion(
    const char *body, size_t body_size, const char *command_id,
    const char *terminal_status, const char *event_type,
    const char *result_json, const char *error_code,
    const char *error_message,
    iris_media_completion_t *completion,
    ivr_media_command_result_t *result,
    char event_id[SALTS_UUID_STRING_SIZE]) {
    json_value_t *root = NULL;
    json_value_t *epoch_value;
    const char *tenant_id;
    const char *session_id;
    const char *worker_id;
    const char *correlation_id;
    uint64_t epoch;
    int valid = 0;
    if (!body || !command_id || !command_id[0] || !terminal_status ||
        !terminal_status[0] || !event_type || !event_type[0] || !result_json ||
        !result_json[0] || !completion || !result ||
        ((root = json_parse((const char *)((const uint8_t *)body), body_size)) ? 0 : -1) != 0 ||
        !root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        return 0;
    }
    tenant_id = room_service_provider_json_string(root, "tenantId");
    session_id = room_service_provider_json_string(root, "sessionId");
    worker_id = room_service_provider_json_string(root, "workerId");
    correlation_id = room_service_provider_json_string(root, "correlationId");
    epoch_value = json_object_get(root, "dispatchEpoch");
    epoch = epoch_value && json_type(epoch_value) == JSON_NUMBER
                ? (uint64_t)json_number(epoch_value)
                : 0u;
    memset(completion, 0, sizeof(*completion));
    memset(result, 0, sizeof(*result));
    if (tenant_id && tenant_id[0] && session_id && session_id[0] && worker_id &&
        worker_id[0] && correlation_id && correlation_id[0] && epoch > 0u &&
        room_service_provider_copy(completion->command_id,
                                   sizeof(completion->command_id),
                                   command_id) &&
        room_service_provider_copy(completion->tenant_id,
                                   sizeof(completion->tenant_id), tenant_id) &&
        room_service_provider_copy(completion->provider_session_id,
                                   sizeof(completion->provider_session_id),
                                   session_id) &&
        room_service_provider_copy(completion->iris_worker_id,
                                   sizeof(completion->iris_worker_id), worker_id) &&
        room_service_provider_copy(completion->correlation_id,
                                   sizeof(completion->correlation_id),
                                   correlation_id) &&
        room_service_provider_copy(
            completion->terminal_status, sizeof(completion->terminal_status),
            terminal_status) &&
        room_service_provider_copy(completion->event_type,
                                   sizeof(completion->event_type),
                                   event_type) &&
        room_service_provider_copy(completion->result_json,
                                   sizeof(completion->result_json),
                                   result_json) &&
        room_service_room_completion_id(command_id, event_id)) {
        completion->dispatch_epoch = epoch;
        result->status_code = strcmp(terminal_status, "succeeded") == 0
                                  ? IVR_OK
                                  : IVR_ESTATE;
        if (error_code) {
            snprintf(result->error_code, sizeof(result->error_code), "%s",
                     error_code);
        }
        if (error_message) {
            snprintf(result->error_message, sizeof(result->error_message), "%s",
                     error_message);
        }
        valid = 1;
    }
    json_free(root);
    root = NULL;
    return valid;
}

static ivr_status_t room_service_deliver_iris_event(
    void *context, const ivr_media_event_t *event, uint64_t store_revision) {
    return iris_completion_dispatcher_enqueue_event(
        (iris_completion_dispatcher_t *)context, event, store_revision);
}

static iris_media_bridge_result_t room_service_dispatch_control_ws_command(
    void *context, const char *idempotency_key, const char *body,
    size_t body_size) {
    room_service_app_server_t *server =
        (room_service_app_server_t *)context;
    int is_room = room_service_is_room_provider_command(body, body_size);
    if (server && server->iris_media_reconciler &&
        !iris_media_reconciler_accepting_commands(
            server->iris_media_reconciler)) {
        iris_media_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = IRIS_MEDIA_BRIDGE_UNAVAILABLE;
        result.error_code = is_room ? "ROOM_PROVIDER_RECONCILING"
                                    : "MEDIA_PROVIDER_RECONCILING";
        result.error_message =
            "provider is reconciling durable and worker state";
        return result;
    }
    if (!server ||
        !atomic_load_explicit(&server->running, memory_order_acquire) ||
        !server->iris_media_bridge || !server->iris_room_bridge) {
        iris_media_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = IRIS_MEDIA_BRIDGE_UNAVAILABLE;
        result.error_code = is_room ? "ROOM_PROVIDER_NOT_READY"
                                    : "MEDIA_PROVIDER_NOT_READY";
        result.error_message = "provider command intake is closed";
        return result;
    }
    if (is_room) {
        iris_room_bridge_result_t room_result =
            iris_room_bridge_dispatch_json(server->iris_room_bridge,
                                           idempotency_key, body, body_size);
        iris_media_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = room_service_room_receipt_status(room_result.status);
        snprintf(result.command_id, sizeof(result.command_id), "%s",
                 room_result.command_id);
        result.error_code = room_result.error_code;
        result.error_message = room_result.error_message;
        if (room_result.status == IRIS_ROOM_BRIDGE_TERMINAL ||
            room_result.status == IRIS_ROOM_BRIDGE_DUPLICATE) {
            iris_media_completion_t completion;
            ivr_media_command_result_t terminal;
            char event_id[SALTS_UUID_STRING_SIZE];
            if (!server->iris_completion_dispatcher ||
                !room_service_build_provider_completion(
                    body, body_size, room_result.command_id,
                    room_result.terminal_status == IRIS_ROOM_TERMINAL_SUCCEEDED
                        ? "succeeded"
                        : "failed",
                    room_result.event_type, room_result.data,
                    room_result.error_code, room_result.error_message,
                    &completion, &terminal, event_id) ||
                iris_completion_dispatcher_enqueue_terminal(
                    server->iris_completion_dispatcher, &completion, &terminal,
                    event_id) != IVR_OK) {
                result.status = IRIS_MEDIA_BRIDGE_INTERNAL;
                result.error_code = "ROOM_COMPLETION_QUEUE_UNAVAILABLE";
                result.error_message =
                    "durable room terminal could not be queued for delivery";
            }
        }
        return result;
    }
    {
        iris_media_bridge_result_t result =
            iris_media_bridge_dispatch_json(server->iris_media_bridge,
                                            idempotency_key, body, body_size);
        if (result.status == IRIS_MEDIA_BRIDGE_TERMINAL) {
            iris_media_completion_t completion;
            ivr_media_command_result_t terminal;
            char event_id[SALTS_UUID_STRING_SIZE];
            if (!server->iris_completion_dispatcher ||
                !room_service_build_provider_completion(
                    body, body_size, result.command_id, result.terminal_status,
                    result.event_type, result.data, result.error_code,
                    result.error_message, &completion, &terminal, event_id) ||
                iris_completion_dispatcher_enqueue_terminal(
                    server->iris_completion_dispatcher, &completion, &terminal,
                    event_id) != IVR_OK) {
                result.status = IRIS_MEDIA_BRIDGE_INTERNAL;
                result.error_code = "MEDIA_COMPLETION_QUEUE_UNAVAILABLE";
                result.error_message =
                    "durable media terminal could not be queued for delivery";
            }
        }
        return result;
    }
}

static ivr_status_t room_service_deliver_control_ws_completion(
    void *context, const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *message_id,
    const char *completed_at, uint64_t completed_at_unix_ms,
    uint64_t ack_timeout_ms) {
    iris_control_completion_ack_t ack;
    ivr_status_t status = iris_control_provider_send_completion(
        (iris_control_provider_t *)context, completion, result, message_id,
        completed_at, completed_at_unix_ms, ack_timeout_ms, &ack);
    if (status != IVR_OK) return status;
    if (ack.disposition ==
            ProviderCompletionAckDisposition_CompletionCommitted ||
        ack.disposition ==
            ProviderCompletionAckDisposition_DuplicateCompletion) {
        return IVR_OK;
    }
    return ack.disposition ==
                   ProviderCompletionAckDisposition_CompletionConflict
               ? IVR_ESTALE
               : IVR_EAUTH;
}

static ivr_status_t room_service_deliver_control_ws_event(
    void *context, const ivr_media_event_t *event, const char *message_id,
    const char *occurred_at, uint64_t ack_timeout_ms) {
    iris_control_event_ack_t ack;
    ivr_status_t status = iris_control_provider_send_event(
        (iris_control_provider_t *)context, event, message_id, occurred_at,
        ack_timeout_ms, &ack);
    if (status != IVR_OK) return status;
    if (ack.disposition == ProviderEventAckDisposition_Committed ||
        ack.disposition == ProviderEventAckDisposition_DuplicateEvent) {
        return IVR_OK;
    }
    return ack.disposition == ProviderEventAckDisposition_EventConflict
               ? IVR_ESTALE
               : IVR_EAUTH;
}

static void room_service_settle_iris_event(
    void *context, const ivr_media_event_t *event, uint64_t store_revision,
    iris_event_delivery_outcome_t outcome, int http_status) {
    iris_event_outbox_on_delivery_result(context, event, store_revision,
                                         (int)outcome, http_status);
}

static ivr_status_t room_service_observe_iris_media_result(
    void *context, const ivr_media_command_result_t *result) {
    room_service_app_server_t *server =
        (room_service_app_server_t *)context;
    return iris_completion_dispatcher_on_media_result(
        server ? server->iris_completion_dispatcher : NULL, result);
}

static ivr_status_t room_service_observe_iris_media_event(
    void *context, const ivr_media_event_t *event) {
    room_service_app_server_t *server =
        (room_service_app_server_t *)context;
    return iris_event_outbox_on_media_event(
        server ? server->iris_event_outbox : NULL, event);
}

static ivr_status_t room_service_observe_iris_inventory(
    void *context, const ivr_worker_inventory_envelope_t *page) {
    room_service_app_server_t *server =
        (room_service_app_server_t *)context;
    return iris_media_reconciler_on_inventory_page(
        server ? server->iris_media_reconciler : NULL, page);
}

#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
ivr_status_t room_service_app_server_test_submit_iris_event(
    room_service_app_server_t *server, const ivr_media_event_t *event) {
    return room_service_observe_iris_media_event(server, event);
}
#endif
#endif

typedef struct {
    char subscriber_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
} room_service_policy_subscription_t;

#define ROOM_SERVICE_LAYOUT_MODE_SPEAKER "speaker"
#define ROOM_SERVICE_LAYOUT_MODE_GRID "grid"
#define ROOM_SERVICE_POLICY_SOURCE_PREFIX "conference_policy"
#define ROOM_SERVICE_POLICY_SOURCE_AUDIO "conference_policy_audio"
#define ROOM_SERVICE_POLICY_SOURCE_SCREEN "conference_policy_screen"
#define ROOM_SERVICE_POLICY_SOURCE_PIN "conference_policy_pin"
#define ROOM_SERVICE_POLICY_SOURCE_ACTIVE "conference_policy_active"
#define ROOM_SERVICE_POLICY_SOURCE_CAMERA "conference_policy_camera"
#define ROOM_SERVICE_CALL_CENTER_POLICY_SOURCE_PREFIX "call_center"
#define ROOM_SERVICE_MAX_CALL_CENTER_EVENTS 512
#define ROOM_SERVICE_SFU_CONTROL_URL_MAX 512
#define ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX 256
#define ROOM_SERVICE_SFU_CONTROL_AUDIENCE "turbomedia-sfu-control"
#define ROOM_SERVICE_SFU_SCOPE_CONTROL_WRITE "sfu.control.write"
#define ROOM_SERVICE_SFU_SCOPE_CONTROL_DANGEROUS "sfu.control.dangerous"

typedef struct room_service_sfu_node_entry_s {
    char node_id[TURBO_NODE_ID_MAX];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];
} room_service_sfu_node_entry_t;

static const char *room_service_json_string_field(
    const json_value_t *obj, const char *key);

static void room_service_copy_string(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) {
        return;
    }

    if (!src) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static int room_service_ensure_capacity(void **items, int *capacity, size_t item_size,
                                        int count_needed) {
    void *new_items;
    int new_capacity;

    if (!items || !capacity || item_size == 0 || count_needed <= *capacity) {
        return 0;
    }

    new_capacity = (*capacity > 0) ? *capacity : 4;
    while (new_capacity < count_needed) {
        new_capacity *= 2;
    }

    new_items = realloc(*items, item_size * (size_t)new_capacity);
    if (!new_items) {
        return -1;
    }

    *items = new_items;
    *capacity = new_capacity;
    return 0;
}

static int room_service_policy_has_managed_source(const char *policy_source) {
    size_t prefix_len = strlen(ROOM_SERVICE_POLICY_SOURCE_PREFIX);

    return policy_source &&
           strncmp(policy_source, ROOM_SERVICE_POLICY_SOURCE_PREFIX, prefix_len) == 0;
}

static int room_service_policy_has_call_center_source(const char *policy_source) {
    size_t prefix_len = strlen(ROOM_SERVICE_CALL_CENTER_POLICY_SOURCE_PREFIX);

    return policy_source &&
           strncmp(policy_source, ROOM_SERVICE_CALL_CENTER_POLICY_SOURCE_PREFIX,
                   prefix_len) == 0;
}

static int room_service_layout_mode_is_valid(const char *layout_mode) {
    return layout_mode &&
           (strcmp(layout_mode, ROOM_SERVICE_LAYOUT_MODE_SPEAKER) == 0 ||
            strcmp(layout_mode, ROOM_SERVICE_LAYOUT_MODE_GRID) == 0);
}

static const char *room_service_normalize_layout_mode(const char *layout_mode) {
    if (!room_service_layout_mode_is_valid(layout_mode)) {
        return ROOM_SERVICE_LAYOUT_MODE_SPEAKER;
    }

    return layout_mode;
}

static turbo_room_layout_mode_t room_service_layout_mode_to_core(
    const char *layout_mode) {
    if (layout_mode && strcmp(layout_mode, ROOM_SERVICE_LAYOUT_MODE_GRID) == 0) {
        return TURBO_ROOM_LAYOUT_GRID;
    }

    return TURBO_ROOM_LAYOUT_SPEAKER;
}

static room_service_conference_policy_t *room_service_find_conference_policy(
    room_service_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return NULL;
    }

    for (i = 0; i < server->conference_policy_count; ++i) {
        if (strcmp(server->conference_policies[i].room_id, room_id) == 0) {
            return &server->conference_policies[i];
        }
    }

    return NULL;
}

static int room_service_room_exists(room_service_app_server_t *server, const char *room_id) {
    turbo_room_summary_t summary;

    if (!server || !server->service || !room_id) {
        return 0;
    }

    return turbo_room_service_get_room_summary(server->service, room_id, &summary) == 0;
}

static int room_service_participant_exists(room_service_app_server_t *server,
                                           const char *room_id,
                                           const char *participant_id) {
    turbo_room_participant_summary_t summary;

    if (!server || !server->service || !room_id || !participant_id || !participant_id[0]) {
        return 0;
    }

    return turbo_room_service_get_participant_summary(server->service, room_id, participant_id,
                                                      &summary) == 0;
}

static room_service_conference_policy_t *room_service_get_or_create_conference_policy_locked(
    room_service_app_server_t *server, const char *room_id) {
    room_service_conference_policy_t *policy;

    if (!server || !room_id) {
        return NULL;
    }

    policy = room_service_find_conference_policy(server, room_id);
    if (policy) {
        return policy;
    }

    if (room_service_ensure_capacity((void **)&server->conference_policies,
                                     &server->conference_policy_capacity,
                                     sizeof(*server->conference_policies),
                                     server->conference_policy_count + 1) != 0) {
        return NULL;
    }

    policy = &server->conference_policies[server->conference_policy_count++];
    memset(policy, 0, sizeof(*policy));
    policy->available = 1;
    room_service_copy_string(policy->room_id, sizeof(policy->room_id), room_id);
    room_service_copy_string(policy->layout_mode, sizeof(policy->layout_mode),
                             ROOM_SERVICE_LAYOUT_MODE_SPEAKER);
    policy->supervisor_mode = TURBO_CALL_CENTER_SUPERVISOR_NONE;
    return policy;
}

static int room_service_fill_conference_policy(room_service_app_server_t *server,
                                               const char *room_id,
                                               room_service_conference_policy_t *out_policy) {
    room_service_conference_policy_t *policy;
    int rc = 0;

    if (!server || !room_id || !out_policy || !room_service_room_exists(server, room_id)) {
        return -1;
    }

    memset(out_policy, 0, sizeof(*out_policy));
    salts_mutex_lock(&server->mutex);
    policy = room_service_find_conference_policy(server, room_id);
    if (policy) {
        *out_policy = *policy;
        goto out;
    }

    out_policy->available = 1;
    room_service_copy_string(out_policy->room_id, sizeof(out_policy->room_id), room_id);
    room_service_copy_string(out_policy->layout_mode, sizeof(out_policy->layout_mode),
                             ROOM_SERVICE_LAYOUT_MODE_SPEAKER);
    out_policy->supervisor_mode = TURBO_CALL_CENTER_SUPERVISOR_NONE;

out:
    salts_mutex_unlock(&server->mutex);
    return rc;
}

static void room_service_capture_policy_warning(
    room_service_conference_policy_apply_result_t *result, const char *warning_code,
    const char *warning_message) {
    if (!result || result->had_warning || !warning_code || !warning_message) {
        return;
    }

    result->had_warning = 1;
    room_service_copy_string(result->warning_code, sizeof(result->warning_code),
                             warning_code);
    room_service_copy_string(result->warning_message, sizeof(result->warning_message),
                             warning_message);
}

static turbo_room_video_layer_t room_service_primary_camera_layer(
    const room_service_conference_policy_t *policy) {
    if (policy && strcmp(policy->layout_mode, ROOM_SERVICE_LAYOUT_MODE_GRID) == 0) {
        return TURBO_ROOM_VIDEO_LAYER_MEDIUM;
    }

    return TURBO_ROOM_VIDEO_LAYER_HIGH;
}

static void room_service_build_policy_subscription(
    const room_service_conference_policy_t *policy,
    const turbo_room_track_summary_t *track_summary, const char *subscriber_participant_id,
    room_service_policy_subscription_t *subscription) {
    int is_screen;
    int is_pinned_owner;
    int is_active_owner;
    turbo_room_video_layer_t primary_camera_layer;

    if (!policy || !track_summary || !subscriber_participant_id || !subscription) {
        return;
    }

    memset(subscription, 0, sizeof(*subscription));
    room_service_copy_string(subscription->subscriber_participant_id,
                             sizeof(subscription->subscriber_participant_id),
                             subscriber_participant_id);
    room_service_copy_string(subscription->track_id, sizeof(subscription->track_id),
                             track_summary->track_id);
    subscription->enabled = 1;

    if (track_summary->kind == TURBO_ROOM_TRACK_AUDIO) {
        subscription->priority = 400;
        subscription->preferred_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
        subscription->target_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_AUDIO);
        return;
    }

    primary_camera_layer = room_service_primary_camera_layer(policy);
    is_screen = track_summary->source == TURBO_ROOM_SOURCE_SCREEN;
    is_pinned_owner =
        policy->pinned_participant_id[0] != '\0' &&
        strcmp(track_summary->owner_participant_id,
               policy->pinned_participant_id) == 0;
    is_active_owner =
        policy->active_speaker_participant_id[0] != '\0' &&
        strcmp(track_summary->owner_participant_id,
               policy->active_speaker_participant_id) == 0;

    if (is_screen) {
        subscription->priority = 300;
        subscription->preferred_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        subscription->target_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_SCREEN);
        return;
    }

    if (is_pinned_owner) {
        subscription->priority = 250;
        subscription->preferred_layer = primary_camera_layer;
        subscription->target_layer = primary_camera_layer;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_PIN);
        return;
    }

    if (!policy->pinned_participant_id[0] && is_active_owner) {
        subscription->priority = 200;
        subscription->preferred_layer = primary_camera_layer;
        subscription->target_layer = primary_camera_layer;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_ACTIVE);
        return;
    }

    subscription->priority = 100;
    subscription->preferred_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
    subscription->target_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
    room_service_copy_string(subscription->policy_source,
                             sizeof(subscription->policy_source),
                             ROOM_SERVICE_POLICY_SOURCE_CAMERA);
}

static int room_service_desired_subscription_matches(
    const turbo_room_subscription_summary_t *summary,
    const room_service_policy_subscription_t *desired) {
    return summary && desired &&
           strcmp(summary->subscriber_participant_id,
                  desired->subscriber_participant_id) == 0 &&
           strcmp(summary->track_id, desired->track_id) == 0 &&
           summary->enabled == desired->enabled &&
           summary->priority == desired->priority &&
           summary->preferred_layer == desired->preferred_layer &&
           summary->target_layer == desired->target_layer &&
           summary->muted == desired->muted &&
           strcmp(summary->policy_source, desired->policy_source) == 0;
}

static int room_service_policy_has_desired_subscription(
    const room_service_policy_subscription_t *subscriptions, int subscription_count,
    const char *subscriber_participant_id, const char *track_id) {
    int i;

    if (!subscriptions || subscription_count <= 0 || !subscriber_participant_id || !track_id) {
        return 0;
    }

    for (i = 0; i < subscription_count; ++i) {
        if (strcmp(subscriptions[i].subscriber_participant_id,
                   subscriber_participant_id) == 0 &&
            strcmp(subscriptions[i].track_id, track_id) == 0) {
            return 1;
        }
    }

    return 0;
}

static int room_service_capture_policy_subscriptions(
    turbo_room_service_t *service, const char *room_id,
    int (*source_filter)(const char *policy_source),
    room_service_policy_subscription_t **out_subscriptions, int *out_count) {
    turbo_room_summary_t room_summary;
    room_service_policy_subscription_t *subscriptions = NULL;
    int count = 0;
    int capacity = 0;
    int i;

    if (out_subscriptions) {
        *out_subscriptions = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!service || !room_id || !source_filter || !out_subscriptions || !out_count ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
        return -1;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t summary;

        if (turbo_room_service_get_subscription_summary_at(service, room_id, i,
                                                           &summary) != 0) {
            free(subscriptions);
            return -1;
        }
        if (!source_filter(summary.policy_source)) {
            continue;
        }

        if (room_service_ensure_capacity((void **)&subscriptions, &capacity,
                                         sizeof(*subscriptions), count + 1) != 0) {
            free(subscriptions);
            return -1;
        }

        memset(&subscriptions[count], 0, sizeof(subscriptions[count]));
        room_service_copy_string(subscriptions[count].subscriber_participant_id,
                                 sizeof(subscriptions[count].subscriber_participant_id),
                                 summary.subscriber_participant_id);
        room_service_copy_string(subscriptions[count].track_id,
                                 sizeof(subscriptions[count].track_id),
                                 summary.track_id);
        subscriptions[count].enabled = summary.enabled;
        subscriptions[count].priority = summary.priority;
        subscriptions[count].preferred_layer = summary.preferred_layer;
        subscriptions[count].target_layer = summary.target_layer;
        subscriptions[count].muted = summary.muted;
        room_service_copy_string(subscriptions[count].policy_source,
                                 sizeof(subscriptions[count].policy_source),
                                 summary.policy_source);
        count++;
    }

    *out_subscriptions = subscriptions;
    *out_count = count;
    return 0;
}

static room_service_room_sync_diagnostic_t *room_service_find_room_sync_diagnostic(
    room_service_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return NULL;
    }

    for (i = 0; i < server->room_sync_diagnostic_count; ++i) {
        if (strcmp(server->room_sync_diagnostics[i].room_id, room_id) == 0) {
            return &server->room_sync_diagnostics[i];
        }
    }

    return NULL;
}

static void room_service_prune_call_center_events_locked(
    room_service_app_server_t *server) {
    int excess;

    if (!server || server->call_center_event_count <= ROOM_SERVICE_MAX_CALL_CENTER_EVENTS) {
        return;
    }

    excess = server->call_center_event_count - ROOM_SERVICE_MAX_CALL_CENTER_EVENTS;
    memmove(server->call_center_events,
            server->call_center_events + excess,
            sizeof(*server->call_center_events) *
                (size_t)(server->call_center_event_count - excess));
    server->call_center_event_count -= excess;
}

static char *room_service_trim_token(char *value) {
    char *end;

    if (!value) {
        return NULL;
    }
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';
    return value;
}

static room_service_sfu_node_entry_t *room_service_find_sfu_node_locked(
    room_service_app_server_t *server, const char *node_id) {
    int i;

    if (!server || !node_id || node_id[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < server->sfu_node_count; ++i) {
        if (strcmp(server->sfu_nodes[i].node_id, node_id) == 0) {
            return &server->sfu_nodes[i];
        }
    }

    return NULL;
}

int room_service_app_server_register_sfu_node(room_service_app_server_t *server,
                                              const char *node_id,
                                              const char *control_url,
                                              const char *control_token) {
    room_service_sfu_node_entry_t *entry;

    if (!server || !node_id || node_id[0] == '\0' || !control_url ||
        control_url[0] == '\0') {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    entry = room_service_find_sfu_node_locked(server, node_id);
    if (!entry) {
        if (room_service_ensure_capacity((void **)&server->sfu_nodes,
                                         &server->sfu_node_capacity,
                                         sizeof(*server->sfu_nodes),
                                         server->sfu_node_count + 1) != 0) {
            salts_mutex_unlock(&server->mutex);
            return -1;
        }
        entry = &server->sfu_nodes[server->sfu_node_count++];
        memset(entry, 0, sizeof(*entry));
        room_service_copy_string(entry->node_id, sizeof(entry->node_id), node_id);
    }
    room_service_copy_string(entry->control_url, sizeof(entry->control_url), control_url);
    room_service_copy_string(entry->control_token, sizeof(entry->control_token),
                             control_token);
    salts_mutex_unlock(&server->mutex);
    return 0;
}

int room_service_app_server_has_sfu_node(room_service_app_server_t *server,
                                         const char *node_id) {
    int found;

    if (!server || !node_id || node_id[0] == '\0') {
        return 0;
    }

    salts_mutex_lock(&server->mutex);
    found = room_service_find_sfu_node_locked(server, node_id) != NULL;
    salts_mutex_unlock(&server->mutex);
    return found;
}

int room_service_app_server_choose_sfu_node(room_service_app_server_t *server,
                                            char *node_id, size_t node_id_size) {
    int index;

    if (!server || !node_id || node_id_size == 0) {
        return -1;
    }

    node_id[0] = '\0';
    salts_mutex_lock(&server->mutex);
    if (server->sfu_node_count > 0) {
        index = server->next_sfu_node_index % server->sfu_node_count;
        server->next_sfu_node_index++;
        room_service_copy_string(node_id, node_id_size, server->sfu_nodes[index].node_id);
        salts_mutex_unlock(&server->mutex);
        return 0;
    }
    salts_mutex_unlock(&server->mutex);

    if (server->config.sfu_control_url && server->config.sfu_control_url[0] != '\0') {
        room_service_copy_string(node_id, node_id_size, "default-sfu");
        return 0;
    }

    return -1;
}

static int room_service_register_sfu_nodes_from_config(
    room_service_app_server_t *server, const char *nodes) {
    char *copy;
    char *cursor;
    char *entry;

    if (!server || !nodes || nodes[0] == '\0') {
        return 0;
    }

    copy = (char *)malloc(strlen(nodes) + 1);
    if (!copy) {
        return -1;
    }
    strcpy(copy, nodes);

    cursor = copy;
    while ((entry = cursor) != NULL && *entry != '\0') {
        char *comma = strchr(entry, ',');
        char *equals;
        char *node_id;
        char *url;

        if (comma) {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = NULL;
        }

        entry = room_service_trim_token(entry);
        if (!entry || entry[0] == '\0') {
            continue;
        }

        equals = strchr(entry, '=');
        if (!equals) {
            free(copy);
            return -1;
        }
        *equals = '\0';
        node_id = room_service_trim_token(entry);
        url = room_service_trim_token(equals + 1);
        if (!node_id || node_id[0] == '\0' || !url || url[0] == '\0' ||
            room_service_app_server_register_sfu_node(server, node_id, url,
                                                      server->config.sfu_control_token) != 0) {
            free(copy);
            return -1;
        }
    }

    free(copy);
    return 0;
}

static int room_service_copy_sfu_route_for_room(room_service_app_server_t *server,
                                                const char *room_id,
                                                char *control_url,
                                                size_t control_url_size,
                                                char *control_token,
                                                size_t control_token_size) {
    turbo_room_summary_t room_summary;
    turbo_room_service_t *service;
    room_service_sfu_node_entry_t *entry;
    int sfu_node_count;

    if (!server || !room_id || !control_url || control_url_size == 0 ||
        !control_token || control_token_size == 0) {
        return -1;
    }

    control_url[0] = '\0';
    control_token[0] = '\0';
    service = room_service_app_server_get_service(server);
    if (!service ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        room_summary.assigned_sfu_node[0] == '\0') {
        if (server->config.sfu_control_url && server->config.sfu_control_url[0] != '\0') {
            room_service_copy_string(control_url, control_url_size,
                                     server->config.sfu_control_url);
            room_service_copy_string(control_token, control_token_size,
                                     server->config.sfu_control_token);
        }
        return 0;
    }

    salts_mutex_lock(&server->mutex);
    sfu_node_count = server->sfu_node_count;
    entry = room_service_find_sfu_node_locked(server, room_summary.assigned_sfu_node);
    if (entry) {
        room_service_copy_string(control_url, control_url_size, entry->control_url);
        room_service_copy_string(control_token, control_token_size, entry->control_token);
        salts_mutex_unlock(&server->mutex);
        return 0;
    }
    salts_mutex_unlock(&server->mutex);

    if (sfu_node_count == 0 && server->config.sfu_control_url &&
        server->config.sfu_control_url[0] != '\0') {
        room_service_copy_string(control_url, control_url_size,
                                 server->config.sfu_control_url);
        room_service_copy_string(control_token, control_token_size,
                                 server->config.sfu_control_token);
        return 0;
    }

    return -1;
}

static void room_service_http_response_free(chttp_response *response) {
    if (!response) return;
    chttp_response_destroy(response);
    free(response);
}

static int room_service_http_response_is_json(const chttp_response *response) {
    static const char json_type[] = "application/json";
    const char *content_type = response
                                   ? chttp_response_header(response,
                                                           "Content-Type")
                                   : NULL;
    size_t index;
    if (!content_type) return 0;
    for (index = 0u; index + 1u < sizeof(json_type); ++index) {
        if (tolower((unsigned char)content_type[index]) != json_type[index])
            return 0;
    }
    return content_type[index] == '\0' || content_type[index] == ';' ||
           isspace((unsigned char)content_type[index]);
}

static json_value_t *room_service_http_response_parse_json(
    const chttp_response *response) {
    if (!response || !response->body || response->body_size == 0u) return NULL;
    return json_parse((const char *)response->body, response->body_size);
}

static chttp_response *room_service_http_post_json(
    turbo_transport_t *client, const char *path, const char *json) {
    const char *headers[] = {"Content-Type", "application/json"};
    if (!client || !path || !json) return NULL;
    return turbo_transport_http_request(
        client, TURBO_HTTP_POST, path, (const uint8_t *)json, strlen(json),
        headers, 2);
}

static turbo_transport_t *room_service_create_sfu_client_for_route(
    room_service_app_server_t *server, const char *control_url,
    const char *control_token) {
    turbo_transport_config_t config = {0};
    turbo_transport_t *client;
    cnet_tls_client_config tls_config = {0};
    const char *ca_file;

    if (!control_url || control_url[0] == '\0') {
        return NULL;
    }

    if (turbo_transport_parse_url(control_url, &config) != 0 ||
        config.type != TURBO_TRANSPORT_HTTP) {
        free((void *)config.host);
        free((void *)config.path);
        return NULL;
    }

    ca_file = server ? server->config.sfu_ca_file : NULL;
    if (config.use_tls) {
        tls_config.size = sizeof(tls_config);
        tls_config.ca_file = ca_file;
        config.tls = &tls_config;
    } else if (ca_file && ca_file[0]) {
        free((void *)config.host);
        free((void *)config.path);
        return NULL;
    }

    config.connect_timeout_ms = 3000;
    config.read_timeout_ms = 3000;
    config.write_timeout_ms = 3000;
    config.user_agent = "TurboRoomService/0.1";
    config.auth_token = control_token;
    client = turbo_transport_create(&config);
    free((void *)config.host);
    free((void *)config.path);
    return client;
}

static int room_service_sfu_command_is_dangerous(const char *type) {
    return type &&
           (strcmp(type, "force_close_room") == 0 ||
            strcmp(type, "detach_room") == 0 ||
            strcmp(type, "set_node_drain") == 0 ||
            strcmp(type, "start_recording") == 0 ||
            strcmp(type, "stop_recording") == 0);
}

static int room_service_issue_sfu_command_token(
    room_service_app_server_t *server, const char *room_id,
    const char *command_json, char **out_token) {
    turbo_media_auth_config_t auth = {0};
    turbo_media_auth_claims_t claims = {0};
    json_value_t *root = NULL;
    const char *command_room_id;
    const char *participant_id;
    const char *type;
    int64_t now;

    if (!server || !room_id || !command_json || !out_token) {
        return -1;
    }
    *out_token = NULL;
    if (!server->config.sfu_auth_secret ||
        server->config.sfu_auth_secret[0] == '\0') {
        return 0;
    }
    if (((root = json_parse((const char *)((const uint8_t *)command_json), strlen(command_json))) ? 0 : -1) != 0 ||
        !root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        return -1;
    }
    type = room_service_json_string_field(root, "type");
    command_room_id = room_service_json_string_field(root, "room_id");
    participant_id = room_service_json_string_field(root, "participant_id");
    if (!type || !command_room_id || strcmp(command_room_id, room_id) != 0) {
        json_free(root);
        root = NULL;
        return -1;
    }

    auth.issuer = server->config.sfu_auth_issuer;
    auth.active_key_id = server->config.sfu_auth_key_id;
    auth.active_secret = server->config.sfu_auth_secret;
    auth.clock_skew_seconds = 0;
    auth.max_ttl_seconds = server->config.sfu_auth_ttl_seconds;
    now = (int64_t)time(NULL);
    claims.subject = server->config.node_id;
    claims.audience = ROOM_SERVICE_SFU_CONTROL_AUDIENCE;
    claims.scope = room_service_sfu_command_is_dangerous(type)
                       ? ROOM_SERVICE_SFU_SCOPE_CONTROL_DANGEROUS
                       : ROOM_SERVICE_SFU_SCOPE_CONTROL_WRITE;
    claims.room_id = command_room_id;
    claims.participant_id = participant_id;
    claims.issued_at = now;
    claims.expires_at = now + server->config.sfu_auth_ttl_seconds;
    *out_token = turbo_media_auth_issue(&auth, &claims);
    json_free(root);
    root = NULL;
    return *out_token ? 0 : -1;
}

static int room_service_post_sfu_command(room_service_app_server_t *server,
                                         const char *room_id,
                                         const char *command_json) {
    turbo_transport_t *client;
    chttp_response *response;
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];
    char *signed_token = NULL;
    const char *request_token;

    if (!server || !room_id || !command_json) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }
    if (room_service_issue_sfu_command_token(server, room_id, command_json,
                                             &signed_token) != 0) {
        return -1;
    }
    request_token = signed_token ? signed_token : control_token;

    client = room_service_create_sfu_client_for_route(
        server, control_url, request_token);
    free(signed_token);
    if (!client) {
        return -1;
    }

    response = room_service_http_post_json(client, "/api/v1/commands",
                                           command_json);
    if (!response) {
        turbo_transport_destroy(client);
        return -1;
    }

    if (response->status_code < 200u || response->status_code >= 300u) {
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    room_service_http_response_free(response);
    turbo_transport_destroy(client);
    return 0;
}

static turbo_room_video_layer_t room_service_parse_video_layer(const char *value) {
    if (!value || strcmp(value, "none") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }
    if (strcmp(value, "low") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_LOW;
    }
    if (strcmp(value, "medium") == 0 || strcmp(value, "mid") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_MEDIUM;
    }
    if (strcmp(value, "high") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_HIGH;
    }
    return 0;
}

static const char *room_service_video_layer_name(turbo_room_video_layer_t layer) {
    switch (layer) {
        case TURBO_ROOM_VIDEO_LAYER_NONE: return "none";
        case TURBO_ROOM_VIDEO_LAYER_LOW: return "low";
        case TURBO_ROOM_VIDEO_LAYER_MEDIUM: return "medium";
        case TURBO_ROOM_VIDEO_LAYER_HIGH: return "high";
        default: return "unknown";
    }
}

static turbo_room_video_layer_t room_service_resolve_subscription_max_layer(
    const turbo_room_subscription_summary_t *subscription) {
    if (!subscription || !subscription->enabled || subscription->muted) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }

    if (subscription->target_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->target_layer;
    }
    if (subscription->preferred_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->preferred_layer;
    }

    return TURBO_ROOM_VIDEO_LAYER_HIGH;
}

static const char *room_service_json_string_field(const json_value_t *obj, const char *key) {
    json_value_t *value;

    if (!obj || !key) {
        return NULL;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_STRING) {
        return NULL;
    }

    return json_string(value);
}

static int room_service_json_bool_field(const json_value_t *obj, const char *key, int def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_BOOL) {
        return def;
    }

    return json_bool(value) ? 1 : 0;
}

static int64_t room_service_json_int64_field(const json_value_t *obj, const char *key,
                                             int64_t def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_NUMBER) {
        return def;
    }

    return (int64_t)json_number(value);
}

#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
static ivr_status_t room_service_prepare_ivr_caller_audio(
    void *context, const char *room_id, const char *call_id, char *error,
    size_t error_capacity) {
    room_service_app_server_t *server =
        (room_service_app_server_t *)context;
    turbo_room_summary_t room;
    turbo_room_track_summary_t selected;
    turbo_room_participant_summary_t receiver;
    char receiver_id[TURBO_PARTICIPANT_ID_MAX];
    int selected_count = 0;
    int receiver_length;

    if (!server || !room_id || !call_id || !error || error_capacity == 0) {
        return IVR_EINVAL;
    }
    error[0] = '\0';
    receiver_length = snprintf(receiver_id, sizeof(receiver_id), "%s-rx",
                               call_id);
    if (receiver_length < 0 ||
        (size_t)receiver_length >= sizeof(receiver_id)) {
        snprintf(error, error_capacity, "IVR receiver participant id too long");
        return IVR_EINVAL;
    }
    if (turbo_room_service_get_room_summary(server->service, room_id,
                                            &room) != 0) {
        snprintf(error, error_capacity, "room not found: %s", room_id);
        return IVR_ESTATE;
    }
    memset(&selected, 0, sizeof(selected));
    for (int i = 0; i < room.published_track_count; ++i) {
        turbo_room_track_summary_t track;
        if (turbo_room_service_get_track_summary_at(server->service, room_id,
                                                    i, &track) != 0) {
            snprintf(error, error_capacity,
                     "failed to inspect caller media tracks");
            return IVR_ESTATE;
        }
        if (strcmp(track.owner_participant_id, call_id) == 0 &&
            track.kind == TURBO_ROOM_TRACK_AUDIO && !track.muted) {
            selected = track;
            selected_count++;
        }
    }
    if (selected_count != 1) {
        snprintf(error, error_capacity,
                 "caller %s has %d eligible audio tracks; expected exactly one",
                 call_id, selected_count);
        return IVR_ESTATE;
    }

    if (turbo_room_service_get_participant_summary(
            server->service, room_id, receiver_id, &receiver) != 0) {
        turbo_room_participant_config_t participant;
        memset(&participant, 0, sizeof(participant));
        participant.participant_id = receiver_id;
        participant.user_id = "";
        participant.display_name = receiver_id;
        participant.role = TURBO_PARTICIPANT_ROLE_BOT;
        if (turbo_room_service_add_participant(server->service, room_id,
                                               &participant) != 0) {
            snprintf(error, error_capacity,
                     "failed to create IVR receiver participant %s",
                     receiver_id);
            return IVR_ESTATE;
        }
    } else if (receiver.role != TURBO_PARTICIPANT_ROLE_BOT) {
        snprintf(error, error_capacity,
                 "IVR receiver participant %s has incompatible role",
                 receiver_id);
        return IVR_ESTATE;
    }

    turbo_room_subscription_config_t subscription;
    turbo_room_subscription_summary_t committed;
    memset(&subscription, 0, sizeof(subscription));
    subscription.subscriber_participant_id = receiver_id;
    subscription.track_id = selected.track_id;
    subscription.enabled = 1;
    subscription.muted = 0;
    subscription.policy_source = "ivr";
    if (turbo_room_service_set_subscription(server->service, room_id,
                                            &subscription) != 0) {
        snprintf(error, error_capacity,
                 "failed to commit IVR audio subscription");
        return IVR_ESTATE;
    }
    if (turbo_room_service_get_subscription_summary(
            server->service, room_id, receiver_id, selected.track_id,
            &committed) != 0 ||
        room_service_app_server_sync_apply_track_subscription(
            server, room_id, &committed) != 0) {
        /* The authoritative desired subscription remains committed and can be
           replayed after SFU recovery; this attempt is not dispatched. */
        snprintf(error, error_capacity,
                 "IVR audio subscription was committed but SFU sync failed");
        return IVR_ESTATE;
    }
    return IVR_OK;
}

static ivr_status_t room_service_release_ivr_caller_audio(
    void *context, const char *room_id, const char *call_id, char *error,
    size_t error_capacity) {
    room_service_app_server_t *server =
        (room_service_app_server_t *)context;
    turbo_room_summary_t room;
    turbo_room_participant_summary_t receiver;
    char receiver_id[TURBO_PARTICIPANT_ID_MAX];
    int receiver_length;

    if (!server || !room_id || !call_id || !error || error_capacity == 0) {
        return IVR_EINVAL;
    }
    error[0] = '\0';
    receiver_length = snprintf(receiver_id, sizeof(receiver_id), "%s-rx",
                               call_id);
    if (receiver_length < 0 ||
        (size_t)receiver_length >= sizeof(receiver_id)) {
        snprintf(error, error_capacity, "IVR receiver participant id too long");
        return IVR_EINVAL;
    }
    if (turbo_room_service_get_participant_summary(
            server->service, room_id, receiver_id, &receiver) != 0) {
        return IVR_OK;
    }
    if (receiver.role != TURBO_PARTICIPANT_ROLE_BOT ||
        turbo_room_service_get_room_summary(server->service, room_id,
                                            &room) != 0) {
        snprintf(error, error_capacity,
                 "IVR receiver participant %s has incompatible state",
                 receiver_id);
        return IVR_ESTATE;
    }

    for (int i = room.subscription_count - 1; i >= 0; --i) {
        turbo_room_subscription_summary_t subscription;
        if (turbo_room_service_get_subscription_summary_at(
                server->service, room_id, i, &subscription) != 0) {
            snprintf(error, error_capacity,
                     "failed to inspect IVR audio subscriptions");
            return IVR_ESTATE;
        }
        if (strcmp(subscription.subscriber_participant_id, receiver_id) != 0) {
            continue;
        }
        subscription.enabled = 0;
        subscription.muted = 1;
        if (room_service_app_server_sync_apply_track_subscription(
                server, room_id, &subscription) != 0) {
            snprintf(error, error_capacity,
                     "failed to disable IVR audio subscription in SFU");
            return IVR_ESTATE;
        }
        if (turbo_room_service_remove_subscription(
                server->service, room_id, receiver_id,
                subscription.track_id) != 0) {
            snprintf(error, error_capacity,
                     "failed to remove IVR audio subscription");
            return IVR_ESTATE;
        }
    }
    if (turbo_room_service_remove_participant(server->service, room_id,
                                              receiver_id) != 0) {
        snprintf(error, error_capacity,
                 "failed to remove IVR receiver participant %s", receiver_id);
        return IVR_ESTATE;
    }
    return IVR_OK;
}

static ivr_status_t room_service_send_iris_media_command(
    void *context, const ivr_media_command_t *command,
    char *out_media_worker_id, size_t out_media_worker_id_capacity) {
    return room_service_app_server_send_ivr_media_command(
        (room_service_app_server_t *)context, command, out_media_worker_id,
        out_media_worker_id_capacity);
}

static ivr_status_t room_service_observe_iris_media_command(
    void *context, const ivr_media_command_t *command,
    iris_resource_observation_t *observation) {
    room_service_app_server_t *server = (room_service_app_server_t *)context;
    if (!server || !server->ivr_control || !command || !observation) {
        return IVR_ESTATE;
    }
    return ivr_control_adapter_observe_media_command(server->ivr_control, command,
                                                  observation);
}

static turbo_participant_role_t room_service_iris_room_role(
    const char *role) {
    if (!role || !role[0] || strcmp(role, "guest") == 0) {
        return TURBO_PARTICIPANT_ROLE_GUEST;
    }
    if (strcmp(role, "host") == 0) return TURBO_PARTICIPANT_ROLE_HOST;
    if (strcmp(role, "cohost") == 0) return TURBO_PARTICIPANT_ROLE_COHOST;
    if (strcmp(role, "audience") == 0) return TURBO_PARTICIPANT_ROLE_AUDIENCE;
    if (strcmp(role, "customer") == 0) return TURBO_PARTICIPANT_ROLE_CUSTOMER;
    if (strcmp(role, "agent") == 0) return TURBO_PARTICIPANT_ROLE_AGENT;
    if (strcmp(role, "supervisor") == 0) return TURBO_PARTICIPANT_ROLE_SUPERVISOR;
    if (strcmp(role, "qa_observer") == 0) return TURBO_PARTICIPANT_ROLE_QA_OBSERVER;
    if (strcmp(role, "bot") == 0) return TURBO_PARTICIPANT_ROLE_BOT;
    return 0;
}

static int room_service_iris_add_json(json_value_t *object, const char *key,
                                      json_value_t *value) {
    if (!value || !json_object_add_checked(object, key, value)) {
        json_free(value);
        value = NULL;
        return 0;
    }
    return 1;
}

static int room_service_iris_room_execution(
    iris_room_execution_t *result, iris_room_terminal_status_t status,
    const char *event_type, const char *code, const char *message,
    const char *room_id, const char *call_id,
    const turbo_room_summary_t *summary, const char *warning_code,
    const char *warning_message) {
    json_value_t *data = json_create_object();
    char *json;
    size_t json_size = 0u;
    int valid;
    if (!result || !event_type || !data) {
        json_free(data);
        data = NULL;
        return -1;
    }
    valid = room_service_iris_add_json(
                data, "roomId", json_create_string(room_id ? room_id : "")) &&
            (!call_id || room_service_iris_add_json(
                             data, "callId", json_create_string(call_id))) &&
            (!summary ||
             (room_service_iris_add_json(
                  data, "roomVersion", json_create_int64(summary->version)) &&
              room_service_iris_add_json(
                  data, "participantCount",
                  json_create_int64(summary->participant_count)))) &&
            (!code || room_service_iris_add_json(
                         data, "code", json_create_string(code))) &&
            (!message || room_service_iris_add_json(
                            data, "message", json_create_string(message))) &&
            (!warning_code || room_service_iris_add_json(
                                 data, "warningCode",
                                 json_create_string(warning_code))) &&
            (!warning_message || room_service_iris_add_json(
                                    data, "warning",
                                    json_create_string(warning_message)));
    if (!valid) {
        json_free(data);
        data = NULL;
        return -1;
    }
    json = json_serialize(data, &json_size);
    json_free(data);
    data = NULL;
    if (!json || json_size == 0u || json_size >= sizeof(result->data) ||
        strlen(event_type) >= sizeof(result->event_type)) {
        json_serialize_free(json);
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->terminal_status = status;
    memcpy(result->event_type, event_type, strlen(event_type) + 1u);
    memcpy(result->data, json, json_size);
    result->data[json_size] = '\0';
    json_serialize_free(json);
    return 0;
}

static int room_service_iris_absent_execution(
    iris_room_execution_t *result, const iris_room_command_t *command,
    const char *event_type, const char *code, const char *message) {
    int rc;
    if (!result || !command) return -1;
    rc = room_service_iris_room_execution(
        result, IRIS_ROOM_TERMINAL_FAILED, event_type, code, message,
        command->room_id,
        command->kind == IRIS_ROOM_COMMAND_UNJOIN ? command->call_id : NULL,
        NULL, NULL, NULL);
    if (rc == 0) result->resource_absent = 1;
    return rc;
}

static int room_service_execute_iris_room_command(
    void *context, const iris_room_command_t *command,
    iris_room_execution_t *result) {
    room_service_app_server_t *server = (room_service_app_server_t *)context;
    turbo_room_summary_t summary;
    char room_id[TURBO_ROOM_ID_MAX] = {0};
    char call_id[TURBO_PARTICIPANT_ID_MAX] = {0};
    const char *warning_code = NULL;
    const char *warning_message = NULL;
    if (!server || !command || !result) return -1;

    if (command->kind == IRIS_ROOM_COMMAND_CREATE) {
        turbo_room_config_t config;
        if (turbo_room_service_get_room_summary(
                server->service, command->room_id, &summary) == 0) {
            return room_service_iris_room_execution(
                result, IRIS_ROOM_TERMINAL_FAILED,
                "provider.conference.failed", "ROOM_ALREADY_EXISTS",
                "conference room already exists", command->room_id, NULL,
                &summary, NULL, NULL);
        }
        memset(&config, 0, sizeof(config));
        config.room_id = command->room_id;
        config.room_generation = command->room_generation;
        config.room_type = TURBO_ROOM_TYPE_CONFERENCE;
        config.created_by = command->provider_session_id;
        if (turbo_room_service_create_room(server->service, &config) != 0 ||
            turbo_room_service_get_room_summary(
                server->service, command->room_id, &summary) != 0) {
            return room_service_iris_room_execution(
                result, IRIS_ROOM_TERMINAL_FAILED,
                "provider.conference.failed", "ROOM_CREATE_FAILED",
                "conference room could not be created", command->room_id,
                NULL, NULL, NULL, NULL);
        }
        return room_service_iris_room_execution(
            result, IRIS_ROOM_TERMINAL_SUCCEEDED,
            "provider.conference.created", NULL, NULL, command->room_id,
            NULL, &summary, NULL, NULL);
    }

    if (command->kind == IRIS_ROOM_COMMAND_DESTROY) {
        if (turbo_room_service_get_room_summary(
                server->service, command->room_id, &summary) != 0) {
            return room_service_iris_absent_execution(
                result, command, "provider.conference.failed",
                "ROOM_NOT_FOUND", "conference room does not exist");
        }
        if (summary.status == TURBO_ROOM_STATUS_CLOSED) {
            return room_service_iris_absent_execution(
                result, command, "provider.conference.failed",
                "ROOM_ALREADY_CLOSED", "conference room is already closed");
        }
        if (summary.participant_count != 0) {
            return room_service_iris_room_execution(
                result, IRIS_ROOM_TERMINAL_FAILED,
                "provider.conference.failed", "ROOM_NOT_EMPTY",
                "conference room still has participants", command->room_id,
                NULL, &summary, NULL, NULL);
        }
        if (turbo_room_service_close_room(server->service,
                                          command->room_id) != 0 ||
            turbo_room_service_get_room_summary(
                server->service, command->room_id, &summary) != 0) {
            return room_service_iris_room_execution(
                result, IRIS_ROOM_TERMINAL_FAILED,
                "provider.conference.failed", "ROOM_CLOSE_FAILED",
                "conference room could not be closed", command->room_id,
                NULL, NULL, NULL, NULL);
        }
        if (summary.assigned_sfu_node[0] &&
            room_service_app_server_sync_force_close_room(
                server, command->room_id) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "room state committed locally; force_close_room was not forwarded to sfu node";
        }
        return room_service_iris_room_execution(
            result, IRIS_ROOM_TERMINAL_SUCCEEDED,
            "provider.conference.destroyed", NULL, NULL, command->room_id,
            NULL, &summary, warning_code, warning_message);
    }

    if (strlen(command->room_id) >= sizeof(room_id) ||
        strlen(command->call_id) >= sizeof(call_id)) {
        return room_service_iris_room_execution(
            result, IRIS_ROOM_TERMINAL_FAILED,
            "provider.connection.failed", "MEMBERSHIP_IDS_INVALID",
            "membership resource identity is invalid",
            command->room_id, command->call_id, NULL, NULL, NULL);
    }
    memcpy(room_id, command->room_id, strlen(command->room_id) + 1u);
    memcpy(call_id, command->call_id, strlen(command->call_id) + 1u);

    if (command->kind == IRIS_ROOM_COMMAND_JOIN) {
        turbo_room_participant_config_t participant;
        turbo_participant_role_t role =
            room_service_iris_room_role(command->role);
        if (role == 0) {
            return room_service_iris_room_execution(
                result, IRIS_ROOM_TERMINAL_FAILED,
                "provider.connection.failed", "PARTICIPANT_ROLE_INVALID",
                "membership role is invalid", room_id, call_id, NULL,
                NULL, NULL);
        }
        memset(&participant, 0, sizeof(participant));
        participant.participant_id = call_id;
        participant.call_generation = command->call_generation;
        participant.user_id = command->user_id;
        participant.display_name = command->display_name[0]
                                       ? command->display_name
                                       : call_id;
        participant.role = role;
        if (turbo_room_service_add_participant(
                server->service, room_id, &participant) != 0 ||
            turbo_room_service_get_room_summary(
                server->service, room_id, &summary) != 0) {
            return room_service_iris_room_execution(
                result, IRIS_ROOM_TERMINAL_FAILED,
                "provider.connection.failed", "MEMBERSHIP_JOIN_FAILED",
                "call could not join the conference room", room_id, call_id,
                NULL, NULL, NULL);
        }
        if (summary.assigned_sfu_node[0] &&
            room_service_app_server_sync_add_session(
                server, room_id, call_id) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "participant state committed locally; add_session was not forwarded to sfu node";
        }
        return room_service_iris_room_execution(
            result, IRIS_ROOM_TERMINAL_SUCCEEDED,
            "provider.connection.joined", NULL, NULL, room_id, call_id,
            &summary, warning_code, warning_message);
    }

    {
        turbo_room_participant_summary_t participant;
        if (turbo_room_service_get_room_summary(
                server->service, room_id, &summary) != 0 ||
            turbo_room_service_get_participant_summary(
                server->service, room_id, call_id, &participant) != 0) {
            return room_service_iris_absent_execution(
                result, command, "provider.connection.failed",
                "MEMBERSHIP_NOT_FOUND",
                "conference membership does not exist");
        }
    }
    if (turbo_room_service_remove_participant(
            server->service, room_id, call_id) != 0 ||
        turbo_room_service_get_room_summary(
            server->service, room_id, &summary) != 0) {
        return room_service_iris_room_execution(
            result, IRIS_ROOM_TERMINAL_FAILED,
            "provider.connection.failed", "MEMBERSHIP_UNJOIN_FAILED",
            "call could not leave the conference room", room_id, call_id,
            NULL, NULL, NULL);
    }
    if (summary.assigned_sfu_node[0] &&
        room_service_app_server_sync_remove_session(
            server, room_id, call_id) != 0) {
        warning_code = "SFU_SYNC_FAILED";
        warning_message =
            "participant state committed locally; remove_session was not forwarded to sfu node";
    }
    return room_service_iris_room_execution(
        result, IRIS_ROOM_TERMINAL_SUCCEEDED,
        "provider.connection.unjoined", NULL, NULL, room_id, call_id,
        &summary, warning_code, warning_message);
}

static ivr_status_t room_service_observe_iris_room_command(
    void *context, const iris_room_command_t *command,
    iris_resource_observation_t *observation) {
    room_service_app_server_t *server = (room_service_app_server_t *)context;
    turbo_room_summary_t room;

    if (!server || !server->service || !command || !observation) {
        return IVR_ESTATE;
    }
    memset(observation, 0, sizeof(*observation));
    observation->state = IRIS_RESOURCE_OBSERVATION_ABSENT;

    if (turbo_room_service_get_room_summary(
            server->service, command->room_id, &room) != 0 ||
        room.room_generation != command->room_generation ||
        room.status == TURBO_ROOM_STATUS_CLOSING ||
        room.status == TURBO_ROOM_STATUS_CLOSED) {
        return IVR_OK;
    }

    if (command->kind == IRIS_ROOM_COMMAND_CREATE ||
        command->kind == IRIS_ROOM_COMMAND_DESTROY) {
        observation->state = IRIS_RESOURCE_OBSERVATION_ACTIVE;
        return IVR_OK;
    }

    if (command->kind == IRIS_ROOM_COMMAND_JOIN ||
        command->kind == IRIS_ROOM_COMMAND_UNJOIN) {
        turbo_room_participant_summary_t participant;
        if (turbo_room_service_get_participant_summary(
                server->service, command->room_id, command->call_id,
                &participant) == 0 &&
            participant.call_generation == command->call_generation) {
            observation->state = IRIS_RESOURCE_OBSERVATION_ACTIVE;
        }
        return IVR_OK;
    }

    observation->state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
    return IVR_EINVAL;
}
#endif

room_service_app_server_t *room_service_app_server_create(
    const room_service_app_config_t *config) {
    room_service_app_server_t *server;

    if (!config || room_service_app_config_validate(config) != 0) {
        return NULL;
    }

    server = (room_service_app_server_t *)calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    memcpy(&server->config, config, sizeof(*config));
    atomic_init(&server->running, 0);
    salts_mutex_init(&server->mutex);
    server->service = turbo_room_service_create();
    if (!server->service) {
        salts_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }
    if (room_service_register_sfu_nodes_from_config(server, config->sfu_nodes) != 0) {
        turbo_room_service_destroy(server->service);
        free(server->sfu_nodes);
        salts_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    if (config->iris_control_host) {
        iris_media_bridge_config_t bridge_config;
        iris_room_bridge_config_t room_bridge_config;
        iris_control_provider_config_t provider_config;
        iris_completion_dispatcher_config_t dispatcher_config;
        char ledger_error[256] = {0};
        server->iris_command_ledger = iris_command_ledger_create_record_store(
            config->iris_event_store_config,
            config->iris_command_ledger_channel,
            config->iris_allow_development_sqlite,
            (size_t)config->iris_command_ledger_queue_capacity,
            (size_t)config->iris_command_retention_batch_size,
            (uint64_t)config->iris_command_terminal_retention_seconds *
                UINT64_C(1000),
            (uint64_t)config->iris_retention_sweep_interval_ms,
            ledger_error, sizeof(ledger_error));
        if (!server->iris_command_ledger) {
            if (ledger_error[0]) {
                TLOG_ERRORF("Iris command ledger initialization failed: {}",
                           ledger_error);
            }
            turbo_room_service_destroy(server->service);
            free(server->sfu_nodes);
            salts_mutex_destroy(&server->mutex);
            free(server);
            return NULL;
        }
        memset(&bridge_config, 0, sizeof(bridge_config));
        bridge_config.correlation_capacity =
            (size_t)config->iris_correlation_capacity;
        bridge_config.send = room_service_send_iris_media_command;
        bridge_config.send_context = server;
        bridge_config.observe = room_service_observe_iris_media_command;
        bridge_config.observe_context = server;
        bridge_config.ledger =
            iris_command_ledger_port(server->iris_command_ledger);
        server->iris_media_bridge = iris_media_bridge_create(&bridge_config);
        if (!server->iris_media_bridge) {
            iris_command_ledger_destroy(server->iris_command_ledger);
            turbo_room_service_destroy(server->service);
            free(server->sfu_nodes);
            salts_mutex_destroy(&server->mutex);
            free(server);
            return NULL;
        }
        memset(&room_bridge_config, 0, sizeof(room_bridge_config));
        room_bridge_config.capacity =
            (size_t)config->iris_correlation_capacity;
        room_bridge_config.execute = room_service_execute_iris_room_command;
        room_bridge_config.execute_context = server;
        room_bridge_config.observe = room_service_observe_iris_room_command;
        room_bridge_config.observe_context = server;
        room_bridge_config.ledger =
            iris_command_ledger_port(server->iris_command_ledger);
        server->iris_room_bridge = iris_room_bridge_create(&room_bridge_config);
        if (!server->iris_room_bridge) {
            iris_media_bridge_destroy(server->iris_media_bridge);
            server->iris_media_bridge = NULL;
            iris_command_ledger_destroy(server->iris_command_ledger);
            server->iris_command_ledger = NULL;
            turbo_room_service_destroy(server->service);
            free(server->sfu_nodes);
            salts_mutex_destroy(&server->mutex);
            free(server);
            return NULL;
        }
        iris_control_provider_config_init(&provider_config);
        provider_config.use_tls = config->iris_control_use_tls;
        provider_config.host = config->iris_control_host;
        provider_config.port = (uint16_t)config->iris_control_port;
        provider_config.path = config->iris_control_path;
        provider_config.provider_instance_id =
            config->iris_provider_instance_id;
        provider_config.iris_identity = config->iris_identity;
        provider_config.ca_file = config->iris_control_ca_file;
        provider_config.certificate_file = config->iris_control_cert_file;
        provider_config.private_key_file = config->iris_control_key_file;
        provider_config.private_key_password =
            config->iris_control_key_password;
        provider_config.server_name = config->iris_control_server_name;
        provider_config.allow_insecure_development_loopback =
            config->iris_control_allow_insecure_loopback;
        provider_config.maximum_ingress_messages =
            (size_t)config->iris_correlation_capacity;
        provider_config.send_queue_capacity =
            (size_t)config->iris_completion_queue_capacity;
        provider_config.dispatch = room_service_dispatch_control_ws_command;
        provider_config.dispatch_context = server;
        server->iris_control_provider =
            iris_control_provider_create(&provider_config);
        if (!server->iris_control_provider) {
            iris_room_bridge_destroy(server->iris_room_bridge);
            iris_media_bridge_destroy(server->iris_media_bridge);
            iris_command_ledger_destroy(server->iris_command_ledger);
            turbo_room_service_destroy(server->service);
            free(server->sfu_nodes);
            salts_mutex_destroy(&server->mutex);
            free(server);
            return NULL;
        }
        memset(&dispatcher_config, 0, sizeof(dispatcher_config));
        dispatcher_config.queue_capacity =
            (size_t)config->iris_completion_queue_capacity;
        dispatcher_config.retry_max_attempts = config->iris_retry_max_attempts;
        dispatcher_config.retry_backoff_ms = config->iris_retry_backoff_ms;
        dispatcher_config.request_timeout_ms = config->iris_ack_timeout_ms;
        dispatcher_config.drain_timeout_ms = config->iris_drain_timeout_ms;
        dispatcher_config.bridge = server->iris_media_bridge;
        dispatcher_config.deliver_completion =
            room_service_deliver_control_ws_completion;
        dispatcher_config.deliver_event = room_service_deliver_control_ws_event;
        dispatcher_config.deliver_context = server->iris_control_provider;
        server->iris_completion_dispatcher =
            iris_completion_dispatcher_create(&dispatcher_config);
        if (!server->iris_completion_dispatcher) {
            iris_control_provider_destroy(server->iris_control_provider);
            server->iris_control_provider = NULL;
            iris_room_bridge_destroy(server->iris_room_bridge);
            server->iris_room_bridge = NULL;
            iris_media_bridge_destroy(server->iris_media_bridge);
            server->iris_media_bridge = NULL;
            iris_command_ledger_destroy(server->iris_command_ledger);
            server->iris_command_ledger = NULL;
            turbo_room_service_destroy(server->service);
            free(server->sfu_nodes);
            salts_mutex_destroy(&server->mutex);
            free(server);
            return NULL;
        }
        {
            char outbox_error[256] = {0};
            iris_event_outbox_retention_config_t retention;
            memset(&retention, 0, sizeof(retention));
            retention.dead_retention_ms =
                (uint64_t)config->iris_dead_retention_seconds *
                UINT64_C(1000);
            retention.archive_retention_ms =
                (uint64_t)config->iris_archive_retention_seconds *
                UINT64_C(1000);
            retention.sweep_interval_ms =
                (uint32_t)config->iris_retention_sweep_interval_ms;
            retention.sweep_batch_size =
                (size_t)config->iris_retention_sweep_batch_size;
            server->iris_event_outbox = iris_event_outbox_create_record_store(
                config->iris_event_store_config,
                config->iris_event_store_channel,
                config->iris_allow_development_sqlite,
                (size_t)config->iris_outbox_request_queue_capacity,
                &retention,
                room_service_deliver_iris_event,
                server->iris_completion_dispatcher,
                outbox_error, sizeof(outbox_error));
            if (!server->iris_event_outbox ||
                iris_completion_dispatcher_set_event_delivery_observer(
                    server->iris_completion_dispatcher,
                    room_service_settle_iris_event,
                    server->iris_event_outbox) != 0) {
                if (outbox_error[0]) {
                    TLOG_ERRORF("Iris event outbox initialization failed: {}",
                               outbox_error);
                }
                iris_event_outbox_destroy(server->iris_event_outbox);
                iris_completion_dispatcher_destroy(
                    server->iris_completion_dispatcher);
                iris_control_provider_destroy(server->iris_control_provider);
                iris_room_bridge_destroy(server->iris_room_bridge);
                iris_media_bridge_destroy(server->iris_media_bridge);
                iris_command_ledger_destroy(server->iris_command_ledger);
                turbo_room_service_destroy(server->service);
                free(server->sfu_nodes);
                salts_mutex_destroy(&server->mutex);
                free(server);
                return NULL;
            }
        }
    }
#endif
    server->http_api = room_service_http_api_create(server);
    if (!server->http_api) {
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
        iris_event_outbox_destroy(server->iris_event_outbox);
        iris_completion_dispatcher_destroy(server->iris_completion_dispatcher);
        iris_control_provider_destroy(server->iris_control_provider);
        iris_room_bridge_destroy(server->iris_room_bridge);
        iris_media_bridge_destroy(server->iris_media_bridge);
        iris_command_ledger_destroy(server->iris_command_ledger);
#endif
        turbo_room_service_destroy(server->service);
        free(server->sfu_nodes);
        salts_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    if (config->control_ws_bind_port > 0) {
        ivr_control_adapter_config_t control_ws_config;
        cnet_tls_server_config control_ws_tls;
        ivr_certificate_identity_entry_t
            identities[ROOM_SERVICE_CONTROL_MAX_WORKER_IDENTITIES];
        ivr_control_worker_acl_entry_t
            worker_acls[ROOM_SERVICE_CONTROL_MAX_WORKER_IDENTITIES];
        int i;
        memset(&control_ws_config, 0, sizeof(control_ws_config));
        memset(&control_ws_tls, 0, sizeof(control_ws_tls));
        control_ws_config.bind_host = config->control_ws_bind_host;
        control_ws_config.bind_port = config->control_ws_bind_port;
        control_ws_config.path = config->control_ws_path;
        control_ws_config.worker_lease_ms =
            (uint64_t)config->control_ws_worker_lease_ms;
        control_ws_config.worker_capacity = ROOM_SERVICE_IVR_WORKER_CAPACITY;
        control_ws_config.dispatch_deadline_ms =
            (uint64_t)config->control_ws_dispatch_deadline_ms;
        control_ws_config.dialog_capacity = (uint32_t)config->control_ws_dialog_capacity;
        control_ws_config.media.context = server;
        control_ws_config.media.prepare_caller_audio =
            room_service_prepare_ivr_caller_audio;
        control_ws_config.media.release_caller_audio =
            room_service_release_ivr_caller_audio;
        if (server->iris_completion_dispatcher) {
            control_ws_config.media_observer.context = server;
            control_ws_config.media_observer.on_media_result =
                room_service_observe_iris_media_result;
            control_ws_config.media_observer.on_media_event =
                room_service_observe_iris_media_event;
        }
        if (server->iris_control_provider) {
            control_ws_config.inventory_observer.context = server;
            control_ws_config.inventory_observer.on_inventory_page =
                room_service_observe_iris_inventory;
        }
        if (config->control_ws_use_tls) {
            ivr_certificate_identity_config_t identity_config =
                IVR_CERTIFICATE_IDENTITY_CONFIG_INIT;
            memset(identities, 0, sizeof(identities));
            memset(worker_acls, 0, sizeof(worker_acls));
            for (i = 0; i < config->control_ws_worker_identity_count; ++i) {
                identities[i].worker_id =
                    config->control_ws_worker_identities[i].worker_id;
                identities[i].active_certificate_sha256 =
                    config->control_ws_worker_identities[i]
                        .active_certificate_sha256;
                identities[i].previous_certificate_sha256 =
                    config->control_ws_worker_identities[i]
                        .previous_certificate_sha256;
                identities[i].previous_expires_at_ms =
                    config->control_ws_worker_identities[i].previous_expires_at_ms;
                identities[i].generation =
                    config->control_ws_worker_identities[i].generation;
            }
            identity_config.entries = identities;
            identity_config.entry_count =
                (size_t)config->control_ws_worker_identity_count;
            int identity_status = ivr_certificate_identity_create(
                &identity_config, &server->ivr_control_identity);
            if (identity_status != SALTS_OK) {
                fprintf(stderr,
                        "RoomService create failed: control WebSocket identity "
                        "status=%d\n",
                        identity_status);
                room_service_http_api_destroy(server->http_api);
                server->http_api = NULL;
                iris_event_outbox_destroy(server->iris_event_outbox);
                iris_completion_dispatcher_destroy(
                    server->iris_completion_dispatcher);
                iris_control_provider_destroy(server->iris_control_provider);
                iris_room_bridge_destroy(server->iris_room_bridge);
                iris_media_bridge_destroy(server->iris_media_bridge);
                turbo_room_service_destroy(server->service);
                server->service = NULL;
                free(server->sfu_nodes);
                salts_mutex_destroy(&server->mutex);
                free(server);
                return NULL;
            }
            control_ws_tls.ca_file = config->control_ws_ca_file;
            control_ws_tls.cert_file = config->control_ws_cert_file;
            control_ws_tls.key_file = config->control_ws_key_file;
            control_ws_tls.key_password = config->control_ws_key_password;
            control_ws_tls.size = sizeof(control_ws_tls);
            control_ws_tls.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
            control_ws_config.tls = &control_ws_tls;
            control_ws_config.verify_peer_identity =
                ivr_certificate_identity_verify;
            control_ws_config.verify_peer_identity_context =
                server->ivr_control_identity;
        }
        for (i = 0; i < config->control_ws_worker_identity_count; ++i) {
            worker_acls[i].worker_id =
                config->control_ws_worker_identities[i].worker_id;
            worker_acls[i].tenant_id =
                config->control_ws_worker_identities[i].tenant_id;
            worker_acls[i].room_scope =
                config->control_ws_worker_identities[i].room_scope;
            worker_acls[i].call_scope =
                config->control_ws_worker_identities[i].call_scope;
            worker_acls[i].content_capabilities =
                config->control_ws_worker_identities[i].content_capabilities;
        }
        control_ws_config.worker_acls = worker_acls;
        control_ws_config.worker_acl_count =
            (size_t)config->control_ws_worker_identity_count;
        ivr_status_t control_status = ivr_control_adapter_create(
            server->service, &control_ws_config, &server->ivr_control);
        if (control_status != IVR_OK) {
            fprintf(stderr,
                    "RoomService create failed: control WebSocket adapter "
                    "status=%d\n",
                    (int)control_status);
            ivr_certificate_identity_destroy(server->ivr_control_identity);
            server->ivr_control_identity = NULL;
            room_service_http_api_destroy(server->http_api);
            server->http_api = NULL;
            iris_event_outbox_destroy(server->iris_event_outbox);
            iris_completion_dispatcher_destroy(
                server->iris_completion_dispatcher);
            iris_control_provider_destroy(server->iris_control_provider);
            iris_room_bridge_destroy(server->iris_room_bridge);
            iris_media_bridge_destroy(server->iris_media_bridge);
            iris_command_ledger_destroy(server->iris_command_ledger);
            turbo_room_service_destroy(server->service);
            server->service = NULL;
            free(server->sfu_nodes);
            salts_mutex_destroy(&server->mutex);
            free(server);
            return NULL;
        }
        if (server->iris_control_provider) {
            iris_media_reconciler_config_t reconcile_config;
            memset(&reconcile_config, 0, sizeof(reconcile_config));
            reconcile_config.provider = server->iris_control_provider;
            reconcile_config.resource_capacity =
                (size_t)config->iris_correlation_capacity;
            reconcile_config.worker_capacity =
                ROOM_SERVICE_IVR_WORKER_CAPACITY;
            reconcile_config.inventory_queue_capacity =
                (uint32_t)config->iris_reconcile_inventory_queue_capacity;
            reconcile_config.retry_max_attempts =
                (uint32_t)config->iris_retry_max_attempts;
            reconcile_config.retry_backoff_ms =
                (uint32_t)config->iris_retry_backoff_ms;
            reconcile_config.request_timeout_ms =
                (uint32_t)config->iris_ack_timeout_ms;
            reconcile_config.drain_timeout_ms =
                (uint32_t)config->iris_drain_timeout_ms;
            reconcile_config.inventory_page_size =
                ROOM_SERVICE_RECONCILE_INVENTORY_PAGE_SIZE;
            reconcile_config.close_deadline_ms =
                (uint64_t)config->iris_ack_timeout_ms;
            reconcile_config.event_outbox = server->iris_event_outbox;
            server->iris_media_reconciler =
                iris_media_reconciler_create(&reconcile_config);
            if (!server->iris_media_reconciler ||
                iris_media_reconciler_set_adapter(
                    server->iris_media_reconciler, server->ivr_control) != 0) {
                iris_media_reconciler_destroy(
                    server->iris_media_reconciler);
                ivr_control_adapter_destroy(server->ivr_control);
                server->ivr_control = NULL;
                ivr_certificate_identity_destroy(server->ivr_control_identity);
                server->ivr_control_identity = NULL;
                room_service_http_api_destroy(server->http_api);
                server->http_api = NULL;
                iris_event_outbox_destroy(server->iris_event_outbox);
                iris_completion_dispatcher_destroy(
                    server->iris_completion_dispatcher);
                iris_control_provider_destroy(
                    server->iris_control_provider);
                iris_room_bridge_destroy(server->iris_room_bridge);
                iris_media_bridge_destroy(server->iris_media_bridge);
                iris_command_ledger_destroy(server->iris_command_ledger);
                turbo_room_service_destroy(server->service);
                server->service = NULL;
                free(server->sfu_nodes);
                salts_mutex_destroy(&server->mutex);
                free(server);
                return NULL;
            }
        }
    }
#endif

    return server;
}

int room_service_app_server_start(room_service_app_server_t *server) {
    if (!server || !server->service) {
        return -1;
    }

#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    if (server->iris_command_ledger &&
        iris_command_ledger_start(server->iris_command_ledger) != SALTS_OK) {
        fprintf(stderr, "RoomService startup failed: Iris command ledger\n");
        return -1;
    }
    if (server->iris_completion_dispatcher &&
        iris_completion_dispatcher_start(
            server->iris_completion_dispatcher) != 0) {
        fprintf(stderr, "RoomService startup failed: Iris completion dispatcher\n");
        iris_command_ledger_stop(server->iris_command_ledger);
        return -1;
    }
    if (server->iris_event_outbox &&
        iris_event_outbox_start(server->iris_event_outbox) != SALTS_OK) {
        fprintf(stderr, "RoomService startup failed: Iris event outbox\n");
        iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
        iris_command_ledger_stop(server->iris_command_ledger);
        return -1;
    }
    if (server->ivr_control &&
        ivr_control_adapter_start(server->ivr_control) != IVR_OK) {
        fprintf(stderr, "RoomService startup failed: IVR control WebSocket\n");
        iris_event_outbox_stop(server->iris_event_outbox);
        iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
        iris_command_ledger_stop(server->iris_command_ledger);
        return -1;
    }
    if (server->iris_control_provider &&
        iris_control_provider_start(server->iris_control_provider) != 0) {
        fprintf(stderr, "RoomService startup failed: Iris control WebSocket\n");
        ivr_control_adapter_stop(server->ivr_control);
        iris_event_outbox_stop(server->iris_event_outbox);
        iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
        iris_command_ledger_stop(server->iris_command_ledger);
        return -1;
    }
    if (server->iris_media_reconciler &&
        iris_media_reconciler_start(server->iris_media_reconciler) != 0) {
        fprintf(stderr, "RoomService startup failed: Iris media reconciler\n");
        iris_control_provider_stop(server->iris_control_provider);
        ivr_control_adapter_stop(server->ivr_control);
        iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
        iris_event_outbox_stop(server->iris_event_outbox);
        iris_command_ledger_stop(server->iris_command_ledger);
        return -1;
    }
#endif
    if (room_service_http_api_start(server->http_api, server->config.bind_host,
                                    server->config.bind_port) != 0) {
        fprintf(stderr, "RoomService startup failed: HTTP API\n");
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
        iris_control_provider_stop(server->iris_control_provider);
        iris_media_reconciler_stop(server->iris_media_reconciler);
        if (server->ivr_control) {
            ivr_control_adapter_stop(server->ivr_control);
        }
        iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
        iris_event_outbox_stop(server->iris_event_outbox);
        iris_command_ledger_stop(server->iris_command_ledger);
#endif
        return -1;
    }

    atomic_store_explicit(&server->running, 1, memory_order_release);
    return 0;
}

int room_service_app_server_run(room_service_app_server_t *server) {
    if (!server ||
        !atomic_load_explicit(&server->running, memory_order_acquire)) {
        return -1;
    }

    if (server->config.dry_run) {
        printf("room_service: dry-run complete\n");
        return 0;
    }

    printf("room_service: running on %s:%d with node_id=%s\n",
           server->config.bind_host,
           server->config.bind_port,
           server->config.node_id);

    while (atomic_load_explicit(&server->running, memory_order_acquire)) {
        room_service_sleep_ms(100);
    }

    return 0;
}

void room_service_app_server_stop(room_service_app_server_t *server) {
    if (!server) {
        return;
    }

    atomic_store_explicit(&server->running, 0, memory_order_release);
    if (server->http_api) {
        room_service_http_api_stop(server->http_api);
    }
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    /* Quiesce producers before their consumers. Provider stop closes command
       ingress and waits for its dispatch worker; reconciler then stops
       producing adapter commands. Adapter stop establishes worker callback
       quiescence before completion/outbox consumers are drained. */
    iris_control_provider_stop(server->iris_control_provider);
    iris_media_reconciler_stop(server->iris_media_reconciler);
    if (server->ivr_control) {
        ivr_control_adapter_stop(server->ivr_control);
    }
    iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
    iris_event_outbox_stop(server->iris_event_outbox);
    iris_command_ledger_stop(server->iris_command_ledger);
#endif
}

void room_service_app_server_destroy(room_service_app_server_t *server) {
    if (!server) {
        return;
    }

    room_service_app_server_stop(server);

    if (server->http_api) {
        room_service_http_api_destroy(server->http_api);
        server->http_api = NULL;
    }
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    iris_media_reconciler_destroy(server->iris_media_reconciler);
    server->iris_media_reconciler = NULL;
    if (server->ivr_control) {
        ivr_control_adapter_destroy(server->ivr_control);
        server->ivr_control = NULL;
    }
    ivr_certificate_identity_destroy(server->ivr_control_identity);
    server->ivr_control_identity = NULL;
    iris_event_outbox_destroy(server->iris_event_outbox);
    server->iris_event_outbox = NULL;
    iris_completion_dispatcher_destroy(server->iris_completion_dispatcher);
    server->iris_completion_dispatcher = NULL;
    iris_control_provider_destroy(server->iris_control_provider);
    server->iris_control_provider = NULL;
    iris_room_bridge_destroy(server->iris_room_bridge);
    server->iris_room_bridge = NULL;
    iris_media_bridge_destroy(server->iris_media_bridge);
    server->iris_media_bridge = NULL;
    iris_command_ledger_destroy(server->iris_command_ledger);
    server->iris_command_ledger = NULL;
#endif
    if (server->service) {
        turbo_room_service_destroy(server->service);
    }
    free(server->sfu_nodes);
    free(server->call_center_events);
    free(server->conference_policies);
    free(server->room_sync_diagnostics);
    salts_mutex_destroy(&server->mutex);
    free(server);
}

turbo_room_service_t *room_service_app_server_get_service(
    room_service_app_server_t *server) {
    if (!server) {
        return NULL;
    }

    return server->service;
}

#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
ivr_status_t room_service_app_server_send_ivr_media_command(
    room_service_app_server_t *server, const ivr_media_command_t *command,
    char *out_worker_id, size_t out_worker_id_capacity) {
    if (!server || !server->ivr_control) {
        return IVR_ESTATE;
    }
    return ivr_control_adapter_send_media_command(
        server->ivr_control, command, out_worker_id, out_worker_id_capacity);
}

iris_media_bridge_result_t room_service_app_server_dispatch_iris_media_command(
    room_service_app_server_t *server, const char *idempotency_key,
    const char *body, size_t body_size) {
    if (!server || !server->iris_media_bridge) {
        iris_media_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = IRIS_MEDIA_BRIDGE_UNAVAILABLE;
        result.error_code = "MEDIA_PROVIDER_UNAVAILABLE";
        result.error_message = "Iris media provider is not configured";
        return result;
    }
    if (server->iris_media_reconciler &&
        !iris_media_reconciler_accepting_commands(
            server->iris_media_reconciler)) {
        iris_media_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = IRIS_MEDIA_BRIDGE_UNAVAILABLE;
        result.error_code = "MEDIA_PROVIDER_RECONCILING";
        result.error_message =
            "media provider is reconciling durable and worker state";
        return result;
    }
    return iris_media_bridge_dispatch_json(server->iris_media_bridge,
                                           idempotency_key, body, body_size);
}

iris_room_bridge_result_t room_service_app_server_dispatch_iris_room_command(
    room_service_app_server_t *server, const char *idempotency_key,
    const char *body, size_t body_size) {
    if (!server || !server->iris_room_bridge) {
        iris_room_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = IRIS_ROOM_BRIDGE_UNAVAILABLE;
        result.error_code = "ROOM_PROVIDER_UNAVAILABLE";
        result.error_message = "Iris room provider is not configured";
        return result;
    }
    if (server->iris_media_reconciler &&
        !iris_media_reconciler_accepting_commands(
            server->iris_media_reconciler)) {
        iris_room_bridge_result_t result;
        memset(&result, 0, sizeof(result));
        result.status = IRIS_ROOM_BRIDGE_UNAVAILABLE;
        result.error_code = "ROOM_PROVIDER_RECONCILING";
        result.error_message =
            "room provider is reconciling durable and worker state";
        return result;
    }
    return iris_room_bridge_dispatch_json(server->iris_room_bridge,
                                          idempotency_key, body, body_size);
}

ivr_status_t room_service_app_server_replay_iris_event(
    room_service_app_server_t *server, const char *event_id) {
    if (!server || !server->iris_event_outbox) return IVR_ESTATE;
    return iris_event_outbox_replay(server->iris_event_outbox, event_id);
}

ivr_status_t room_service_app_server_replay_iris_dead_letters(
    room_service_app_server_t *server, size_t limit,
    iris_event_replay_batch_result_t *result) {
    if (!server || !server->iris_event_outbox) return IVR_ECLOSED;
    return iris_event_outbox_replay_dead_letters(
        server->iris_event_outbox, limit, result);
}

ivr_status_t room_service_app_server_list_iris_dead_letters(
    room_service_app_server_t *server, iris_event_dead_letter_t *items,
    size_t capacity, size_t *count, size_t *total) {
    if (!server || !server->iris_event_outbox) return IVR_ESTATE;
    return iris_event_outbox_list_dead_letters(
        server->iris_event_outbox, items, capacity, count, total);
}

ivr_status_t room_service_app_server_list_iris_archived_events(
    room_service_app_server_t *server, iris_event_archive_t *items,
    size_t capacity, size_t *count, size_t *total) {
    if (!server || !server->iris_event_outbox) return IVR_ESTATE;
    return iris_event_outbox_list_archived(
        server->iris_event_outbox, items, capacity, count, total);
}

ivr_status_t room_service_app_server_run_iris_event_retention(
    room_service_app_server_t *server,
    iris_event_retention_result_t *result) {
    if (!server || !server->iris_event_outbox) return IVR_ECLOSED;
    return iris_event_outbox_run_retention(server->iris_event_outbox, result);
}
#endif

const room_service_app_config_t *room_service_app_server_get_config(
    room_service_app_server_t *server) {
    return server ? &server->config : NULL;
}

int room_service_app_server_get_stats(room_service_app_server_t *server,
                                      room_service_app_stats_t *stats) {
    if (!server || !stats) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    salts_mutex_lock(&server->mutex);
    stats->running =
        atomic_load_explicit(&server->running, memory_order_acquire);
    stats->room_sync_diagnostic_count = server->room_sync_diagnostic_count;
    stats->call_center_event_count = server->call_center_event_count;
    stats->conference_policy_count = server->conference_policy_count;
    stats->sfu_node_count = server->sfu_node_count;
    salts_mutex_unlock(&server->mutex);
    return 0;
}

int room_service_app_server_get_ivr_metrics(
    room_service_app_server_t *server, room_service_ivr_metrics_t *metrics) {
    if (!server || !metrics) {
        return -1;
    }

    memset(metrics, 0, sizeof(*metrics));
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
    if (server->ivr_control) {
        ivr_control_adapter_stats_t stats;
        ivr_control_adapter_get_stats(server->ivr_control, &stats);
        metrics->enabled = 1;
        metrics->workers = stats.workers;
        metrics->worker_capacity = stats.worker_capacity;
        metrics->worker_high_water = stats.worker_high_water;
        metrics->assignments = stats.assignments;
        metrics->assignment_capacity = stats.assignment_capacity;
        metrics->assignment_high_water = stats.assignment_high_water;
        metrics->dialogs = stats.dialogs;
        metrics->dialog_capacity = stats.dialog_capacity;
        metrics->dialog_high_water = stats.dialog_high_water;
        metrics->lease_expired_total = stats.lease_expired_total;
        metrics->dispatch_timeout_total = stats.dispatch_timeout_total;
        metrics->release_timeout_total = stats.release_timeout_total;
        metrics->request_queue_items = stats.bridge.request_queue_items;
        metrics->request_queue_capacity = stats.bridge.request_queue_capacity;
        metrics->request_queue_high_water =
            stats.bridge.request_queue_high_water;
        metrics->request_queue_drops_total = stats.bridge.request_queue_drops;
        metrics->peer_event_queue_items =
            stats.bridge.peer_event_queue_items;
        metrics->peer_event_queue_capacity =
            stats.bridge.peer_event_queue_capacity;
        metrics->peer_event_queue_high_water =
            stats.bridge.peer_event_queue_high_water;
        metrics->peer_event_queue_drops_total =
            stats.bridge.peer_event_queue_drops;
        metrics->peer_event_queue_overflowed =
            stats.bridge.peer_event_queue_overflowed;
    }
    if (server->iris_completion_dispatcher) {
        iris_completion_dispatcher_stats_t stats;
        iris_completion_dispatcher_get_stats(
            server->iris_completion_dispatcher, &stats);
        metrics->iris_provider_enabled = 1;
        metrics->iris_queue_items = (uint32_t)stats.queue_items;
        metrics->iris_queue_capacity = (uint32_t)stats.queue_capacity;
        metrics->iris_queue_high_water = (uint32_t)stats.queue_high_water;
        metrics->iris_in_flight = (uint32_t)stats.in_flight;
        metrics->iris_enqueued_total = stats.enqueued_total;
        metrics->iris_queue_full_total = stats.queue_full_total;
        metrics->iris_closed_rejections_total = stats.closed_rejections_total;
        metrics->iris_delivery_attempts_total =
            stats.delivery_attempts_total;
        metrics->iris_retries_total = stats.retries_total;
        metrics->iris_fence_conflicts_total = stats.fence_conflicts_total;
        metrics->iris_fence_refresh_failures_total =
            stats.fence_refresh_failures_total;
        metrics->iris_completion_success_total =
            stats.completion_success_total;
        metrics->iris_completion_failure_total =
            stats.completion_failure_total;
        metrics->iris_event_success_total = stats.event_success_total;
        metrics->iris_event_failure_total = stats.event_failure_total;
        metrics->iris_shutdown_restored_completions_total =
            stats.shutdown_restored_completions_total;
        metrics->iris_shutdown_dropped_events_total =
            stats.shutdown_dropped_events_total;
        metrics->iris_last_drain_duration_ms = stats.last_drain_duration_ms;
        metrics->iris_max_drain_duration_ms = stats.max_drain_duration_ms;
    }
    if (server->iris_command_ledger) {
        iris_command_ledger_stats_t stats;
        iris_command_ledger_get_stats(server->iris_command_ledger, &stats);
        metrics->iris_ledger_request_queue_items =
            (uint32_t)stats.request_queue_items;
        metrics->iris_ledger_request_queue_capacity =
            (uint32_t)stats.request_queue_capacity;
        metrics->iris_ledger_request_queue_high_water =
            (uint32_t)stats.request_queue_high_water;
        metrics->iris_ledger_record_capacity =
            (uint64_t)stats.record_capacity;
        metrics->iris_ledger_claims_total = stats.claims_total;
        metrics->iris_ledger_replays_total = stats.duplicates_total;
        metrics->iris_ledger_conflicts_total = stats.conflicts_total;
        metrics->iris_ledger_unknown_total = stats.unknown_total;
        metrics->iris_ledger_storage_failures_total =
            stats.storage_failures_total;
        metrics->iris_ledger_queue_rejections_total =
            stats.queue_rejections_total;
        metrics->iris_ledger_recovered_unknown_total =
            stats.recovered_unknown_total;
        metrics->iris_ledger_resource_queries_total =
            stats.resource_queries_total;
        metrics->iris_ledger_resource_seen_total = stats.resource_seen_total;
        metrics->iris_ledger_retained_deleted_total =
            stats.retained_deleted_total;
        metrics->iris_ledger_retention_sweeps_total =
            stats.retention_sweeps_total;
        metrics->iris_ledger_retention_failures_total =
            stats.retention_failures_total;
    }
    if (server->iris_event_outbox) {
        iris_event_outbox_stats_t stats;
        iris_event_outbox_get_stats(server->iris_event_outbox, &stats);
        metrics->iris_outbox_request_queue_items =
            (uint32_t)stats.request_queue_items;
        metrics->iris_outbox_request_queue_capacity =
            (uint32_t)stats.request_queue_capacity;
        metrics->iris_outbox_request_queue_high_water =
            (uint32_t)stats.request_queue_high_water;
        metrics->iris_outbox_pending_records =
            (uint32_t)stats.pending_records;
        metrics->iris_outbox_in_flight_records =
            (uint32_t)stats.in_flight_records;
        metrics->iris_outbox_dead_records = (uint32_t)stats.dead_records;
        metrics->iris_outbox_archived_records =
            (uint32_t)stats.archived_records;
        metrics->iris_outbox_record_capacity =
            (uint64_t)stats.record_capacity;
        metrics->iris_outbox_retained_payload_bytes =
            (uint64_t)stats.retained_payload_bytes;
        metrics->iris_outbox_peak_retained_payload_bytes =
            (uint64_t)stats.peak_retained_payload_bytes;
        metrics->iris_outbox_persisted_total = stats.persisted_total;
        metrics->iris_outbox_duplicate_total = stats.duplicate_total;
        metrics->iris_outbox_conflict_total = stats.conflict_total;
        metrics->iris_outbox_persist_failure_total =
            stats.persist_failure_total;
        metrics->iris_outbox_capacity_rejection_total =
            stats.capacity_rejection_total;
        metrics->iris_outbox_schedule_rejection_total =
            stats.schedule_rejection_total;
        metrics->iris_outbox_delivered_total = stats.delivered_total;
        metrics->iris_outbox_dead_lettered_total =
            stats.dead_lettered_total;
        metrics->iris_outbox_settlement_failure_total =
            stats.settlement_failure_total;
        metrics->iris_outbox_stale_settlement_total =
            stats.stale_settlement_total;
        metrics->iris_outbox_decode_failure_total =
            stats.decode_failure_total;
        metrics->iris_outbox_recovered_total = stats.recovered_total;
        metrics->iris_outbox_replayed_total = stats.replayed_total;
        metrics->iris_outbox_archived_total = stats.archived_total;
        metrics->iris_outbox_archive_deleted_total =
            stats.archive_deleted_total;
        metrics->iris_outbox_retention_failure_total =
            stats.retention_failure_total;
    }
    if (server->iris_media_reconciler) {
        iris_media_reconciler_stats_t stats;
        iris_media_reconciler_get_stats(server->iris_media_reconciler, &stats);
        metrics->iris_reconcile_state = (int)stats.state;
        metrics->iris_reconcile_accepting_commands =
            iris_media_reconciler_accepting_commands(
                server->iris_media_reconciler);
        metrics->iris_reconcile_inventory_queue_items =
            (uint32_t)stats.inventory_queue_items;
        metrics->iris_reconcile_inventory_queue_capacity =
            (uint32_t)stats.inventory_queue_capacity;
        metrics->iris_reconcile_cycles_total = stats.reconcile_cycles_total;
        metrics->iris_reconcile_failures_total =
            stats.reconcile_failures_total;
        metrics->iris_reconcile_expected_fetches_total =
            stats.expected_fetches_total;
        metrics->iris_reconcile_inventory_pages_total =
            stats.inventory_pages_total;
        metrics->iris_reconcile_rebound_total = stats.rebound_total;
        metrics->iris_reconcile_orphan_close_total =
            stats.orphan_close_total;
        metrics->iris_reconcile_resource_lost_total =
            stats.resource_lost_total;
        metrics->iris_reconcile_inventory_queue_full_total =
            stats.inventory_queue_full_total;
    }
#endif
    return 0;
}

int room_service_app_server_sync_attach_room(room_service_app_server_t *server,
                                             const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"attach_room\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_detach_room(room_service_app_server_t *server,
                                             const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"detach_room\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_force_close_room(room_service_app_server_t *server,
                                                  const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"force_close_room\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_add_session(room_service_app_server_t *server,
                                             const char *room_id,
                                             const char *participant_id) {
    char json[384];

    if (!server || !room_id || !participant_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"add_session\","
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\","
             "\"session_id\":\"%s\""
             "}",
             room_id, participant_id, participant_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_remove_session(room_service_app_server_t *server,
                                                const char *room_id,
                                                const char *participant_id) {
    char json[384];

    if (!server || !room_id || !participant_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"remove_session\","
             "\"room_id\":\"%s\","
             "\"session_id\":\"%s\""
             "}",
             room_id, participant_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_set_receiver_bandwidth(
    room_service_app_server_t *server, const char *room_id, const char *participant_id,
    int bandwidth_bps) {
    char json[384];

    if (!server || !room_id || !participant_id || bandwidth_bps < 0) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"set_receiver_bandwidth\","
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\","
             "\"bandwidth_bps\":%d"
             "}",
             room_id, participant_id, bandwidth_bps);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_set_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id, int enabled,
    turbo_room_video_layer_t max_layer) {
    turbo_room_subscription_summary_t subscription;

    memset(&subscription, 0, sizeof(subscription));
    room_service_copy_string(subscription.subscriber_participant_id,
                             sizeof(subscription.subscriber_participant_id),
                             receiver_participant_id);
    room_service_copy_string(subscription.track_id, sizeof(subscription.track_id),
                             track_id);
    subscription.enabled = enabled ? 1 : 0;
    subscription.preferred_layer = max_layer;
    subscription.target_layer = max_layer;
    subscription.muted = enabled ? 0 : 1;
    return room_service_app_server_sync_apply_track_subscription(server, room_id,
                                                                 &subscription);
}

int room_service_app_server_sync_apply_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const turbo_room_subscription_summary_t *subscription) {
    char json[768];
    const char *preferred_layer_name;
    const char *target_layer_name;
    const char *max_layer_name;
    turbo_room_video_layer_t max_layer;

    if (!server || !room_id || !subscription ||
        !subscription->subscriber_participant_id[0] || !subscription->track_id[0]) {
        return -1;
    }

    max_layer = room_service_resolve_subscription_max_layer(subscription);
    if (max_layer < TURBO_ROOM_VIDEO_LAYER_NONE ||
        max_layer > TURBO_ROOM_VIDEO_LAYER_HIGH) {
        return -1;
    }
    preferred_layer_name = room_service_video_layer_name(subscription->preferred_layer);
    target_layer_name = room_service_video_layer_name(subscription->target_layer);
    max_layer_name = room_service_video_layer_name(max_layer);

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"set_track_subscription\","
             "\"room_id\":\"%s\","
             "\"receiver_participant_id\":\"%s\","
             "\"track_id\":\"%s\","
             "\"enabled\":%s,"
             "\"priority\":%d,"
             "\"preferred_layer\":\"%s\","
             "\"target_layer\":\"%s\","
             "\"muted\":%s,"
             "\"policy_source\":\"%s\","
             "\"max_layer\":\"%s\""
             "}",
             room_id, subscription->subscriber_participant_id, subscription->track_id,
             subscription->enabled ? "true" : "false",
             subscription->priority,
             preferred_layer_name,
             target_layer_name,
             subscription->muted ? "true" : "false",
             subscription->policy_source,
             max_layer_name);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_start_recording(room_service_app_server_t *server,
                                                 const char *room_id,
                                                 const char *recording_id,
                                                 const char *mode) {
    char json[512];

    if (!server || !room_id || !recording_id || !mode) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"start_recording\","
             "\"room_id\":\"%s\","
             "\"recording_id\":\"%s\","
             "\"mode\":\"%s\""
             "}",
             room_id, recording_id, mode);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_stop_recording(room_service_app_server_t *server,
                                                const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"stop_recording\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_ensure_recording(
    room_service_app_server_t *server, const char *room_id,
    const char *recording_id, const char *mode, int *started) {
    room_service_sfu_recording_status_t status;

    if (started) {
        *started = 0;
    }
    if (!server || !room_id || !recording_id || !recording_id[0] ||
        !mode || !mode[0]) {
        return -1;
    }

    memset(&status, 0, sizeof(status));
    if (room_service_app_server_fetch_recording_status(server, room_id,
                                                       &status) != 0) {
        return -1;
    }

    if (status.found && status.active) {
        if (strcmp(status.recording_id, recording_id) == 0 &&
            strcmp(status.mode, mode) == 0) {
            return 0;
        }
        if (room_service_app_server_sync_stop_recording(server, room_id) != 0) {
            return -1;
        }
    }

    if (room_service_app_server_sync_start_recording(server, room_id,
                                                     recording_id, mode) != 0) {
        return -1;
    }
    if (started) {
        *started = 1;
    }
    return 0;
}

int room_service_app_server_sync_register_track(room_service_app_server_t *server,
                                                const char *room_id,
                                                const turbo_room_track_summary_t *track) {
    char json[640];
    int written;
    int i;

    if (!server || !room_id || !track || track->main_ssrc == 0) {
        return -1;
    }

    written = snprintf(json, sizeof(json),
                       "{"
                       "\"type\":\"register_published_track\","
                       "\"room_id\":\"%s\","
                       "\"participant_id\":\"%s\","
                       "\"track_id\":\"%s\","
                       "\"kind\":\"%s\","
                       "\"codec_name\":\"%s\","
                       "\"main_ssrc\":%u,"
                       "\"layer_ssrcs\":[",
                       room_id, track->owner_participant_id, track->track_id,
                       track->kind == TURBO_ROOM_TRACK_AUDIO ? "audio" : "video",
                       track->codec_name,
                       (unsigned)track->main_ssrc);
    if (written < 0 || written >= (int)sizeof(json)) {
        return -1;
    }

    for (i = 0; i < track->layer_count && i < 3; ++i) {
        int rc = snprintf(json + written, sizeof(json) - (size_t)written,
                          "%s%u", i == 0 ? "" : ",",
                          (unsigned)track->layer_ssrcs[i]);
        if (rc < 0 || rc >= (int)(sizeof(json) - (size_t)written)) {
            return -1;
        }
        written += rc;
    }

    if (track->layer_count <= 0) {
        int rc = snprintf(json + written, sizeof(json) - (size_t)written, "%u",
                          (unsigned)track->main_ssrc);
        if (rc < 0 || rc >= (int)(sizeof(json) - (size_t)written)) {
            return -1;
        }
        written += rc;
    }

    if (snprintf(json + written, sizeof(json) - (size_t)written, "]}")
        >= (int)(sizeof(json) - (size_t)written)) {
        return -1;
    }

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_unregister_track(room_service_app_server_t *server,
                                                  const char *room_id,
                                                  const char *track_id) {
    char json[384];

    if (!server || !room_id || !track_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"unregister_published_track\","
             "\"room_id\":\"%s\","
             "\"track_id\":\"%s\""
             "}",
             room_id, track_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_replay_room_state(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_replay_stats_t *out_stats) {
    turbo_room_service_t *service;
    turbo_room_summary_t room_summary;
    room_service_sfu_replay_stats_t stats;
    int i;

    memset(&stats, 0, sizeof(stats));

    if (!server || !room_id) {
        return -1;
    }

    service = room_service_app_server_get_service(server);
    if (!service ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
        return -1;
    }

    if (room_service_app_server_sync_attach_room(server, room_id) != 0) {
        return -1;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int effective_bandwidth_bps = 0;

        if (turbo_room_service_get_participant_summary_at(service, room_id, i,
                                                          &participant_summary) != 0) {
            return -1;
        }

        if (room_service_app_server_sync_add_session(server, room_id,
                                                     participant_summary.participant_id) != 0) {
            return -1;
        }
        stats.participants_replayed++;

        if (turbo_room_service_get_effective_receiver_bandwidth(
                service, room_id, participant_summary.participant_id,
                &effective_bandwidth_bps, NULL) != 0) {
            return -1;
        }

        if (room_service_app_server_sync_set_receiver_bandwidth(
                server, room_id, participant_summary.participant_id,
                effective_bandwidth_bps) != 0) {
            return -1;
        }
        stats.receiver_bandwidths_replayed++;
    }

    for (i = 0; i < room_summary.published_track_count; ++i) {
        turbo_room_track_summary_t track_summary;

        if (turbo_room_service_get_track_summary_at(service, room_id, i, &track_summary) != 0) {
            return -1;
        }

        if (track_summary.main_ssrc == 0) {
            stats.skipped_items++;
            continue;
        }

        if (room_service_app_server_sync_register_track(server, room_id, &track_summary) != 0) {
            return -1;
        }
        stats.tracks_replayed++;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;
        turbo_room_track_summary_t track_summary;

        if (turbo_room_service_get_subscription_summary_at(service, room_id, i,
                                                           &subscription_summary) != 0) {
            return -1;
        }

        if (turbo_room_service_get_track_summary(service, room_id, subscription_summary.track_id,
                                                 &track_summary) != 0 ||
            track_summary.main_ssrc == 0) {
            stats.skipped_items++;
            continue;
        }

        if (room_service_app_server_sync_apply_track_subscription(
                server, room_id, &subscription_summary) != 0) {
            return -1;
        }
        stats.subscriptions_replayed++;
    }

    if (room_summary.recording_state == TURBO_ROOM_RECORDING_ACTIVE) {
        int recording_started = 0;

        if (room_summary.recording_id[0] == '\0' ||
            room_summary.recording_mode[0] == '\0' ||
            stats.tracks_replayed == 0) {
            stats.skipped_items++;
        } else if (room_service_app_server_sync_ensure_recording(
                       server, room_id, room_summary.recording_id,
                       room_summary.recording_mode, &recording_started) != 0) {
            return -1;
        } else if (recording_started) {
            stats.recordings_replayed++;
        }
    }

    if (out_stats) {
        *out_stats = stats;
    }
    return 0;
}

int room_service_app_server_sync_resync_room(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_replay_stats_t *out_stats) {
    turbo_room_service_t *service;
    turbo_room_summary_t room_summary;

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }

    if (!server || !room_id) {
        return -1;
    }

    service = room_service_app_server_get_service(server);
    if (!service ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        room_summary.assigned_sfu_node[0] == '\0') {
        return -1;
    }

    if (room_service_app_server_sync_force_close_room(server, room_id) != 0) {
        return -1;
    }

    return room_service_app_server_sync_replay_room_state(server, room_id,
                                                          out_stats);
}

int room_service_app_server_fetch_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id,
    room_service_sfu_track_subscription_t *subscription) {
    turbo_transport_t *client;
    chttp_response *response;
    json_value_t *root = NULL;
    json_value_t *track_subscription;
    char command_json[512];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];

    if (!subscription) {
        return -1;
    }

    memset(subscription, 0, sizeof(*subscription));
    if (!server || !room_id || !receiver_participant_id || !track_id) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }

    client = room_service_create_sfu_client_for_route(
        server, control_url, control_token);
    if (!client) {
        return -1;
    }

    snprintf(command_json, sizeof(command_json),
             "{"
             "\"type\":\"get_track_subscription\","
             "\"room_id\":\"%s\","
             "\"receiver_participant_id\":\"%s\","
             "\"track_id\":\"%s\""
             "}",
             room_id, receiver_participant_id, track_id);

    response = room_service_http_post_json(client, "/api/v1/commands",
                                           command_json);
    if (!response) {
        turbo_transport_destroy(client);
        return -1;
    }

    if (response->status_code == 404) {
        subscription->available = 1;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return 0;
    }

    if (response->status_code < 200u || response->status_code >= 300u ||
        !room_service_http_response_is_json(response)) {
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    root = room_service_http_response_parse_json(response);
    if (!root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    track_subscription = json_object_get(root, "track_subscription");
    if (!track_subscription || json_type(track_subscription) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    subscription->available = 1;
    subscription->found = 1;
    subscription->enabled =
        room_service_json_bool_field(track_subscription, "enabled", 0);
    subscription->priority = json_get_int(track_subscription, "priority", 0);
    subscription->preferred_layer = room_service_parse_video_layer(
        room_service_json_string_field(track_subscription, "preferred_layer"));
    subscription->target_layer = room_service_parse_video_layer(
        room_service_json_string_field(track_subscription, "target_layer"));
    subscription->muted =
        room_service_json_bool_field(track_subscription, "muted", 0);
    subscription->max_layer = room_service_parse_video_layer(
        room_service_json_string_field(track_subscription, "max_layer"));

    if (room_service_json_string_field(track_subscription, "receiver_participant_id")) {
        strncpy(subscription->receiver_participant_id,
                room_service_json_string_field(track_subscription,
                                               "receiver_participant_id"),
                sizeof(subscription->receiver_participant_id) - 1);
    }
    if (room_service_json_string_field(track_subscription, "sender_participant_id")) {
        strncpy(subscription->sender_participant_id,
                room_service_json_string_field(track_subscription,
                                               "sender_participant_id"),
                sizeof(subscription->sender_participant_id) - 1);
    }
    if (room_service_json_string_field(track_subscription, "track_id")) {
        strncpy(subscription->track_id,
                room_service_json_string_field(track_subscription, "track_id"),
                sizeof(subscription->track_id) - 1);
    }
    if (room_service_json_string_field(track_subscription, "policy_source")) {
        strncpy(subscription->policy_source,
                room_service_json_string_field(track_subscription, "policy_source"),
                sizeof(subscription->policy_source) - 1);
    }

    json_free(root);

    root = NULL;
    room_service_http_response_free(response);
    turbo_transport_destroy(client);
    return 0;
}

int room_service_app_server_fetch_participant_stats(
    room_service_app_server_t *server, const char *room_id, const char *participant_id,
    room_service_sfu_participant_stats_t *stats) {
    turbo_transport_t *client;
    chttp_response *response;
    json_value_t *root = NULL;
    json_value_t *participant_stats;
    char command_json[384];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];

    if (!stats) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    if (!server || !room_id || !participant_id) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }

    client = room_service_create_sfu_client_for_route(
        server, control_url, control_token);
    if (!client) {
        return -1;
    }

    snprintf(command_json, sizeof(command_json),
             "{"
             "\"type\":\"get_participant_stats\","
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\""
             "}",
             room_id, participant_id);

    response = room_service_http_post_json(client, "/api/v1/commands",
                                           command_json);
    if (!response) {
        turbo_transport_destroy(client);
        return -1;
    }

    if (response->status_code == 404) {
        stats->available = 1;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return 0;
    }

    if (response->status_code < 200u || response->status_code >= 300u ||
        !room_service_http_response_is_json(response)) {
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    root = room_service_http_response_parse_json(response);
    if (!root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    participant_stats = json_object_get(root, "participant_stats");
    if (!participant_stats || json_type(participant_stats) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    stats->available = 1;
    stats->found = 1;
    if (room_service_json_string_field(participant_stats, "participant_id")) {
        strncpy(stats->participant_id,
                room_service_json_string_field(participant_stats, "participant_id"),
                sizeof(stats->participant_id) - 1);
    }
    stats->stream_count = json_get_int(participant_stats, "stream_count", 0);
    stats->available_bandwidth =
        json_get_int(participant_stats, "available_bandwidth", 0);
    stats->packets_sent =
        room_service_json_int64_field(participant_stats, "packets_sent", 0);
    stats->bytes_sent =
        room_service_json_int64_field(participant_stats, "bytes_sent", 0);
    stats->packets_received =
        room_service_json_int64_field(participant_stats, "packets_received", 0);
    stats->bytes_received =
        room_service_json_int64_field(participant_stats, "bytes_received", 0);

    json_free(root);

    root = NULL;
    room_service_http_response_free(response);
    turbo_transport_destroy(client);
    return 0;
}

int room_service_app_server_fetch_recording_status(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_recording_status_t *status) {
    turbo_transport_t *client;
    chttp_response *response;
    json_value_t *root = NULL;
    json_value_t *recording_status;
    char command_json[256];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];

    if (!status) {
        return -1;
    }

    memset(status, 0, sizeof(*status));
    if (!server || !room_id) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }

    client = room_service_create_sfu_client_for_route(
        server, control_url, control_token);
    if (!client) {
        return -1;
    }

    snprintf(command_json, sizeof(command_json),
             "{"
             "\"type\":\"get_recording_status\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    response = room_service_http_post_json(client, "/api/v1/commands",
                                           command_json);
    if (!response) {
        turbo_transport_destroy(client);
        return -1;
    }

    if (response->status_code == 404) {
        status->available = 1;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return 0;
    }

    if (response->status_code < 200u || response->status_code >= 300u ||
        !room_service_http_response_is_json(response)) {
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    root = room_service_http_response_parse_json(response);
    if (!root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    recording_status = json_object_get(root, "recording_status");
    if (!recording_status || json_type(recording_status) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        room_service_http_response_free(response);
        turbo_transport_destroy(client);
        return -1;
    }

    status->available = 1;
    status->found = 1;
    status->active = room_service_json_bool_field(recording_status, "active", 0);
    status->track_count = json_get_int(recording_status, "track_count", 0);
    room_service_copy_string(status->room_id, sizeof(status->room_id),
                             room_service_json_string_field(recording_status, "room_id"));
    room_service_copy_string(
        status->recording_id, sizeof(status->recording_id),
        room_service_json_string_field(recording_status, "recording_id"));
    room_service_copy_string(status->mode, sizeof(status->mode),
                             room_service_json_string_field(recording_status, "mode"));

    json_free(root);

    root = NULL;
    room_service_http_response_free(response);
    turbo_transport_destroy(client);
    return 0;
}

int room_service_app_server_record_room_sync(
    room_service_app_server_t *server, const char *room_id, const char *operation,
    const room_service_sfu_replay_stats_t *stats, const char *warning_code,
    const char *warning_message) {
    room_service_room_sync_diagnostic_t *diagnostic;
    int rc = -1;

    if (!server || !room_id || !operation || !stats) {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    diagnostic = room_service_find_room_sync_diagnostic(server, room_id);
    if (!diagnostic) {
        if (room_service_ensure_capacity((void **)&server->room_sync_diagnostics,
                                         &server->room_sync_diagnostic_capacity,
                                         sizeof(*server->room_sync_diagnostics),
                                         server->room_sync_diagnostic_count + 1) != 0) {
            goto out;
        }

        diagnostic = &server->room_sync_diagnostics[server->room_sync_diagnostic_count++];
        memset(diagnostic, 0, sizeof(*diagnostic));
    }

    diagnostic->available = 1;
    room_service_copy_string(diagnostic->room_id, sizeof(diagnostic->room_id), room_id);
    room_service_copy_string(diagnostic->operation, sizeof(diagnostic->operation), operation);
    diagnostic->replay_stats = *stats;
    diagnostic->had_warning = (warning_code && warning_message) ? 1 : 0;
    room_service_copy_string(diagnostic->warning_code, sizeof(diagnostic->warning_code),
                             warning_code);
    room_service_copy_string(diagnostic->warning_message,
                             sizeof(diagnostic->warning_message), warning_message);
    diagnostic->sequence = ++server->room_sync_sequence;
    rc = 0;

out:
    salts_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_get_room_sync_diagnostic(
    room_service_app_server_t *server, const char *room_id,
    room_service_room_sync_diagnostic_t *diagnostic) {
    room_service_room_sync_diagnostic_t *stored;

    if (!server || !room_id || !diagnostic) {
        return -1;
    }

    memset(diagnostic, 0, sizeof(*diagnostic));
    salts_mutex_lock(&server->mutex);
    stored = room_service_find_room_sync_diagnostic(server, room_id);
    if (!stored) {
        salts_mutex_unlock(&server->mutex);
        return -1;
    }

    *diagnostic = *stored;
    salts_mutex_unlock(&server->mutex);
    return 0;
}

int room_service_app_server_record_call_center_event(
    room_service_app_server_t *server, const char *room_id, const char *event_type,
    const char *participant_id, const char *peer_participant_id, const char *state,
    const char *detail) {
    room_service_call_center_event_t *event;
    int rc = -1;

    if (!server || !room_id || !room_id[0] || !event_type || !event_type[0]) {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    if (room_service_ensure_capacity((void **)&server->call_center_events,
                                     &server->call_center_event_capacity,
                                     sizeof(*server->call_center_events),
                                     server->call_center_event_count + 1) != 0) {
        goto out;
    }

    event = &server->call_center_events[server->call_center_event_count++];
    memset(event, 0, sizeof(*event));
    event->available = 1;
    room_service_copy_string(event->room_id, sizeof(event->room_id), room_id);
    room_service_copy_string(event->event_type, sizeof(event->event_type), event_type);
    room_service_copy_string(event->participant_id, sizeof(event->participant_id),
                             participant_id);
    room_service_copy_string(event->peer_participant_id,
                             sizeof(event->peer_participant_id), peer_participant_id);
    room_service_copy_string(event->state, sizeof(event->state), state);
    room_service_copy_string(event->detail, sizeof(event->detail), detail);
    event->sequence = ++server->call_center_event_sequence;
    room_service_prune_call_center_events_locked(server);
    rc = 0;

out:
    salts_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_list_call_center_events(
    room_service_app_server_t *server, const char *room_id, int64_t after_sequence,
    int limit, room_service_call_center_event_t *events, int event_capacity,
    int *out_count, int64_t *out_latest_sequence) {
    int count = 0;
    int i;

    if (out_count) {
        *out_count = 0;
    }
    if (out_latest_sequence) {
        *out_latest_sequence = 0;
    }
    if (!server || !room_id || !events || event_capacity <= 0 || limit < 0) {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    if (limit == 0 || limit > event_capacity) {
        limit = event_capacity;
    }

    for (i = 0; i < server->call_center_event_count; ++i) {
        room_service_call_center_event_t *event = &server->call_center_events[i];

        if (!event->available || strcmp(event->room_id, room_id) != 0) {
            continue;
        }
        if (event->sequence > after_sequence) {
            if (count < limit) {
                events[count++] = *event;
            }
        }
        if (out_latest_sequence && event->sequence > *out_latest_sequence) {
            *out_latest_sequence = event->sequence;
        }
    }

    salts_mutex_unlock(&server->mutex);

    if (out_count) {
        *out_count = count;
    }
    return 0;
}

int room_service_app_server_set_conference_layout_mode(
    room_service_app_server_t *server, const char *room_id, const char *layout_mode) {
    room_service_conference_policy_t *policy;
    const char *normalized_layout;
    int rc = -1;

    if (!server || !room_id || !room_service_layout_mode_is_valid(layout_mode) ||
        !room_service_room_exists(server, room_id)) {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        goto out;
    }

    normalized_layout = room_service_normalize_layout_mode(layout_mode);
    if (strcmp(policy->layout_mode, normalized_layout) == 0) {
        rc = 0;
        goto out;
    }

    if (turbo_room_service_set_layout_mode(server->service, room_id,
                                           room_service_layout_mode_to_core(
                                               normalized_layout)) != 0) {
        goto out;
    }

    room_service_copy_string(policy->layout_mode, sizeof(policy->layout_mode),
                             normalized_layout);
    policy->version = ++server->conference_policy_sequence;
    rc = 0;

out:
    salts_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_set_conference_active_speaker(
    room_service_app_server_t *server, const char *room_id, const char *participant_id) {
    room_service_conference_policy_t *policy;
    const char *normalized_participant_id = participant_id ? participant_id : "";
    int rc = -1;

    if (!server || !room_id || !room_service_room_exists(server, room_id)) {
        return -1;
    }

    if (normalized_participant_id[0] != '\0' &&
        !room_service_participant_exists(server, room_id, normalized_participant_id)) {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        goto out;
    }

    if (strcmp(policy->active_speaker_participant_id, normalized_participant_id) == 0) {
        rc = 0;
        goto out;
    }

    if (turbo_room_service_set_active_speaker(
            server->service, room_id,
            normalized_participant_id[0] != '\0' ? normalized_participant_id : NULL) != 0) {
        goto out;
    }

    room_service_copy_string(policy->active_speaker_participant_id,
                             sizeof(policy->active_speaker_participant_id),
                             normalized_participant_id);
    policy->version = ++server->conference_policy_sequence;
    rc = 0;

out:
    salts_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_set_conference_pin(room_service_app_server_t *server,
                                               const char *room_id,
                                               const char *participant_id) {
    room_service_conference_policy_t *policy;
    const char *normalized_participant_id = participant_id ? participant_id : "";
    int rc = -1;

    if (!server || !room_id || !room_service_room_exists(server, room_id)) {
        return -1;
    }

    if (normalized_participant_id[0] != '\0' &&
        !room_service_participant_exists(server, room_id, normalized_participant_id)) {
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        goto out;
    }

    if (strcmp(policy->pinned_participant_id, normalized_participant_id) == 0) {
        rc = 0;
        goto out;
    }

    if (turbo_room_service_pin_participant(
            server->service, room_id,
            normalized_participant_id[0] != '\0' ? normalized_participant_id : NULL) != 0) {
        goto out;
    }

    room_service_copy_string(policy->pinned_participant_id,
                             sizeof(policy->pinned_participant_id),
                             normalized_participant_id);
    policy->version = ++server->conference_policy_sequence;
    rc = 0;

out:
    salts_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_get_conference_policy(
    room_service_app_server_t *server, const char *room_id,
    room_service_conference_policy_t *policy) {
    if (!policy) {
        return -1;
    }

    return room_service_fill_conference_policy(server, room_id, policy);
}

int room_service_app_server_apply_conference_policy(
    room_service_app_server_t *server, const char *room_id,
    room_service_conference_policy_apply_result_t *result) {
    turbo_room_summary_t room_summary;
    room_service_conference_policy_t policy;
    room_service_policy_subscription_t *desired_subscriptions = NULL;
    room_service_policy_subscription_t *stale_subscriptions = NULL;
    int desired_count = 0;
    int desired_capacity = 0;
    int stale_count = 0;
    int stale_capacity = 0;
    int i;

    if (result) {
        memset(result, 0, sizeof(*result));
    }

    if (!server || !server->service || !room_id ||
        turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0 ||
        room_service_fill_conference_policy(server, room_id, &policy) != 0) {
        return -1;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int j;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0) {
            free(desired_subscriptions);
            return -1;
        }

        for (j = 0; j < room_summary.published_track_count; ++j) {
            turbo_room_track_summary_t track_summary;
            room_service_policy_subscription_t desired;

            if (turbo_room_service_get_track_summary_at(server->service, room_id, j,
                                                        &track_summary) != 0) {
                free(desired_subscriptions);
                return -1;
            }

            if (strcmp(participant_summary.participant_id,
                       track_summary.owner_participant_id) == 0) {
                continue;
            }

            room_service_build_policy_subscription(&policy, &track_summary,
                                                   participant_summary.participant_id,
                                                   &desired);
            if (room_service_ensure_capacity((void **)&desired_subscriptions, &desired_capacity,
                                             sizeof(*desired_subscriptions),
                                             desired_count + 1) != 0) {
                free(desired_subscriptions);
                return -1;
            }
            desired_subscriptions[desired_count++] = desired;
        }
    }

    for (i = 0; i < desired_count; ++i) {
        turbo_room_subscription_summary_t existing_summary;
        turbo_room_subscription_config_t config;
        const char *warning_code = NULL;
        const char *warning_message = NULL;
        int already_matches = 0;

        memset(&config, 0, sizeof(config));
        if (turbo_room_service_get_subscription_summary(server->service, room_id,
                                                        desired_subscriptions[i]
                                                            .subscriber_participant_id,
                                                        desired_subscriptions[i].track_id,
                                                        &existing_summary) == 0) {
            already_matches = room_service_desired_subscription_matches(
                &existing_summary, &desired_subscriptions[i]);
        }

        if (already_matches) {
            continue;
        }

        config.subscriber_participant_id =
            desired_subscriptions[i].subscriber_participant_id;
        config.track_id = desired_subscriptions[i].track_id;
        config.enabled = desired_subscriptions[i].enabled;
        config.priority = desired_subscriptions[i].priority;
        config.preferred_layer = desired_subscriptions[i].preferred_layer;
        config.target_layer = desired_subscriptions[i].target_layer;
        config.muted = desired_subscriptions[i].muted;
        config.policy_source = desired_subscriptions[i].policy_source;

        if (turbo_room_service_set_subscription(server->service, room_id, &config) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (result) {
            result->subscriptions_applied++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0') {
            turbo_room_subscription_summary_t applied_summary;
            if (turbo_room_service_get_subscription_summary(
                    server->service, room_id, config.subscriber_participant_id,
                    config.track_id, &applied_summary) != 0 ||
                room_service_app_server_sync_apply_track_subscription(
                    server, room_id, &applied_summary) != 0) {
                warning_code = "SFU_SYNC_FAILED";
                warning_message =
                    "conference policy committed locally; set_track_subscription was not forwarded to sfu node";
            }
            room_service_capture_policy_warning(result, warning_code, warning_message);
        }
    }

    if (turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        free(stale_subscriptions);
        free(desired_subscriptions);
        return -1;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;

        if (turbo_room_service_get_subscription_summary_at(server->service, room_id, i,
                                                           &subscription_summary) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (!room_service_policy_has_managed_source(subscription_summary.policy_source) ||
            room_service_policy_has_desired_subscription(
                desired_subscriptions, desired_count,
                subscription_summary.subscriber_participant_id,
                subscription_summary.track_id)) {
            continue;
        }

        if (room_service_ensure_capacity((void **)&stale_subscriptions, &stale_capacity,
                                         sizeof(*stale_subscriptions),
                                         stale_count + 1) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        memset(&stale_subscriptions[stale_count], 0, sizeof(stale_subscriptions[stale_count]));
        room_service_copy_string(stale_subscriptions[stale_count].subscriber_participant_id,
                                 sizeof(stale_subscriptions[stale_count]
                                            .subscriber_participant_id),
                                 subscription_summary.subscriber_participant_id);
        room_service_copy_string(stale_subscriptions[stale_count].track_id,
                                 sizeof(stale_subscriptions[stale_count].track_id),
                                 subscription_summary.track_id);
        stale_count++;
    }

    for (i = 0; i < stale_count; ++i) {
        if (turbo_room_service_remove_subscription(
                server->service, room_id,
                stale_subscriptions[i].subscriber_participant_id,
                stale_subscriptions[i].track_id) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (result) {
            result->subscriptions_removed++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0') {
            turbo_room_subscription_summary_t disabled_summary;

            memset(&disabled_summary, 0, sizeof(disabled_summary));
            room_service_copy_string(disabled_summary.subscriber_participant_id,
                                     sizeof(disabled_summary.subscriber_participant_id),
                                     stale_subscriptions[i].subscriber_participant_id);
            room_service_copy_string(disabled_summary.track_id,
                                     sizeof(disabled_summary.track_id),
                                     stale_subscriptions[i].track_id);
            disabled_summary.enabled = 0;
            disabled_summary.muted = 1;
            disabled_summary.preferred_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
            disabled_summary.target_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
            if (room_service_app_server_sync_apply_track_subscription(
                    server, room_id, &disabled_summary) != 0) {
                room_service_capture_policy_warning(
                    result, "SFU_SYNC_FAILED",
                    "conference policy committed locally; track unsubscribe was not forwarded to sfu node");
            }
        }
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int effective_bandwidth_bps = 0;
        int is_explicit_bandwidth = 0;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (turbo_room_service_get_effective_receiver_bandwidth(
                server->service, room_id, participant_summary.participant_id,
                &effective_bandwidth_bps, &is_explicit_bandwidth) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (result) {
            result->receiver_bandwidths_reconciled++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0' && !is_explicit_bandwidth &&
            room_service_app_server_sync_set_receiver_bandwidth(
                server, room_id, participant_summary.participant_id,
                effective_bandwidth_bps) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "conference policy committed locally; derived receiver bandwidth was not forwarded to sfu node");
        }
    }

    free(stale_subscriptions);
    free(desired_subscriptions);
    return 0;
}

int room_service_app_server_apply_call_center_policy(
    room_service_app_server_t *server, const char *room_id,
    turbo_call_center_supervisor_mode_t supervisor_mode,
    room_service_conference_policy_apply_result_t *result) {
    turbo_room_summary_t room_summary;
    room_service_conference_policy_t *policy;
    room_service_policy_subscription_t *before_subscriptions = NULL;
    room_service_policy_subscription_t *after_subscriptions = NULL;
    int before_count = 0;
    int after_count = 0;
    int i;

    if (result) {
        memset(result, 0, sizeof(*result));
    }

    if (!server || !server->service || !room_id ||
        turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        return -1;
    }

    if (room_service_capture_policy_subscriptions(
            server->service, room_id, room_service_policy_has_call_center_source,
            &before_subscriptions, &before_count) != 0) {
        return -1;
    }

    if (turbo_room_service_reconcile_call_center_subscriptions(
            server->service, room_id, supervisor_mode) != 0) {
        free(before_subscriptions);
        return -1;
    }

    salts_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        salts_mutex_unlock(&server->mutex);
        free(before_subscriptions);
        return -1;
    }
    policy->supervisor_mode = supervisor_mode;
    policy->version = ++server->conference_policy_sequence;
    salts_mutex_unlock(&server->mutex);

    if (room_service_capture_policy_subscriptions(
            server->service, room_id, room_service_policy_has_call_center_source,
            &after_subscriptions, &after_count) != 0) {
        free(before_subscriptions);
        return -1;
    }

    for (i = 0; i < after_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;

        if (turbo_room_service_get_subscription_summary(
                server->service, room_id,
                after_subscriptions[i].subscriber_participant_id,
                after_subscriptions[i].track_id, &subscription_summary) != 0) {
            free(after_subscriptions);
            free(before_subscriptions);
            return -1;
        }

        if (result) {
            result->subscriptions_applied++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0' &&
            room_service_app_server_sync_apply_track_subscription(
                server, room_id, &subscription_summary) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "call-center policy committed locally; set_track_subscription was not forwarded to sfu node");
        }
    }

    for (i = 0; i < before_count; ++i) {
        turbo_room_subscription_summary_t disabled_summary;

        if (room_service_policy_has_desired_subscription(
                after_subscriptions, after_count,
                before_subscriptions[i].subscriber_participant_id,
                before_subscriptions[i].track_id)) {
            continue;
        }

        if (result) {
            result->subscriptions_removed++;
        }

        if (room_summary.assigned_sfu_node[0] == '\0') {
            continue;
        }

        memset(&disabled_summary, 0, sizeof(disabled_summary));
        room_service_copy_string(disabled_summary.subscriber_participant_id,
                                 sizeof(disabled_summary.subscriber_participant_id),
                                 before_subscriptions[i].subscriber_participant_id);
        room_service_copy_string(disabled_summary.track_id,
                                 sizeof(disabled_summary.track_id),
                                 before_subscriptions[i].track_id);
        disabled_summary.enabled = 0;
        disabled_summary.muted = 1;
        disabled_summary.preferred_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
        disabled_summary.target_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
        room_service_copy_string(disabled_summary.policy_source,
                                 sizeof(disabled_summary.policy_source),
                                 before_subscriptions[i].policy_source);
        if (room_service_app_server_sync_apply_track_subscription(
                server, room_id, &disabled_summary) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "call-center policy committed locally; track unsubscribe was not forwarded to sfu node");
        }
    }

    if (turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        free(after_subscriptions);
        free(before_subscriptions);
        return -1;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int effective_bandwidth_bps = 0;
        int is_explicit_bandwidth = 0;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0 ||
            turbo_room_service_get_effective_receiver_bandwidth(
                server->service, room_id, participant_summary.participant_id,
                &effective_bandwidth_bps, &is_explicit_bandwidth) != 0) {
            free(after_subscriptions);
            free(before_subscriptions);
            return -1;
        }

        if (result) {
            result->receiver_bandwidths_reconciled++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0' && !is_explicit_bandwidth &&
            room_service_app_server_sync_set_receiver_bandwidth(
                server, room_id, participant_summary.participant_id,
                effective_bandwidth_bps) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "call-center policy committed locally; derived receiver bandwidth was not forwarded to sfu node");
        }
    }

    free(after_subscriptions);
    free(before_subscriptions);
    return 0;
}
