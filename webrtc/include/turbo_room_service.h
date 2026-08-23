/**
 * Room service model for conference control-plane state.
 */
#ifndef TURBO_ROOM_SERVICE_H
#define TURBO_ROOM_SERVICE_H

#include <turbo_export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_ROOM_ID_MAX 64
#define TURBO_PARTICIPANT_ID_MAX 64
#define TURBO_TRACK_ID_MAX 64
#define TURBO_USER_ID_MAX 64
#define TURBO_DISPLAY_NAME_MAX 128
#define TURBO_NODE_ID_MAX 64
#define TURBO_CODEC_NAME_MAX 32
#define TURBO_POLICY_SOURCE_MAX 32
#define TURBO_RECORDING_ID_MAX 64
#define TURBO_RECORDING_MODE_MAX 32
#define TURBO_CALL_CENTER_QUEUE_ID_MAX 64
#define TURBO_CALL_CENTER_QUEUE_ENTRY_ID_MAX 64
#define TURBO_CALL_CENTER_ENDPOINT_ID_MAX 64
#define TURBO_CALL_CENTER_DISPOSITION_CODE_MAX 64

typedef struct turbo_room_service_s turbo_room_service_t;

typedef enum {
    TURBO_ROOM_TYPE_CALL = 1,
    TURBO_ROOM_TYPE_CONFERENCE = 2,
    TURBO_ROOM_TYPE_WEBINAR = 3
} turbo_room_type_t;

typedef enum {
    TURBO_ROOM_STATUS_OPEN = 1,
    TURBO_ROOM_STATUS_ACTIVE = 2,
    TURBO_ROOM_STATUS_CLOSING = 3,
    TURBO_ROOM_STATUS_CLOSED = 4
} turbo_room_status_t;

typedef enum {
    TURBO_PARTICIPANT_ROLE_HOST = 1,
    TURBO_PARTICIPANT_ROLE_COHOST = 2,
    TURBO_PARTICIPANT_ROLE_GUEST = 3,
    TURBO_PARTICIPANT_ROLE_AUDIENCE = 4,
    TURBO_PARTICIPANT_ROLE_CUSTOMER = 5,
    TURBO_PARTICIPANT_ROLE_AGENT = 6,
    TURBO_PARTICIPANT_ROLE_SUPERVISOR = 7,
    TURBO_PARTICIPANT_ROLE_QA_OBSERVER = 8,
    TURBO_PARTICIPANT_ROLE_BOT = 9
} turbo_participant_role_t;

typedef enum {
    TURBO_PARTICIPANT_JOIN_INVITED = 1,
    TURBO_PARTICIPANT_JOIN_JOINING = 2,
    TURBO_PARTICIPANT_JOIN_JOINED = 3,
    TURBO_PARTICIPANT_JOIN_LEAVING = 4,
    TURBO_PARTICIPANT_JOIN_LEFT = 5
} turbo_participant_join_state_t;

typedef enum {
    TURBO_PARTICIPANT_SESSION_NONE = 1,
    TURBO_PARTICIPANT_SESSION_NEGOTIATING = 2,
    TURBO_PARTICIPANT_SESSION_CONNECTED = 3,
    TURBO_PARTICIPANT_SESSION_RECONNECTING = 4,
    TURBO_PARTICIPANT_SESSION_FAILED = 5,
    TURBO_PARTICIPANT_SESSION_CLOSED = 6
} turbo_participant_session_state_t;

typedef enum {
    TURBO_ROOM_TRACK_AUDIO = 1,
    TURBO_ROOM_TRACK_VIDEO = 2
} turbo_room_track_kind_t;

typedef enum {
    TURBO_ROOM_SOURCE_MIC = 1,
    TURBO_ROOM_SOURCE_CAMERA = 2,
    TURBO_ROOM_SOURCE_SCREEN = 3
} turbo_room_track_source_t;

typedef enum {
    TURBO_ROOM_VIDEO_LAYER_NONE = 0,
    TURBO_ROOM_VIDEO_LAYER_LOW = 1,
    TURBO_ROOM_VIDEO_LAYER_MEDIUM = 2,
    TURBO_ROOM_VIDEO_LAYER_HIGH = 3
} turbo_room_video_layer_t;

