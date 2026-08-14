/**
 * @file server.h
 * @brief Room service application wrapper
 */
#ifndef TURBO_ROOM_SERVICE_APP_SERVER_H
#define TURBO_ROOM_SERVICE_APP_SERVER_H

#include "room_service/config.h"
#include "turbo_room_service.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct room_service_app_server_s room_service_app_server_t;

#define ROOM_SERVICE_CONFERENCE_LAYOUT_MODE_MAX 16

typedef struct {
    int available;
    int found;
    char receiver_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char sender_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
    turbo_room_video_layer_t max_layer;
} room_service_sfu_track_subscription_t;

typedef struct {
    int available;
    int found;
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    int stream_count;
    int available_bandwidth;
    int64_t packets_sent;
    int64_t bytes_sent;
    int64_t packets_received;
    int64_t bytes_received;
} room_service_sfu_participant_stats_t;

typedef struct {
    int available;
    int found;
    int active;
    char room_id[TURBO_ROOM_ID_MAX];
    char recording_id[TURBO_RECORDING_ID_MAX];
    char mode[TURBO_RECORDING_MODE_MAX];
    int track_count;
} room_service_sfu_recording_status_t;

typedef struct {
    int participants_replayed;
    int receiver_bandwidths_replayed;
    int tracks_replayed;
    int subscriptions_replayed;
    int recordings_replayed;
    int skipped_items;
} room_service_sfu_replay_stats_t;

typedef struct {
    int available;
    char room_id[TURBO_ROOM_ID_MAX];
    char operation[16];
    room_service_sfu_replay_stats_t replay_stats;
    int had_warning;
    char warning_code[64];
    char warning_message[256];
    int64_t sequence;
} room_service_room_sync_diagnostic_t;

typedef struct {
    int available;
    char room_id[TURBO_ROOM_ID_MAX];
    char layout_mode[ROOM_SERVICE_CONFERENCE_LAYOUT_MODE_MAX];
    char active_speaker_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char pinned_participant_id[TURBO_PARTICIPANT_ID_MAX];
    turbo_call_center_supervisor_mode_t supervisor_mode;
    int64_t version;
} room_service_conference_policy_t;

typedef struct {
    int subscriptions_applied;
    int subscriptions_removed;
    int receiver_bandwidths_reconciled;
    int had_warning;
    char warning_code[64];
    char warning_message[256];
} room_service_conference_policy_apply_result_t;

#define ROOM_SERVICE_CALL_CENTER_EVENT_TYPE_MAX 32
#define ROOM_SERVICE_CALL_CENTER_EVENT_STATE_MAX 32
#define ROOM_SERVICE_CALL_CENTER_EVENT_DETAIL_MAX 64

typedef struct {
    int available;
    char room_id[TURBO_ROOM_ID_MAX];
    char event_type[ROOM_SERVICE_CALL_CENTER_EVENT_TYPE_MAX];
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    char peer_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char state[ROOM_SERVICE_CALL_CENTER_EVENT_STATE_MAX];
    char detail[ROOM_SERVICE_CALL_CENTER_EVENT_DETAIL_MAX];
    int64_t sequence;
} room_service_call_center_event_t;

typedef struct {
    int running;
    int room_sync_diagnostic_count;
    int call_center_event_count;
    int conference_policy_count;
    int sfu_node_count;
} room_service_app_stats_t;

