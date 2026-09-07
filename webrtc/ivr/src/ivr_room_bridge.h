#ifndef TURBO_MEDIA_IVR_ROOM_BRIDGE_H
#define TURBO_MEDIA_IVR_ROOM_BRIDGE_H

/**
 * @file ivr_room_bridge.h
 * @brief RoomService-side H1 WebSocket bridge for IVR commands.
 *
 * Binds a CHTTP HTTP/1.1 WebSocket endpoint, decodes TIVR command frames
 * (generated
 * schema typed messages), enforces idempotency (message_id) and room version
 * checks, applies the command through a host-provided handler, and replies
 * with an IvrCommandResultV1 frame back to the worker. Processing runs on the
 * bridge's own worker thread: the WebSocket callback only captures the
 * generation-fenced route and enqueues a clone (send APIs are not re-entered
 * from the network
 * callback). The host (RoomService) owns the authoritative state and provides
 * get_room_version/on_command.
 */

#include "ivr/ivr_worker.h"
#include "ivr_control_gateway.h"
#include <cnet/cnet.h>

#include "data_bind.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_room_bridge_s ivr_room_bridge_t;

/* One decoded IVR command handed to the host handler. */
typedef struct {
    uint32_t wire_version;
    char message_id[128];
    char assignment_id[128];
    char attempt_id[128];
    char worker_id[128];
    char worker_instance_id[128];
    uint64_t worker_connection_generation;
    char room_id[128];
    char call_id[128];
    uint64_t call_generation;
    uint64_t expected_room_version;
    uint64_t current_room_version;
    char command[64];      /* e.g. "conference.join" */
    char participant_role[32]; /* e.g. "caller" (ConferenceJoinCommandV1) */
    char instance_id[128];
    uint64_t connection_generation;
    uint32_t max_sessions;
    uint32_t active_sessions;
    uint32_t reserved_sessions;
    uint64_t lease_duration_ms;
    int draining;
    uint64_t health_generation;
    int health_ready;
    char capabilities[256];
} ivr_room_command_t;

typedef struct {
    int status_code; /* 0 = applied; negative ivr_status_t error */
    uint64_t room_version;
    uint64_t sequence;
    char error_message[128];
    /* Internal bridge policy; not serialized. Retryable failures are not put
       in the message_id dedup cache, so the same stable command can resume
       after its external side effect becomes available. */
    int retryable;
} ivr_room_command_result_t;

/* One worker-owned dispatch outcome. The bridge validates that the result
   arrived over the currently registered worker route before notifying the
   host on the bridge worker thread. */
typedef struct {
    uint32_t wire_version;
    char message_id[128];
    char assignment_id[128];
    char attempt_id[128];
    char worker_id[128];
    char worker_instance_id[128];
    uint64_t worker_connection_generation;
    char room_id[128];
    char call_id[128];
    uint64_t call_generation;
    int status_code;
    uint32_t active_sessions;
    uint32_t max_sessions;
    char error_code[64];
    char error_message[128];
} ivr_dispatch_result_t;

typedef ivr_dispatch_result_t ivr_release_result_t;

#define IVR_MEDIA_EVENT_PAYLOAD_CAPACITY 4096u

/* Owning media observations forwarded toward the Iris/provider adapter.
   These are transport facts only; consuming them must not advance RoomService
   business state. */
typedef struct {
    char message_id[128];
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char worker_id[128];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t operation_generation;
    int status_code;
    char error_code[64];
    char error_message[128];
} ivr_media_command_result_t;

typedef struct {
    char event_id[128];
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char worker_id[128];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t sequence;
    char event_type[128];
    uint64_t occurred_at_ms;
    char input_id[IVR_MEDIA_ID_CAPACITY];
    char input_value[IVR_MEDIA_ID_CAPACITY];
    char payload_json[IVR_MEDIA_EVENT_PAYLOAD_CAPACITY];
} ivr_media_event_t;

typedef struct {
    void *context;
    /* Current authoritative room version; 0 skips the expected-version check. */
    uint64_t (*get_room_version)(void *ctx, const char *room_id);
    /* Validate and apply one command. Fill *result. */
    ivr_status_t (*on_command)(void *ctx, const ivr_room_command_t *command,
                               ivr_room_command_result_t *result);
    /* Observe one authenticated dispatch result. Runs on the bridge worker
       thread, never in the WebSocket callback. */
    ivr_status_t (*on_dispatch_result)(void *ctx,
                                       const ivr_dispatch_result_t *result);
    ivr_status_t (*on_release_result)(void *ctx,
                                      const ivr_release_result_t *result);
    /* Authenticated media facts. The bridge invokes these on its owner thread
       and never interprets them as workflow transitions. */
    ivr_status_t (*on_media_result)(
        void *ctx, const ivr_media_command_result_t *result);
    ivr_status_t (*on_media_event)(void *ctx,
                                   const ivr_media_event_t *event);
    /* Authenticated worker inventory page. The page and strings are owning
       copies valid for the callback; retain by copying if needed later. */
    ivr_status_t (*on_inventory_page)(
        void *ctx, const ivr_worker_inventory_envelope_t *result);
    /* Periodic owner-loop tick for bounded deadlines/lease transitions. */
    void (*on_tick)(void *ctx);
} ivr_room_command_handler_t;