typedef enum {
    TURBO_ROOM_RECORDING_STOPPED = 1,
    TURBO_ROOM_RECORDING_STARTING = 2,
    TURBO_ROOM_RECORDING_ACTIVE = 3,
    TURBO_ROOM_RECORDING_STOPPING = 4,
    TURBO_ROOM_RECORDING_FAILED = 5
} turbo_room_recording_state_t;

typedef enum {
    TURBO_ROOM_LAYOUT_GRID = 1,
    TURBO_ROOM_LAYOUT_SPEAKER = 2
} turbo_room_layout_mode_t;

typedef enum {
    TURBO_CALL_CENTER_SUPERVISOR_NONE = 0,
    TURBO_CALL_CENTER_SUPERVISOR_MONITOR = 1,
    TURBO_CALL_CENTER_SUPERVISOR_WHISPER = 2,
    TURBO_CALL_CENTER_SUPERVISOR_BARGE = 3
} turbo_call_center_supervisor_mode_t;

typedef enum {
    TURBO_CALL_CENTER_QUEUE_CALLER = 1,
    TURBO_CALL_CENTER_QUEUE_CALLEE = 2
} turbo_call_center_queue_side_t;

typedef enum {
    TURBO_CALL_CENTER_AGENT_AVAILABLE = 1,
    TURBO_CALL_CENTER_AGENT_RESERVED = 2,
    TURBO_CALL_CENTER_AGENT_BUSY = 3,
    TURBO_CALL_CENTER_AGENT_OFFLINE = 4,
    TURBO_CALL_CENTER_AGENT_WRAP_UP = 5
} turbo_call_center_agent_state_t;

typedef enum {
    TURBO_CALL_CENTER_ROOM_NONE = 0,
    TURBO_CALL_CENTER_ROOM_ACTIVE = 1,
    TURBO_CALL_CENTER_ROOM_WRAP_UP = 2,
    TURBO_CALL_CENTER_ROOM_COMPLETED = 3
} turbo_call_center_room_state_t;

typedef struct {
    const char *room_id;
    uint64_t room_generation;
    turbo_room_type_t room_type;
    const char *created_by;
} turbo_room_config_t;

typedef struct {
    const char *participant_id;
    uint64_t call_generation;
    const char *user_id;
    const char *display_name;
    turbo_participant_role_t role;
} turbo_room_participant_config_t;

typedef struct {
    const char *track_id;
    const char *owner_participant_id;
    turbo_room_track_kind_t kind;
    turbo_room_track_source_t source;
    const char *codec_name;
    int simulcast_enabled;
    uint32_t main_ssrc;
    const uint32_t *layer_ssrcs;
    int layer_count;
} turbo_room_track_config_t;

typedef struct {
    const char *subscriber_participant_id;
    const char *track_id;
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    const char *policy_source;
} turbo_room_subscription_config_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    uint64_t room_generation;
    turbo_room_type_t room_type;
    turbo_room_status_t status;
    char assigned_sfu_node[TURBO_NODE_ID_MAX];
    turbo_room_recording_state_t recording_state;
    turbo_room_layout_mode_t layout_mode;
    char active_speaker_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char pinned_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char recording_id[TURBO_RECORDING_ID_MAX];
    char recording_mode[TURBO_RECORDING_MODE_MAX];
    int participant_count;
    int published_track_count;
    int subscription_count;
    int64_t version;
} turbo_room_summary_t;

typedef struct {
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    uint64_t call_generation;
    char user_id[TURBO_USER_ID_MAX];
    char display_name[TURBO_DISPLAY_NAME_MAX];
    turbo_participant_role_t role;
    turbo_participant_join_state_t join_state;
    turbo_participant_session_state_t session_state;
    int bandwidth_bps;
    int64_t version;
} turbo_room_participant_summary_t;

typedef struct {
    char track_id[TURBO_TRACK_ID_MAX];
    char owner_participant_id[TURBO_PARTICIPANT_ID_MAX];
    turbo_room_track_kind_t kind;
    turbo_room_track_source_t source;
    char codec_name[TURBO_CODEC_NAME_MAX];
    int simulcast_enabled;
    int muted;
    uint32_t main_ssrc;
    uint32_t layer_ssrcs[3];
    int layer_count;
    int64_t version;
} turbo_room_track_summary_t;

