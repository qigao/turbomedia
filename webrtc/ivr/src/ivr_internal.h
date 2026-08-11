#ifndef TURBO_MEDIA_IVR_INTERNAL_H
#define TURBO_MEDIA_IVR_INTERNAL_H

/**
 * @file ivr_internal.h
 * @brief Internal shared types for the IVR worker (not installed).
 *
 * Everything here is owned by the IVR core and must not leak into the public
 * ABI (`ivr_worker.h`). Views from the public ABI are copied into these
 * owning types at the session boundary.
 */

#include "ivr/ivr_worker.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Owned byte/string buffer                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;   /* NUL-terminated for convenience; size() may be < cap */
    size_t size;
    size_t cap;
} ivr_str_t;

void ivr_str_init(ivr_str_t *s);
void ivr_str_free(ivr_str_t *s);
/* Copy `len` bytes; on failure the buffer is left untouched. */
int ivr_str_assign(ivr_str_t *s, const char *data, size_t len);
/* Append; on failure the buffer is left untouched. */
int ivr_str_append(ivr_str_t *s, const char *data, size_t len);
int ivr_str_set_u64(ivr_str_t *s, uint64_t value);

/* ------------------------------------------------------------------ */
/* Owned command / event                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    ivr_str_t message_id;
    ivr_str_t worker_id;
    ivr_str_t room_id;
    ivr_str_t call_id;
    uint64_t call_generation;
    uint64_t expected_room_version;
    ivr_str_t command_type; /* e.g. "rtc.join", "conference.join" */
    ivr_str_t args_json;    /* canonical JSON object or empty */
} ivr_command_t;

typedef enum {
    IVR_EVENT_KIND_STATE = 1,   /* room/rtc domain events */
    IVR_EVENT_KIND_INPUT = 2,   /* dtmf.final / asr.final */
    IVR_EVENT_KIND_TIMEOUT = 3, /* input window timeout */
    IVR_EVENT_KIND_SNAPSHOT = 4, /* room.snapshot.loaded */
    /* Point-to-point command acknowledgement. It does not advance the
       authoritative per-call domain sequence. */
    IVR_EVENT_KIND_COMMAND_RESULT = 5,
    /* Derived media facts with sequence=0; they are observable by the
       workflow but never advance the authoritative RoomService sequence. */
    IVR_EVENT_KIND_MEDIA = 6
} ivr_event_kind_t;

typedef struct {
    ivr_str_t event_id;
    ivr_str_t event_type; /* "room.transfer.completed", "dtmf.final", ... */
    ivr_str_t room_id;
    ivr_str_t call_id;
    uint64_t call_generation;
    uint64_t room_version;
    uint64_t sequence; /* per-call monotonic; 0 for input events */
    uint64_t occurred_at_ms;
    ivr_str_t causation_id; /* may be empty */
    ivr_str_t payload_json; /* canonical JSON object */
    ivr_event_kind_t kind;
    /* input window bookkeeping (IVR_EVENT_KIND_INPUT only) */
    ivr_str_t input_id; /* may be empty */
    ivr_str_t input_value;
} ivr_event_t;

void ivr_event_init(ivr_event_t *e);
void ivr_event_free(ivr_event_t *e);
int ivr_event_copy_from_view(ivr_event_t *e, const ivr_event_view_t *view);
/* Normalize a raw event JSON (sequence/generation checks happen in session). */
int ivr_event_classify(ivr_event_t *e);

void ivr_command_init(ivr_command_t *c);
void ivr_command_free(ivr_command_t *c);
int ivr_command_set_from_view(ivr_command_t *c, const ivr_command_view_t *view);

/* ------------------------------------------------------------------ */
/* Engine interface (adapter boundary)                                 */
/* ------------------------------------------------------------------ */

/* Session hooks the engine uses for blocking VoiceXML input and terminal
   checks. Implemented by the session; used by the TurboXML adapter. */
