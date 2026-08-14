#ifndef TURBO_MEDIA_IVR_FLOWMQ_GATEWAY_H
#define TURBO_MEDIA_IVR_FLOWMQ_GATEWAY_H

/**
 * @file ivr_flowmq_gateway.h
 * @brief FlowMQ DEALER command gateway (compiled when TURBO_MEDIA_HAS_FLOWMQ).
 *
 * Implements an adapter-local command sender over a FlowMQ DEALER endpoint
 * (CONNECT to the RoomService ROUTER). Commands are encoded as TIVR frames
 * (12-byte header + DataBind BIN payload) using the generated
 * turbomedia_ivr_v1 schema; the payload is the per-command typed message.
 * Commands without a RoomService schema message (rtc.*, accept, disconnect)
 * are worker-local and are rejected here with IVR_ESTATE.
 */

#include "ivr/ivr_worker.h"
#include "data_bind.h"

typedef struct turbo_flow_fmq_security_binding_s
    turbo_flow_fmq_security_binding_t;
typedef struct turbo_flow_fmq_tls_config_s turbo_flow_fmq_tls_config_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_flowmq_gateway_s ivr_flowmq_gateway_t;

/* Adapter-only envelope for RoomService control commands. This is not an IVR
   worker or workflow API; Iris-facing media commands use their own typed wire
   messages. */
typedef struct {
    ivr_bytes_view_t message_id;
    ivr_bytes_view_t command_type;
    ivr_call_ref_t call;
    ivr_bytes_view_t args_json;
} ivr_command_view_t;

typedef struct {
    uint32_t abi_version;
    void *context;
    ivr_status_t (*submit_copy)(void *context,
                                const ivr_command_view_t *command);
} ivr_command_gateway_ops_t;

typedef struct {
    const char *worker_id; /* DEALER identity; also stamped on commands */
    const char *host;      /* ROUTER host */
    int port;
    int transport;         /* turbo_flow_fmq_transport_t; 0 = TCP */
    const char *path;      /* WS/WSS path; NULL = "/" */
    uint64_t timeout_ms;   /* 0 = 5000 */
    uint64_t reconnect_initial_ms; /* 0 = 1000 */
    uint64_t reconnect_max_ms;     /* 0 = 30000 */
    /* Borrowed object-level TLS/WSS material; FlowMQ copies it during create. */
    const turbo_flow_fmq_tls_config_t *tls;
    /* Borrowed secure binding; when non-NULL create uses the mandatory-secure
       FlowMQ facade and fails closed on authentication/ACL errors. */
    const turbo_flow_fmq_security_binding_t *security;
    /* Optional DEALER reply ingress (command results from the ROUTER).
       The frame bytes are borrowed for the call only; copy to retain. */
    void (*on_reply)(void *ctx, const uint8_t *frame, size_t len);
    void *reply_ctx;
    /* Transport callback; invoked from the FlowMQ execution context. The
       receiver must only copy/publish state and must not re-enter FlowMQ. */
    void (*on_connection)(void *ctx, int connected);
    void *connection_ctx;
} ivr_flowmq_gateway_config_t;

ivr_status_t ivr_flowmq_gateway_create(const ivr_flowmq_gateway_config_t *config,
                                       ivr_command_gateway_ops_t *ops,
                                       ivr_flowmq_gateway_t **out_gateway);

/* Start the DEALER facade (connect + receive loop). Required before send. */
ivr_status_t ivr_flowmq_gateway_start(ivr_flowmq_gateway_t *gateway);

void ivr_flowmq_gateway_destroy(ivr_flowmq_gateway_t *gateway);

/* Encode one command as a TIVR frame (header + BIN payload). Pure function:
   no network is touched, so it is unit-testable without a FlowMQ peer.
   Returns IVR_ESTATE for commands without a RoomService schema message. */
ivr_status_t ivr_flowmq_gateway_encode_command(DataBind *codec,
                                               const ivr_command_view_t *command,
                                               const char *worker_id,
                                               uint8_t *frame, size_t frame_cap,
                                               size_t *out_len);

/* Encode one worker.sync registration frame (WorkerSyncCommandV1; only
   message_id + worker_id, no per-call fields). Pure function. */
