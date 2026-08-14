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
/* Owned event                                                         */
/* ------------------------------------------------------------------ */

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

/* Typed provider media commands include the opening operation generation.
   Recording it at the same transition that publishes the ACTIVE slot lets a
   restarted RoomService compare worker inventory with the next Iris command.
   This entry point is internal so the installed embedded ABI remains stable. */
ivr_status_t ivr_worker_open_media_operation(
    ivr_worker_t *worker, const ivr_media_operation_t *operation);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_INTERNAL_H */