typedef struct ivr_session_hooks {
    void *context;
    int (*input_wait)(void *ctx, uint64_t timeout_ms, ivr_str_t *out);
    int (*begin_input_window)(void *ctx, const ivr_bytes_view_t *id);
    void (*input_cancelled)(void *ctx, int succeeded);
    void (*input_command_completed)(void *ctx, int submitted);
    int (*is_terminal)(void *ctx);
} ivr_session_hooks_t;
typedef struct ivr_xml_engine ivr_xml_engine_t;

/* Intents emitted by the engine; the sink forwards them to gateway/media. */
typedef struct {
    void *context;
    /* Submit a command intent to the gateway. The sink copies. */
    ivr_status_t (*command)(void *context, const ivr_command_t *command);
    /* Play TTS text on the bot send track. The sink copies. */
    ivr_status_t (*play_tts)(void *context, const ivr_call_ref_t *call,
                             const ivr_bytes_view_t *text);
    /* Cancel the current prompt/input (barge-in, noinput, terminal). */
    ivr_status_t (*cancel_input)(void *context, const ivr_call_ref_t *call);
    /* Tear down the bot peer (RTC closing). */
    ivr_status_t (*stop_bot)(void *context, const ivr_call_ref_t *call);
} ivr_engine_sink_t;

typedef struct {
    /* Create the engine for one session from a loaded content package.
       `sink` is borrowed by the engine for its lifetime. */
    ivr_xml_engine_t *(*create)(const void *content, const ivr_engine_sink_t *sink,
                               const ivr_session_hooks_t *hooks,
                               const ivr_call_ref_t *call);
    /* Submit a normalized owned event (control thread only). */
    ivr_status_t (*submit_event)(ivr_xml_engine_t *engine, const ivr_event_t *event);
    /* Drive the engine; may block in VoiceXML collect_input. */
    ivr_status_t (*step)(ivr_xml_engine_t *engine);
    /* Terminal: cancel blocking input immediately. */
    void (*request_terminal)(ivr_xml_engine_t *engine);
    void (*destroy)(ivr_xml_engine_t *engine);
} ivr_xml_engine_ops_t;

/* ------------------------------------------------------------------ */
/* Session internal interface (worker <-> session)                     */
/* ------------------------------------------------------------------ */

typedef struct ivr_session_ops {
    void (*request_terminal)(ivr_session_t *session);
    void (*destroy)(ivr_session_t *session);
} ivr_session_ops_t;

/* Per-session mutable state shared with the worker for drain bookkeeping. */
typedef struct {
    uint64_t call_generation;
    uint64_t room_version;   /* last observed authoritative version */
    uint64_t last_sequence;  /* last contiguous sequence delivered */
    uint64_t dropped_stale_inputs; /* stale/late final counter */
} ivr_session_stats_t;

/* Input wait primitive used by the adapter blocking collect_input. */
typedef struct ivr_input_waiter ivr_input_waiter_t;

/* ------------------------------------------------------------------ */
/* Bounded JSON string builder for hand-rolled encoders                */
/* ------------------------------------------------------------------ */

/* The hand-rolled schema encoders build typed JSON that DataBind parses
   before BIN serialization. All string fields must be JSON-escaped so legal
   identifiers containing quotes/backslashes/control characters cannot produce
   malformed or semantically different messages. The builder keeps `buf`
   NUL-terminated and fails closed (overflow flag) instead of truncating into
   invalid JSON. */
typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    int overflow;
} ivr_json_builder_t;

void ivr_json_builder_init(ivr_json_builder_t *jb, char *buf, size_t cap);
/* Append raw, already-valid JSON text (numbers, punctuation, literals). */
void ivr_json_builder_raw(ivr_json_builder_t *jb, const char *text);
/* Append one JSON string literal: '"' + escaped bytes + '"'. A NULL `data` is
   treated as an empty string. */
void ivr_json_builder_string(ivr_json_builder_t *jb, const char *data,
                             size_t len);
/* Append the JSON string literal of a NUL-terminated C string (NULL = ""). */
void ivr_json_builder_string_cstr(ivr_json_builder_t *jb, const char *s);
/* 1 when no overflow occurred and the buffer holds a complete string. */
int ivr_json_builder_ok(const ivr_json_builder_t *jb);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_INTERNAL_H */