ivr_status_t ivr_flowmq_gateway_encode_worker_sync(DataBind *codec,
                                                   const char *message_id,
                                                   const char *worker_id,
                                                   uint8_t *frame,
                                                   size_t frame_cap,
                                                   size_t *out_len);

/* Send worker.sync on the DEALER channel (worker-level registration; the
   WorkerSyncResultV1 reply arrives through the configured on_reply). */
ivr_status_t ivr_flowmq_gateway_send_worker_sync(ivr_flowmq_gateway_t *gateway,
                                                 const char *message_id);

typedef struct {
    const char *instance_id;
    uint64_t connection_generation;
    uint32_t max_sessions;
    uint32_t active_sessions;
    uint32_t reserved_sessions;
    uint64_t lease_duration_ms;
    int draining;
    uint64_t health_generation;
    int health_ready;
    const char *capabilities;
} ivr_worker_status_view_t;

/* Encode/send production worker registration and lease renewal commands.
   WorkerSyncCommandV1 remains available for wire compatibility only. */
ivr_status_t ivr_flowmq_gateway_encode_worker_sync_v2(
    DataBind *codec, const char *message_id, const char *worker_id,
    const ivr_worker_status_view_t *status, uint8_t *frame, size_t frame_cap,
    size_t *out_len);
ivr_status_t ivr_flowmq_gateway_encode_worker_heartbeat(
    DataBind *codec, const char *message_id, const char *worker_id,
    const ivr_worker_status_view_t *status, uint8_t *frame, size_t frame_cap,
    size_t *out_len);
ivr_status_t ivr_flowmq_gateway_send_worker_sync_v2(
    ivr_flowmq_gateway_t *gateway, const char *message_id,
    const ivr_worker_status_view_t *status);
ivr_status_t ivr_flowmq_gateway_send_worker_heartbeat(
    ivr_flowmq_gateway_t *gateway, const char *message_id,
    const ivr_worker_status_view_t *status);

/* Send one raw TIVR frame on the DEALER channel. Used for pre-encoded frames
   (e.g. negative identity tests) and future command types. */
ivr_status_t ivr_flowmq_gateway_send_frame(ivr_flowmq_gateway_t *gateway,
                                           const uint8_t *frame, size_t len);

/* One call dispatch pushed by the RoomService ROUTER to this worker. */
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
    char content_package[64];
    char content_version[64];
    uint64_t deadline_timeout_ms;
} ivr_call_dispatch_t;

/* Worker-owned outcome of one pushed dispatch. The frame is sent back on the
   worker DEALER only after content, media and the session slot have been
   created (or rejected with a bounded error). */
typedef struct {
    char message_id[128];
    char worker_id[128];
    char room_id[128];
    char call_id[128];
    uint64_t call_generation;
    int status_code;
    char error_code[64];
    char error_message[128];
} ivr_call_dispatch_result_t;

typedef struct {
    char message_id[128];
    char worker_id[128];
    char room_id[128];
    char call_id[128];
    uint64_t call_generation;
    char reason[64];
} ivr_call_release_t;

/* One decoded IvrCommandResultV1 reply. All strings are owned by this
   envelope, so callers may retain it after the borrowed FlowMQ callback
   returns. */
typedef struct {
    char message_id[128];
    char worker_id[128];
    char room_id[128];
    char call_id[128];
    uint64_t call_generation;
    int status_code;
    uint64_t room_version;
    uint64_t sequence;
    char error_code[64];
    char error_message[128];
} ivr_command_result_envelope_t;

typedef enum {
    IVR_MEDIA_COMMAND_SESSION_OPEN = 1,
    IVR_MEDIA_COMMAND_PLAY,
    IVR_MEDIA_COMMAND_INPUT_START,
    IVR_MEDIA_COMMAND_INPUT_STOP,
    IVR_MEDIA_COMMAND_CANCEL,
    IVR_MEDIA_COMMAND_SESSION_CLOSE
} ivr_media_command_kind_t;

typedef struct {
    ivr_media_command_kind_t kind;
    char message_id[128];
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char worker_id[128];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t operation_generation;
    uint64_t deadline_timeout_ms;
    char text[4096];
    char input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t input_generation;
    char reason[128];
} ivr_media_command_t;

typedef struct {
    char message_id[128];
    char worker_id[128];
    ivr_worker_inventory_query_t query;
} ivr_worker_inventory_request_t;

