/**
 * @file server.h
 * @brief Room service application wrapper
 */
#ifndef TURBO_ROOM_SERVICE_APP_SERVER_H
#define TURBO_ROOM_SERVICE_APP_SERVER_H

#include "room_service/config.h"
#include "turbo_room_service.h"
#include "turbo_media_revocation_fanout.h"
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
} room_service_ivr_metrics_t;

room_service_app_server_t *room_service_app_server_create(
    const room_service_app_config_t *config);
int room_service_app_server_start(room_service_app_server_t *server);
int room_service_app_server_run(room_service_app_server_t *server);
/* Control-thread lifecycle: stop is idempotent and destroy stops a running
 * server before releasing any producer/consumer dependency. A nonzero result
 * retains ownership so the caller can retry after the blocking work drains. */
int room_service_app_server_stop(room_service_app_server_t *server);
int room_service_app_server_destroy(room_service_app_server_t *server);
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

/**
 * Register/update one SFU membership entry with explicit TLS identity metadata
 * for security-control revocation fan-out. This is the same SFU membership
 * used for room routing; no parallel node registry is created.
 */
int room_service_app_server_register_sfu_node_secure(
    room_service_app_server_t *server,
    const char *node_id,
    const char *control_url,
    const char *control_token,
    const char *security_server_name);

/**
 * Publish a canonical revocation snapshot to the current SFU membership.
 */
int room_service_app_server_publish_sfu_revocation_snapshot(
    room_service_app_server_t *server,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count,
    turbo_media_revocation_fanout_report_t *report);

/**
 * Publish one exact-next revoke. If SFU membership changed since the previous
 * publish, the implementation reconciles the rebuilt target set using the
 * caller-supplied covering snapshot instead of sending an incremental event.
 */
int room_service_app_server_publish_sfu_revocation(
    room_service_app_server_t *server,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex,
    const char *const *covering_sha256_hex,
    size_t covering_count,
    turbo_media_revocation_fanout_report_t *report);

uint64_t room_service_app_server_sfu_membership_version(
    room_service_app_server_t *server);
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