typedef struct {
    char subscriber_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
    int64_t version;
} turbo_room_subscription_summary_t;

typedef struct {
    const char *queue_id;
    turbo_call_center_queue_side_t side;
    const char *entry_id;
    const char *endpoint_id;
    int priority;
} turbo_call_center_queue_entry_config_t;

typedef struct {
    char queue_id[TURBO_CALL_CENTER_QUEUE_ID_MAX];
    turbo_call_center_queue_side_t side;
    char entry_id[TURBO_CALL_CENTER_QUEUE_ENTRY_ID_MAX];
    char endpoint_id[TURBO_CALL_CENTER_ENDPOINT_ID_MAX];
    int priority;
    int64_t sequence;
    int64_t version;
} turbo_call_center_queue_entry_summary_t;

typedef struct {
    int matched;
    turbo_call_center_queue_entry_summary_t caller;
    turbo_call_center_queue_entry_summary_t callee;
} turbo_call_center_queue_match_summary_t;

typedef struct {
    char endpoint_id[TURBO_CALL_CENTER_ENDPOINT_ID_MAX];
    turbo_call_center_agent_state_t state;
    int explicit_state;
    int64_t version;
} turbo_call_center_agent_state_summary_t;

typedef struct {
    int available;
    char room_id[TURBO_ROOM_ID_MAX];
    turbo_call_center_room_state_t state;
    char customer_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char agent_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char consult_agent_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char disposition_code[TURBO_CALL_CENTER_DISPOSITION_CODE_MAX];
    int64_t version;
} turbo_call_center_room_summary_t;

TURBO_MEDIA_API turbo_room_service_t *turbo_room_service_create(void);
TURBO_MEDIA_API void turbo_room_service_destroy(turbo_room_service_t *service);

TURBO_MEDIA_API int turbo_room_service_create_room(turbo_room_service_t *service,
                                             const turbo_room_config_t *config);
TURBO_MEDIA_API int turbo_room_service_close_room(turbo_room_service_t *service,
                                            const char *room_id);
TURBO_MEDIA_API int turbo_room_service_discard_unassigned_room(
    turbo_room_service_t *service, const char *room_id);
TURBO_MEDIA_API int turbo_room_service_assign_sfu_node(turbo_room_service_t *service,
                                                 const char *room_id,
                                                 const char *node_id);
TURBO_MEDIA_API int turbo_room_service_get_room_summary(turbo_room_service_t *service,
                                                  const char *room_id,
                                                  turbo_room_summary_t *summary);

TURBO_MEDIA_API int turbo_room_service_add_participant(
    turbo_room_service_t *service, const char *room_id,
    const turbo_room_participant_config_t *config);
TURBO_MEDIA_API int turbo_room_service_remove_participant(turbo_room_service_t *service,
                                                    const char *room_id,
                                                    const char *participant_id);
