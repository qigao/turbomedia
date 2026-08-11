#include "ivr_flowmq_subscriber.h"
#include "ivr_frame.h"
#include "turbomedia_ivr_v1.h"
#include "turbo_flow_fmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_SUB_MAX_TOPIC 1024u

struct ivr_flowmq_subscriber_s {
    turbo_flow_fmq_app_t *app;
    DataBind *codec;
    char topic[IVR_SUB_MAX_TOPIC]; /* SUB prefix; "" = all topics */
    void (*on_event)(void *ctx, const ivr_event_view_t *event);
    void *event_ctx;
    void (*on_connection)(void *ctx, int connected);
    void *connection_ctx;
    uint64_t events_received;
    uint64_t decode_errors;
};

static void subscriber_on_connection_event(
    void *ctx, const turbo_flow_fmq_event_t *event) {
    ivr_flowmq_subscriber_t *subscriber =
        (ivr_flowmq_subscriber_t *)ctx;
    int connected;
    if (!subscriber || !subscriber->on_connection || !event) {
        return;
    }
    switch (event->kind) {
    case TURBO_FLOW_FMQ_EVENT_PEER_CONNECTED:
    case TURBO_FLOW_FMQ_EVENT_RECONNECT_SUCCEEDED:
        connected = 1;
        break;
    case TURBO_FLOW_FMQ_EVENT_PEER_DISCONNECTED:
    case TURBO_FLOW_FMQ_EVENT_RECONNECT_SCHEDULED:
    case TURBO_FLOW_FMQ_EVENT_RECONNECT_FAILED:
    case TURBO_FLOW_FMQ_EVENT_HEARTBEAT_TIMEOUT:
    case TURBO_FLOW_FMQ_EVENT_AUTHENTICATION_FAILED:
        connected = 0;
        break;
    default:
        return;
    }
    subscriber->on_connection(subscriber->connection_ctx, connected);
}

/* schema_type_id -> canonical worker-facing event type. Only kinds the worker
   can route to a per-call session are mapped; unknown ids are rejected. */
typedef struct {
    uint16_t type_id;
    const char *event_type;
} ivr_event_type_map_t;

static const ivr_event_type_map_t kEventTypeMap[] = {
    {IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1, "room.participant.joined"},
    {IVR_TYPE_DTMF_FINAL_EVENT_V1, "dtmf.final"},
    {IVR_TYPE_ASR_FINAL_EVENT_V1, "asr.final"},
    {IVR_TYPE_INPUT_TIMEOUT_EVENT_V1, "input.timeout"},
    {IVR_TYPE_PLAYBACK_FINISHED_EVENT_V1, "playback.finished"},
    {IVR_TYPE_IVR_WORKER_LOST_EVENT_V1, "call.terminal"},
    {IVR_TYPE_ROOM_SNAPSHOT_V1, "room.snapshot.loaded"},
};
#define IVR_EVENT_TYPE_MAP_COUNT (sizeof(kEventTypeMap) / sizeof(kEventTypeMap[0]))

static const char *ivr_event_type_find(uint16_t type_id) {
    for (size_t i = 0; i < IVR_EVENT_TYPE_MAP_COUNT; i++) {
        if (kEventTypeMap[i].type_id == type_id) {
            return kEventTypeMap[i].event_type;
        }
    }
    return NULL;
}

/* One decoded event. The view's string fields borrow from the DataBind object
   value tree and from the serialized JSON; both are owned here and released by
   ivr_subscriber_decoded_free(). */
struct ivr_subscriber_decoded_s {
    ivr_event_view_t view;
    DataBindObject *obj;
    char *json;
};

const ivr_event_view_t *ivr_subscriber_decoded_view(
    const ivr_subscriber_decoded_t *decoded) {
    return decoded ? &decoded->view : NULL;
}