typedef struct {
    int enabled;
    uint32_t workers;
    uint32_t worker_capacity;
    uint32_t worker_high_water;
    uint32_t assignments;
    uint32_t assignment_capacity;
    uint32_t assignment_high_water;
    uint32_t dialogs;
    uint32_t dialog_capacity;
    uint32_t dialog_high_water;
    uint64_t lease_expired_total;
    uint64_t dispatch_timeout_total;
    uint64_t release_timeout_total;
    uint32_t request_queue_items;
    uint32_t request_queue_capacity;
    uint32_t request_queue_high_water;
    uint64_t request_queue_drops_total;
    uint32_t peer_event_queue_items;
    uint32_t peer_event_queue_capacity;
    uint32_t peer_event_queue_high_water;
    uint64_t peer_event_queue_drops_total;
    int peer_event_queue_overflowed;
    int iris_provider_enabled;
    uint32_t iris_queue_items;
    uint32_t iris_queue_capacity;
    uint32_t iris_queue_high_water;
    uint32_t iris_in_flight;
    uint64_t iris_enqueued_total;
    uint64_t iris_queue_full_total;
    uint64_t iris_closed_rejections_total;
    uint64_t iris_delivery_attempts_total;
    uint64_t iris_retries_total;
    uint64_t iris_fence_conflicts_total;
    uint64_t iris_fence_refresh_failures_total;
    uint64_t iris_completion_success_total;
    uint64_t iris_completion_failure_total;
    uint64_t iris_event_success_total;
    uint64_t iris_event_failure_total;
    uint32_t iris_ledger_request_queue_items;
    uint32_t iris_ledger_request_queue_capacity;
    uint32_t iris_ledger_request_queue_high_water;
    uint64_t iris_ledger_record_capacity;
    uint64_t iris_ledger_claims_total;
    uint64_t iris_ledger_replays_total;
    uint64_t iris_ledger_conflicts_total;
    uint64_t iris_ledger_unknown_total;
    uint64_t iris_ledger_storage_failures_total;
    uint64_t iris_ledger_queue_rejections_total;
    uint64_t iris_ledger_recovered_unknown_total;
    uint64_t iris_ledger_resource_queries_total;
    uint64_t iris_ledger_resource_seen_total;
    uint64_t iris_ledger_retained_deleted_total;
    uint64_t iris_ledger_retention_sweeps_total;
    uint64_t iris_ledger_retention_failures_total;
    uint32_t iris_outbox_request_queue_items;
    uint32_t iris_outbox_request_queue_capacity;
    uint32_t iris_outbox_request_queue_high_water;
    uint32_t iris_outbox_pending_records;
    uint32_t iris_outbox_in_flight_records;
    uint32_t iris_outbox_dead_records;
    uint32_t iris_outbox_archived_records;
    uint64_t iris_outbox_record_capacity;
    uint64_t iris_outbox_retained_payload_bytes;
    uint64_t iris_outbox_peak_retained_payload_bytes;
    uint64_t iris_outbox_persisted_total;
    uint64_t iris_outbox_duplicate_total;
    uint64_t iris_outbox_conflict_total;
    uint64_t iris_outbox_persist_failure_total;
    uint64_t iris_outbox_capacity_rejection_total;
    uint64_t iris_outbox_schedule_rejection_total;
    uint64_t iris_outbox_delivered_total;
    uint64_t iris_outbox_dead_lettered_total;
    uint64_t iris_outbox_settlement_failure_total;
    uint64_t iris_outbox_stale_settlement_total;
    uint64_t iris_outbox_decode_failure_total;
    uint64_t iris_outbox_recovered_total;
    uint64_t iris_outbox_replayed_total;
    uint64_t iris_outbox_archived_total;
    uint64_t iris_outbox_archive_deleted_total;
    uint64_t iris_outbox_retention_failure_total;
    uint64_t iris_shutdown_restored_completions_total;
    uint64_t iris_shutdown_dropped_events_total;
    uint64_t iris_last_drain_duration_ms;
    uint64_t iris_max_drain_duration_ms;
    int iris_reconcile_state;
    int iris_reconcile_accepting_commands;
    uint32_t iris_reconcile_inventory_queue_items;
    uint32_t iris_reconcile_inventory_queue_capacity;
    uint64_t iris_reconcile_cycles_total;
    uint64_t iris_reconcile_failures_total;
    uint64_t iris_reconcile_expected_fetches_total;
    uint64_t iris_reconcile_inventory_pages_total;
    uint64_t iris_reconcile_rebound_total;
    uint64_t iris_reconcile_orphan_close_total;
    uint64_t iris_reconcile_resource_lost_total;
    uint64_t iris_reconcile_inventory_queue_full_total;
} room_service_ivr_metrics_t;

room_service_app_server_t *room_service_app_server_create(
    const room_service_app_config_t *config);
int room_service_app_server_start(room_service_app_server_t *server);
int room_service_app_server_run(room_service_app_server_t *server);
/* Control-thread lifecycle: stop is idempotent and destroy stops a running
 * server before releasing any producer/consumer dependency. */
void room_service_app_server_stop(room_service_app_server_t *server);
void room_service_app_server_destroy(room_service_app_server_t *server);
turbo_room_service_t *room_service_app_server_get_service(
    room_service_app_server_t *server);
const room_service_app_config_t *room_service_app_server_get_config(
    room_service_app_server_t *server);
int room_service_app_server_get_stats(room_service_app_server_t *server,
                                      room_service_app_stats_t *stats);