typedef struct {
    char message_id[128];
    char worker_id[128];
    ivr_worker_inventory_page_t page;
    int status_code;
    char error_code[64];
    char error_message[128];
} ivr_worker_inventory_envelope_t;

ivr_status_t ivr_flowmq_gateway_decode_media_command(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_media_command_t *out);
ivr_status_t ivr_flowmq_gateway_send_media_result(
    ivr_flowmq_gateway_t *gateway, const ivr_media_command_t *command,
    int status_code, const char *error_code, const char *error_message);
ivr_status_t ivr_flowmq_gateway_send_media_event(
    ivr_flowmq_gateway_t *gateway, const char *worker_id,
    const ivr_event_view_t *event, uint64_t occurred_at_ms);

/* Worker-side inventory control plane. Decode validates exact wire version,
   bounded limit and target worker identity fields. The page encoder owns no
   borrowed output: it serializes the caller-owned page before returning. */
ivr_status_t ivr_flowmq_gateway_decode_inventory_query(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_worker_inventory_request_t *out);
ivr_status_t ivr_flowmq_gateway_encode_inventory_page(
    DataBind *codec, const ivr_worker_inventory_envelope_t *result,
    uint8_t *frame, size_t frame_capacity, size_t *out_size);
ivr_status_t ivr_flowmq_gateway_send_inventory_page(
    ivr_flowmq_gateway_t *gateway,
    const ivr_worker_inventory_envelope_t *result);

/* Pure decode of one CallDispatchCommandV1 frame (kind=command). Returns
   IVR_OK and fills *out; IVR_ESTATE for non-dispatch/malformed frames. */
ivr_status_t ivr_flowmq_gateway_decode_dispatch(DataBind *codec,
                                                const uint8_t *frame,
                                                size_t len,
                                                ivr_call_dispatch_t *out);

/* 1 when a dispatch received at eceived_at_ms is still inside its
   deadline_timeout_ms relative TTL at 
ow_ms. A zero deadline is invalid
   for V2 dispatches (decode rejects it); overflow-safe. Both timestamps use
   the same monotonic clock. */
int ivr_flowmq_gateway_dispatch_deadline_ok(
    const ivr_call_dispatch_t *dispatch, uint64_t received_at_ms,
    uint64_t now_ms);

/* Pure BIN/TIVR encode of CallDispatchResultV1. */
ivr_status_t ivr_flowmq_gateway_encode_dispatch_result(
    DataBind *codec, const ivr_call_dispatch_t *dispatch, int status_code,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len);

/* V2 dispatch/result preserve V1 IDs and add assignment/attempt fencing. */
ivr_status_t ivr_flowmq_gateway_encode_dispatch_result_v2(
    DataBind *codec, const ivr_call_dispatch_t *dispatch, int status_code,
    uint32_t active_sessions, uint32_t max_sessions,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len);

/* Send a dispatch result from the worker DEALER to RoomService. */
ivr_status_t ivr_flowmq_gateway_send_dispatch_result(
    ivr_flowmq_gateway_t *gateway, const ivr_call_dispatch_t *dispatch,
    int status_code, const char *error_code, const char *error_message);
ivr_status_t ivr_flowmq_gateway_send_dispatch_result_v2(
    ivr_flowmq_gateway_t *gateway, const ivr_call_dispatch_t *dispatch,
    int status_code, uint32_t active_sessions, uint32_t max_sessions,
    const char *error_code, const char *error_message);

ivr_status_t ivr_flowmq_gateway_decode_release(DataBind *codec,
                                               const uint8_t *frame,
                                               size_t len,
                                               ivr_call_release_t *out);
ivr_status_t ivr_flowmq_gateway_encode_release_result(
    DataBind *codec, const ivr_call_release_t *release, int status_code,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len);
ivr_status_t ivr_flowmq_gateway_send_release_result(
    ivr_flowmq_gateway_t *gateway, const ivr_call_release_t *release,
    int status_code, const char *error_code, const char *error_message);

/* Pure decode of one IvrCommandResultV1 frame (kind=result). */
ivr_status_t ivr_flowmq_gateway_decode_result(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_command_result_envelope_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_FLOWMQ_GATEWAY_H */