typedef struct {
    const char *host; /* bind address */
    int port;
    const char *path; /* HTTP/1.1 WebSocket route; NULL = default route. */
    uint64_t timeout_ms;
    /* Borrowed TLS material; CHTTP/CNet copies it during create. */
    const cnet_tls_server_config *tls;
    uint32_t queue_capacity;  /* bounded cloned-request queue; 0 = 64 */
    uint32_t dedup_capacity;  /* bounded message_id result cache; 0 = 64 */
    /* Replay/dedup retention window in milliseconds. A mutation replayed after
       its stored result is older than this window is explicitly rejected
       (IVR_ESTALE) instead of silently re-applied. 0 = 60000. */
    uint64_t dedup_retention_ms;
    /* Injectable monotonic clock for the retention window; NULL falls back to
       a process-monotonic clock. */
    uint64_t (*now_ms)(void *ctx);
    void *now_ctx;
    ivr_room_command_handler_t handler;
    /* Optional mTLS identity verifier. Inputs are callback-borrowed. */
    int (*verify_peer_identity)(void *context,
                                const char *certificate_sha256,
                                const char *claimed_identity);
    void *verify_peer_identity_context;
} ivr_room_bridge_config_t;

ivr_status_t ivr_room_bridge_create(const ivr_room_bridge_config_t *config,
                                    ivr_room_bridge_t **out_bridge);
ivr_status_t ivr_room_bridge_start(ivr_room_bridge_t *bridge);
ivr_status_t ivr_room_bridge_stop(ivr_room_bridge_t *bridge);
ivr_status_t ivr_room_bridge_destroy(ivr_room_bridge_t *bridge);

/* Pure decode of one TIVR command frame into an ivr_room_command_t. */
ivr_status_t ivr_room_decode_frame(DataBind *codec, const uint8_t *frame,
                                   size_t len, ivr_room_command_t *out);

/* Pure decode of CallDispatchResultV1 (kind=result). */
ivr_status_t ivr_room_decode_dispatch_result(DataBind *codec,
                                             const uint8_t *frame, size_t len,
                                             ivr_dispatch_result_t *out);
ivr_status_t ivr_room_decode_release_result(DataBind *codec,
                                            const uint8_t *frame, size_t len,
                                            ivr_release_result_t *out);
ivr_status_t ivr_room_decode_media_result(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_media_command_result_t *out);
ivr_status_t ivr_room_decode_media_event(DataBind *codec,
                                         const uint8_t *frame, size_t len,
                                         ivr_media_event_t *out);

/* Pure encode of a ConferenceParticipantJoinedEventV1 domain event frame. */
ivr_status_t ivr_room_bridge_encode_participant_joined(
    DataBind *codec, const char *event_id, const char *causation_id,
    const char *worker_id, const char *room_id, const char *call_id,
    uint64_t call_generation, uint64_t room_version, uint64_t sequence,
    uint64_t occurred_at_ms, const char *participant_id,
    const char *participant_role, uint8_t *frame, size_t frame_cap,
    size_t *out_len);

/* Send one committed participant-joined fact to the target worker route after
   it accepted its dispatch. */
ivr_status_t ivr_room_bridge_publish_participant_joined(
    ivr_room_bridge_t *bridge, const char *event_id,
    const char *causation_id, const char *worker_id, const char *room_id,
    const char *call_id, uint64_t call_generation, uint64_t room_version,
    uint64_t sequence, const char *participant_id,
    const char *participant_role);

/* Publish the terminal fact committed after an IVR worker route/lease is
   lost. The caller retains its outbox marker until this returns IVR_OK. */
ivr_status_t ivr_room_bridge_publish_worker_lost(
    ivr_room_bridge_t *bridge, const char *event_id,
    const char *causation_id, const char *worker_id, const char *room_id,
    const char *call_id, uint64_t call_generation, uint64_t room_version,
    uint64_t sequence, const char *reason);

/* Pure encode of a RoomSnapshotV1 snapshot frame (kind=snapshot). Empty
   participant_role/call_state are allowed (the session recovery only reads
   room_version/sequence). */
ivr_status_t ivr_room_bridge_encode_snapshot(
    DataBind *codec, const char *room_id, const char *call_id,
    uint64_t call_generation, uint64_t room_version, uint64_t last_sequence,
    const char *participant_role, const char *call_state, uint8_t *frame,
    size_t frame_cap, size_t *out_len);