int room_service_app_server_get_ivr_metrics(
    room_service_app_server_t *server, room_service_ivr_metrics_t *metrics);
int room_service_app_server_register_sfu_node(room_service_app_server_t *server,
                                              const char *node_id,
                                              const char *control_url,
                                              const char *control_token);
int room_service_app_server_has_sfu_node(room_service_app_server_t *server,
                                         const char *node_id);
int room_service_app_server_choose_sfu_node(room_service_app_server_t *server,
                                            char *node_id, size_t node_id_size);
int room_service_app_server_sync_attach_room(room_service_app_server_t *server,
                                             const char *room_id);
int room_service_app_server_sync_detach_room(room_service_app_server_t *server,
                                             const char *room_id);
int room_service_app_server_sync_force_close_room(room_service_app_server_t *server,
                                                  const char *room_id);
int room_service_app_server_sync_add_session(room_service_app_server_t *server,
                                             const char *room_id,
                                             const char *participant_id);
int room_service_app_server_sync_remove_session(room_service_app_server_t *server,
                                                const char *room_id,
                                                const char *participant_id);
int room_service_app_server_sync_set_receiver_bandwidth(
    room_service_app_server_t *server, const char *room_id, const char *participant_id,
    int bandwidth_bps);
int room_service_app_server_sync_set_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id, int enabled,
    turbo_room_video_layer_t max_layer);
int room_service_app_server_sync_apply_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const turbo_room_subscription_summary_t *subscription);
int room_service_app_server_sync_start_recording(room_service_app_server_t *server,
                                                 const char *room_id,
                                                 const char *recording_id,
                                                 const char *mode);
int room_service_app_server_sync_stop_recording(room_service_app_server_t *server,
                                                const char *room_id);
int room_service_app_server_sync_ensure_recording(
    room_service_app_server_t *server, const char *room_id,
    const char *recording_id, const char *mode, int *started);
int room_service_app_server_sync_register_track(room_service_app_server_t *server,
                                                const char *room_id,
                                                const turbo_room_track_summary_t *track);
int room_service_app_server_sync_unregister_track(room_service_app_server_t *server,
                                                  const char *room_id,
                                                  const char *track_id);
int room_service_app_server_sync_replay_room_state(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_replay_stats_t *out_stats);
int room_service_app_server_sync_resync_room(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_replay_stats_t *out_stats);
int room_service_app_server_fetch_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id,
    room_service_sfu_track_subscription_t *subscription);
int room_service_app_server_fetch_participant_stats(
    room_service_app_server_t *server, const char *room_id, const char *participant_id,
    room_service_sfu_participant_stats_t *stats);
int room_service_app_server_fetch_recording_status(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_recording_status_t *status);
int room_service_app_server_record_room_sync(
    room_service_app_server_t *server, const char *room_id, const char *operation,
    const room_service_sfu_replay_stats_t *stats, const char *warning_code,
    const char *warning_message);
int room_service_app_server_get_room_sync_diagnostic(
    room_service_app_server_t *server, const char *room_id,
    room_service_room_sync_diagnostic_t *diagnostic);
int room_service_app_server_record_call_center_event(
    room_service_app_server_t *server, const char *room_id, const char *event_type,
    const char *participant_id, const char *peer_participant_id, const char *state,
    const char *detail);
int room_service_app_server_list_call_center_events(
    room_service_app_server_t *server, const char *room_id, int64_t after_sequence,
    int limit, room_service_call_center_event_t *events, int event_capacity,
    int *out_count, int64_t *out_latest_sequence);
int room_service_app_server_set_conference_layout_mode(
    room_service_app_server_t *server, const char *room_id, const char *layout_mode);
int room_service_app_server_set_conference_active_speaker(
    room_service_app_server_t *server, const char *room_id,
    const char *participant_id);
int room_service_app_server_set_conference_pin(room_service_app_server_t *server,
                                               const char *room_id,
                                               const char *participant_id);
int room_service_app_server_get_conference_policy(
    room_service_app_server_t *server, const char *room_id,
    room_service_conference_policy_t *policy);
int room_service_app_server_apply_conference_policy(
    room_service_app_server_t *server, const char *room_id,
    room_service_conference_policy_apply_result_t *result);
int room_service_app_server_apply_call_center_policy(
    room_service_app_server_t *server, const char *room_id,
    turbo_call_center_supervisor_mode_t supervisor_mode,
    room_service_conference_policy_apply_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ROOM_SERVICE_APP_SERVER_H */