void ivr_subscriber_decoded_free(ivr_subscriber_decoded_t *decoded) {
    if (!decoded) {
        return;
    }
    if (decoded->json) {
        data_bind_serialized_free(decoded->json);
        decoded->json = NULL;
    }
    if (decoded->obj) {
        data_bind_object_free(decoded->obj);
        decoded->obj = NULL;
    }
    free(decoded);
}

static const char *view_string(const DataBindValue *root, const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    const char *s = v ? data_bind_value_as_string(v) : NULL;
    return s ? s : "";
}

static uint64_t view_u64(const DataBindValue *root, const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    return v ? data_bind_value_as_uint64(v) : 0;
}

ivr_status_t ivr_flowmq_subscriber_decode(DataBind *codec, const uint8_t *frame,
                                          size_t len,
                                          ivr_subscriber_decoded_t **out_decoded) {
    if (!codec || !frame || !out_decoded) {
        return IVR_EINVAL;
    }
    *out_decoded = NULL;
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        return IVR_ESTATE; /* malformed/unsupported frame envelope */
    }
    if ((info.kind != IVR_KIND_EVENT && info.kind != IVR_KIND_SNAPSHOT) ||
        info.format != IVR_FMT_BIN) {
        return IVR_ESTATE; /* only BIN event/snapshot frames are domain data */
    }
    const char *event_type = ivr_event_type_find(info.schema_type_id);
    const char *type_name = ivr_frame_type_name(info.schema_type_id);
    if (!event_type || !type_name) {
        return IVR_ESTATE; /* event type not routable by a worker */
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(codec, type_name,
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj, &err) !=
        DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    char *json = NULL;
    size_t json_len = 0;
    if (data_bind_object_serialize_json(codec, obj, &json, &json_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    ivr_subscriber_decoded_t *decoded =
        (ivr_subscriber_decoded_t *)calloc(1, sizeof(*decoded));
    if (!decoded) {
        data_bind_serialized_free(json);
        data_bind_object_free(obj);
        return IVR_ENOSPC;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    ivr_event_view_t *v = &decoded->view;
    v->event_id.data = view_string(root, "event_id");
    v->event_id.size = strlen(v->event_id.data);
    v->event_type.data = event_type;
    v->event_type.size = strlen(event_type);
    v->call.room_id.data = view_string(root, "room_id");
    v->call.room_id.size = strlen(v->call.room_id.data);
    v->call.call_id.data = view_string(root, "call_id");
    v->call.call_id.size = strlen(v->call.call_id.data);
    v->call.call_generation = view_u64(root, "call_generation");
    /* The session maps call.expected_room_version -> per-call room_version. */
    v->call.expected_room_version = view_u64(root, "room_version");
    /* snapshot messages carry the authoritative continuity marker as
       last_sequence; state/input events use sequence */
    v->sequence = (info.schema_type_id == IVR_TYPE_ROOM_SNAPSHOT_V1)
                      ? view_u64(root, "last_sequence")
                      : view_u64(root, "sequence");
    v->input_id.data = view_string(root, "input_id");
    v->input_id.size = strlen(v->input_id.data);
    v->input_value.data = view_string(root, "input_value");
    v->input_value.size = strlen(v->input_value.data);
    v->payload_json.data = json;
    v->payload_json.size = json_len;
    decoded->obj = obj;
    decoded->json = json;
    *out_decoded = decoded;
    return IVR_OK;
}

/* FlowMQ SUB ingress: decode in place and forward a borrowed view. The decoded
   object stays alive until the callback returns (never cross a queue with it). */
static int subscriber_on_message(turbo_flow_fmq_app_t *app,
                                 turbo_flow_msg_t *message, void *ctx) {
    ivr_flowmq_subscriber_t *s = (ivr_flowmq_subscriber_t *)ctx;
    (void)app;
    if (!s || !message || message->payload.len == 0) {
        return TURBO_OK;
    }
    ivr_subscriber_decoded_t *decoded = NULL;
    if (ivr_flowmq_subscriber_decode(s->codec,
                                     (const uint8_t *)message->payload.data,
                                     message->payload.len, &decoded) != IVR_OK) {
        s->decode_errors++;
        return TURBO_OK;
    }
    s->events_received++;
    if (s->on_event) {
        s->on_event(s->event_ctx, ivr_subscriber_decoded_view(decoded));
    }
    ivr_subscriber_decoded_free(decoded);
    return TURBO_OK;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

ivr_status_t ivr_flowmq_subscriber_create(
    const ivr_flowmq_subscriber_config_t *config,
    ivr_flowmq_subscriber_t **out_subscriber) {
    if (!config || !config->host || config->port <= 0 || !out_subscriber ||
        (config->security &&
         (!config->identity || config->identity[0] == '\0'))) {
        return IVR_EINVAL;
    }
    ivr_flowmq_subscriber_t *s =
        (ivr_flowmq_subscriber_t *)calloc(1, sizeof(*s));
    if (!s) {
        return IVR_ENOSPC;
    }
    if (config->topic) {
        if (strlen(config->topic) >= sizeof(s->topic)) {
            free(s);
            return IVR_EINVAL;
        }
        snprintf(s->topic, sizeof(s->topic), "%s", config->topic);
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    if (TurboMediaIvrV1_codec_create(&s->codec, &err) != DATA_BIND_OK) {
        free(s);
        return IVR_ENOSPC;
    }
    s->on_event = config->on_event;
    s->event_ctx = config->event_ctx;
    s->on_connection = config->on_connection;
    s->connection_ctx = config->connection_ctx;

    turbo_flow_fmq_config_t ep = TURBO_FLOW_FMQ_CONFIG_INIT;
    ep.pattern = TURBO_FLOW_FMQ_SUB;
    ep.mode = TURBO_FLOW_FMQ_CONNECT;
    ep.transport = config->transport ? config->transport : TURBO_FLOW_FMQ_TCP;
    ep.host = config->host;
    ep.port = config->port;
    ep.topic = s->topic; /* "" subscribes to all topics */
    ep.identity = config->identity;
    ep.max_frame_size = TURBO_FLOW_FMQ_DEFAULT_MAX_FRAME_SIZE;
    ep.timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
    ep.reconnect_initial_ms = 1000;
    ep.reconnect_max_ms = 30000;
    ep.tls = config->tls;
    ep.path = config->path ? config->path : "/";
    ep.event_callback = subscriber_on_connection_event;
    ep.event_ctx = s;

    turbo_flow_fmq_app_options_t opt = TURBO_FLOW_FMQ_APP_OPTIONS_INIT;
    opt.on_message = subscriber_on_message;
    opt.message_ctx = s;
    int app_rc = config->security
                     ? turbo_flow_fmq_app_create_secure(&ep, &opt,
                                                        config->security,
                                                        &s->app)
                     : turbo_flow_fmq_app_create(&ep, &opt, &s->app);
    if (app_rc != TURBO_OK) {
        data_bind_free(s->codec);
        s->codec = NULL;
        free(s);
        return IVR_ENOSPC;
    }
    *out_subscriber = s;
    return IVR_OK;
}

ivr_status_t ivr_flowmq_subscriber_start(ivr_flowmq_subscriber_t *subscriber) {
    if (!subscriber || !subscriber->app) {
        return IVR_EINVAL;
    }
    return turbo_flow_fmq_app_start(subscriber->app) == TURBO_OK ? IVR_OK
                                                                 : IVR_ESTATE;
}

void ivr_flowmq_subscriber_destroy(ivr_flowmq_subscriber_t *subscriber) {
    if (!subscriber) {
        return;
    }
    if (subscriber->app) {
        turbo_flow_fmq_app_destroy(subscriber->app);
        subscriber->app = NULL;
    }
    if (subscriber->codec) {
        data_bind_free(subscriber->codec);
        subscriber->codec = NULL;
    }
    free(subscriber);
}