/* Pure encode of a CallDispatchCommandV1 dispatch frame (kind=command). */
ivr_status_t ivr_room_bridge_encode_dispatch(
    DataBind *codec, const char *message_id, const char *worker_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    uint64_t expected_room_version, const char *content_package,
    uint8_t *frame, size_t frame_cap, size_t *out_len);

ivr_status_t ivr_room_bridge_encode_dispatch_v2(
    DataBind *codec, const ivr_call_dispatch_t *dispatch, uint8_t *frame,
    size_t frame_cap, size_t *out_len);

/* Push a dispatch command to a registered worker over its WebSocket route using the
   route captured at that worker's worker.sync registration. Returns IVR_OK on
   local delivery, IVR_ESTATE when the worker has no valid route (not
   registered / stale after reconnect). */
ivr_status_t ivr_room_bridge_dispatch_call(
    ivr_room_bridge_t *bridge, const char *worker_id, const char *message_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    uint64_t expected_room_version, const char *content_package);
ivr_status_t ivr_room_bridge_dispatch_call_v2(
    ivr_room_bridge_t *bridge, const ivr_call_dispatch_t *dispatch);

ivr_status_t ivr_room_bridge_encode_release(
    DataBind *codec, const char *message_id, const char *worker_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    const char *reason, uint8_t *frame, size_t frame_cap, size_t *out_len);
ivr_status_t ivr_room_bridge_release_call(
    ivr_room_bridge_t *bridge, const char *worker_id, const char *message_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    const char *reason);

/* Encode/send one Iris-owned media command to the authenticated worker route.
   No RoomService business state is read or changed by these functions. */
ivr_status_t ivr_room_bridge_encode_media_command(
    DataBind *codec, const ivr_media_command_t *command, uint8_t *frame,
    size_t frame_cap, size_t *out_len);
ivr_status_t ivr_room_bridge_send_media_command(
    ivr_room_bridge_t *bridge, const ivr_media_command_t *command);

/* RoomService-side inventory query and result codec. The worker route must be
   registered; send performs no waiting and the result arrives through
   on_inventory_page on the bridge owner thread. */
ivr_status_t ivr_room_bridge_encode_inventory_query(
    DataBind *codec, const ivr_worker_inventory_request_t *request,
    uint8_t *frame, size_t frame_capacity, size_t *out_size);
ivr_status_t ivr_room_bridge_request_inventory(
    ivr_room_bridge_t *bridge,
    const ivr_worker_inventory_request_t *request);
ivr_status_t ivr_room_decode_inventory_page(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_worker_inventory_envelope_t *out);

/* True only while the registered worker still has a live WebSocket peer and a
   reusable generation-fenced route. A disconnect invalidates it before
   another dispatch can select it; worker.sync after reconnect refreshes it. */
int ivr_room_bridge_worker_available(const ivr_room_bridge_t *bridge,
                                     const char *worker_id);

typedef struct {
    uint32_t request_queue_items;
    uint32_t request_queue_capacity;
    uint32_t request_queue_high_water;
    uint64_t request_queue_drops;
    uint32_t peer_event_queue_items;
    uint32_t peer_event_queue_capacity;
    uint32_t peer_event_queue_high_water;
    uint64_t peer_event_queue_drops;
    int peer_event_queue_overflowed;
    uint64_t dedup_hits;
    uint64_t dedup_expired_rejects;
    uint64_t version_rejects;
    uint64_t auth_rejects;
    uint64_t dispatch_result_rejects;
    uint64_t media_results;
    uint64_t media_result_rejects;
    uint64_t media_events;
    uint64_t media_event_rejects;
    uint64_t inventory_pages;
    uint64_t inventory_page_rejects;
} ivr_room_bridge_stats_t;

/* Thread-safe, read-only bounded-queue observation. */
void ivr_room_bridge_get_stats(const ivr_room_bridge_t *bridge,
                               ivr_room_bridge_stats_t *out);

/* Pure encode of a WorkerSyncResultV1 registration reply frame. */
ivr_status_t ivr_room_encode_worker_sync_result(
    DataBind *codec, const char *message_id, const char *worker_id,
    const ivr_room_command_result_t *result, uint8_t *frame, size_t frame_cap,
    size_t *out_len);

/* Pure encode of an IvrCommandResultV1 reply frame. */
ivr_status_t ivr_room_encode_result(DataBind *codec, const char *message_id,
                                    const char *worker_id,
                                    const char *room_id, const char *call_id,
                                    uint64_t call_generation,
                                    const ivr_room_command_result_t *result,
                                    uint8_t *frame, size_t frame_cap,
                                    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_ROOM_BRIDGE_H */