TURBO_MEDIA_API int turbo_room_service_set_participant_session_state(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    turbo_participant_session_state_t session_state);
TURBO_MEDIA_API int turbo_room_service_set_participant_bandwidth(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    int bandwidth_bps);
TURBO_MEDIA_API int turbo_room_service_get_participant_summary(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    turbo_room_participant_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_get_participant_summary_at(
    turbo_room_service_t *service, const char *room_id, int index,
    turbo_room_participant_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_get_effective_receiver_bandwidth(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    int *bandwidth_bps, int *is_explicit);

TURBO_MEDIA_API int turbo_room_service_publish_track(
    turbo_room_service_t *service, const char *room_id,
    const turbo_room_track_config_t *config);
TURBO_MEDIA_API int turbo_room_service_unpublish_track(turbo_room_service_t *service,
                                                 const char *room_id,
                                                 const char *track_id);
TURBO_MEDIA_API int turbo_room_service_set_track_muted(turbo_room_service_t *service,
                                                 const char *room_id,
                                                 const char *track_id,
                                                 int muted);
TURBO_MEDIA_API int turbo_room_service_get_track_summary(
    turbo_room_service_t *service, const char *room_id, const char *track_id,
    turbo_room_track_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_get_track_summary_at(
    turbo_room_service_t *service, const char *room_id, int index,
    turbo_room_track_summary_t *summary);

TURBO_MEDIA_API int turbo_room_service_set_subscription(
    turbo_room_service_t *service, const char *room_id,
    const turbo_room_subscription_config_t *config);
TURBO_MEDIA_API int turbo_room_service_remove_subscription(
    turbo_room_service_t *service, const char *room_id,
    const char *subscriber_participant_id, const char *track_id);
TURBO_MEDIA_API int turbo_room_service_get_subscription_summary(
    turbo_room_service_t *service, const char *room_id,
    const char *subscriber_participant_id, const char *track_id,
    turbo_room_subscription_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_get_subscription_summary_at(
    turbo_room_service_t *service, const char *room_id, int index,
    turbo_room_subscription_summary_t *summary);

TURBO_MEDIA_API int turbo_room_service_start_recording(turbo_room_service_t *service,
                                                 const char *room_id,
                                                 const char *recording_id,
                                                 const char *mode);
TURBO_MEDIA_API int turbo_room_service_stop_recording(turbo_room_service_t *service,
                                                const char *room_id);
TURBO_MEDIA_API int turbo_room_service_set_layout_mode(
    turbo_room_service_t *service, const char *room_id,
    turbo_room_layout_mode_t layout_mode);
TURBO_MEDIA_API int turbo_room_service_set_active_speaker(
    turbo_room_service_t *service, const char *room_id,
    const char *participant_id);
TURBO_MEDIA_API int turbo_room_service_pin_participant(
    turbo_room_service_t *service, const char *room_id,
    const char *participant_id);
TURBO_MEDIA_API int turbo_room_service_reconcile_subscriptions(
    turbo_room_service_t *service, const char *room_id);
TURBO_MEDIA_API int turbo_room_service_reconcile_call_center_subscriptions(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_supervisor_mode_t supervisor_mode);
TURBO_MEDIA_API int turbo_room_service_enqueue_call_center_queue_entry(
    turbo_room_service_t *service,
    const turbo_call_center_queue_entry_config_t *config);
TURBO_MEDIA_API int turbo_room_service_remove_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side, const char *entry_id);
TURBO_MEDIA_API int turbo_room_service_get_call_center_queue_depth(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side, int *depth);
TURBO_MEDIA_API int turbo_room_service_peek_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side,
    turbo_call_center_queue_entry_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_pop_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side,
    turbo_call_center_queue_entry_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_match_call_center_queue(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_match_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_claim_call_center_queue_match(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_match_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_complete_call_center_queue_match(
    turbo_room_service_t *service, const char *queue_id,
    const char *caller_entry_id, const char *callee_entry_id);
TURBO_MEDIA_API int turbo_room_service_rollback_call_center_queue_match(
    turbo_room_service_t *service, const char *queue_id,
    const char *caller_entry_id, const char *callee_entry_id);
TURBO_MEDIA_API int turbo_room_service_recover_stale_call_center_queue_claims(
    turbo_room_service_t *service, const char *queue_id,
    uint64_t lease_ms, int *recovered);
TURBO_MEDIA_API int turbo_room_service_set_call_center_agent_state(
    turbo_room_service_t *service, const char *endpoint_id,
    turbo_call_center_agent_state_t state);
TURBO_MEDIA_API int turbo_room_service_get_call_center_agent_state(
    turbo_room_service_t *service, const char *endpoint_id,
    turbo_call_center_agent_state_summary_t *summary);
TURBO_MEDIA_API int turbo_room_service_start_call_center_room(
    turbo_room_service_t *service, const char *room_id,
    const char *customer_participant_id, const char *agent_participant_id);
TURBO_MEDIA_API int turbo_room_service_set_call_center_consult_agent(
    turbo_room_service_t *service, const char *room_id,
    const char *consult_agent_participant_id);
TURBO_MEDIA_API int turbo_room_service_complete_call_center_transfer(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_agent_state_t released_agent_state);
TURBO_MEDIA_API int turbo_room_service_finalize_call_center_room(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_room_state_t state, const char *disposition_code,
    turbo_call_center_agent_state_t agent_state);
TURBO_MEDIA_API int turbo_room_service_get_call_center_room_summary(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_room_summary_t *summary);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ROOM_SERVICE_H */
