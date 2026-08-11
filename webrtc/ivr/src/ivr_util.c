#include "ivr_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ivr_str_init(ivr_str_t *s) {
    s->data = NULL;
    s->size = 0;
    s->cap = 0;
}

void ivr_str_free(ivr_str_t *s) {
    free(s->data);
    s->data = NULL;
    s->size = 0;
    s->cap = 0;
}

int ivr_str_assign(ivr_str_t *s, const char *data, size_t len) {
    if (len + 1 > s->cap) {
        char *next = (char *)realloc(s->data, len + 1);
        if (!next) {
            return -1;
        }
        s->data = next;
        s->cap = len + 1;
    }
    if (len > 0 && data) {
        memcpy(s->data, data, len);
    }
    s->data[len] = '\0';
    s->size = len;
    return 0;
}

int ivr_str_append(ivr_str_t *s, const char *data, size_t len) {
    size_t need = s->size + len;
    if (need + 1 > s->cap) {
        size_t next_cap = s->cap ? s->cap : 32;
        while (next_cap < need + 1) {
            next_cap *= 2;
        }
        char *next = (char *)realloc(s->data, next_cap);
        if (!next) {
            return -1;
        }
        s->data = next;
        s->cap = next_cap;
    }
    if (len > 0 && data) {
        memcpy(s->data + s->size, data, len);
    }
    s->size = need;
    s->data[s->size] = '\0';
    return 0;
}

static void ivr_json_append_raw(ivr_json_builder_t *jb, const char *s,
                                   size_t len) {
    if (!jb || !jb->buf || jb->overflow) {
        return;
    }
    if (len > jb->cap - 1 - jb->pos) {
        jb->overflow = 1;
        jb->buf[jb->pos] = '\0';
        return;
    }
    if (len > 0) {
        memcpy(jb->buf + jb->pos, s, len);
    }
    jb->pos += len;
    jb->buf[jb->pos] = '\0';
}

void ivr_json_builder_init(ivr_json_builder_t *jb, char *buf, size_t cap) {
    if (!jb) {
        return;
    }
    jb->buf = buf;
    jb->cap = cap;
    jb->pos = 0;
    jb->overflow = 0;
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
}

void ivr_json_builder_raw(ivr_json_builder_t *jb, const char *text) {
    if (!jb || !text) {
        return;
    }
    ivr_json_append_raw(jb, text, strlen(text));
}

void ivr_json_builder_string(ivr_json_builder_t *jb, const char *data,
                             size_t len) {
    static const char hex[] = "0123456789abcdef";
    if (!jb) {
        return;
    }
    if (!data) {
        data = "";
        len = 0;
    }
    ivr_json_append_raw(jb, "\"", 1);
    for (size_t i = 0; i < len && !jb->overflow; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '"':
            ivr_json_append_raw(jb, "\\\"", 2);
            break;
        case '\\':
            ivr_json_append_raw(jb, "\\\\", 2);
            break;
        case '\b':
            ivr_json_append_raw(jb, "\\b", 2);
            break;
        case '\f':
            ivr_json_append_raw(jb, "\\f", 2);
            break;
        case '\n':
            ivr_json_append_raw(jb, "\\n", 2);
            break;
        case '\r':
            ivr_json_append_raw(jb, "\\r", 2);
            break;
        case '\t':
            ivr_json_append_raw(jb, "\\t", 2);
            break;
        default:
            if (c < 0x20) {
                char esc[7];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(c >> 4) & 0x0f];
                esc[5] = hex[c & 0x0f];
                esc[6] = '\0';
                ivr_json_append_raw(jb, esc, 6);
            } else {
                ivr_json_append_raw(jb, (const char *)&data[i], 1);
            }
            break;
        }
    }
    if (!jb->overflow) {
        ivr_json_append_raw(jb, "\"", 1);
    }
}

void ivr_json_builder_string_cstr(ivr_json_builder_t *jb, const char *s) {
    ivr_json_builder_string(jb, s, s ? strlen(s) : 0);
}

int ivr_json_builder_ok(const ivr_json_builder_t *jb) {
    return jb && jb->buf && !jb->overflow && jb->pos < jb->cap;
}

int ivr_str_set_u64(ivr_str_t *s, uint64_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    return ivr_str_assign(s, buf, (size_t)n);
}

void ivr_event_init(ivr_event_t *e) {
    memset(e, 0, sizeof(*e));
    ivr_str_init(&e->event_id);
    ivr_str_init(&e->event_type);
    ivr_str_init(&e->room_id);
    ivr_str_init(&e->call_id);
    ivr_str_init(&e->causation_id);
    ivr_str_init(&e->payload_json);
    ivr_str_init(&e->input_id);
    ivr_str_init(&e->input_value);
}

void ivr_event_free(ivr_event_t *e) {
    ivr_str_free(&e->event_id);
    ivr_str_free(&e->event_type);
    ivr_str_free(&e->room_id);
    ivr_str_free(&e->call_id);
    ivr_str_free(&e->causation_id);
    ivr_str_free(&e->payload_json);
    ivr_str_free(&e->input_id);
    ivr_str_free(&e->input_value);
}

int ivr_event_copy_from_view(ivr_event_t *e, const ivr_event_view_t *view) {
    if (!e || !view) {
        return -1;
    }
    if (ivr_str_assign(&e->event_id, view->event_id.data, view->event_id.size) < 0 ||
        ivr_str_assign(&e->event_type, view->event_type.data, view->event_type.size) < 0 ||
        ivr_str_assign(&e->room_id, view->call.room_id.data, view->call.room_id.size) < 0 ||
        ivr_str_assign(&e->call_id, view->call.call_id.data, view->call.call_id.size) < 0 ||
        ivr_str_assign(&e->payload_json, view->payload_json.data, view->payload_json.size) < 0 ||
        ivr_str_assign(&e->input_id, view->input_id.data, view->input_id.size) < 0 ||
        ivr_str_assign(&e->input_value, view->input_value.data, view->input_value.size) < 0) {
        return -1;
    }
    e->call_generation = view->call.call_generation;
    e->room_version = view->call.expected_room_version;
    e->sequence = view->sequence;
    return 0;
}

int ivr_event_classify(ivr_event_t *e) {
    if (!e || e->event_type.size == 0) {
        return -1;
    }
    if (strcmp(e->event_type.data, "dtmf.final") == 0 ||
        strcmp(e->event_type.data, "asr.final") == 0) {
        e->kind = IVR_EVENT_KIND_INPUT;
    } else if (strcmp(e->event_type.data, "input.timeout") == 0) {
        e->kind = IVR_EVENT_KIND_TIMEOUT;
    } else if (strcmp(e->event_type.data, "room.snapshot.loaded") == 0) {
        e->kind = IVR_EVENT_KIND_SNAPSHOT;
    } else if (strcmp(e->event_type.data, "command.result") == 0) {
        e->kind = IVR_EVENT_KIND_COMMAND_RESULT;
    } else if (e->sequence == 0 &&
               (strncmp(e->event_type.data, "rtc.", 4) == 0 ||
                strncmp(e->event_type.data, "media.", 6) == 0 ||
                strncmp(e->event_type.data, "provider.", 9) == 0)) {
        e->kind = IVR_EVENT_KIND_MEDIA;
    } else {
        e->kind = IVR_EVENT_KIND_STATE;
    }
    return 0;
}

void ivr_command_init(ivr_command_t *c) {
    memset(c, 0, sizeof(*c));
    ivr_str_init(&c->message_id);
    ivr_str_init(&c->worker_id);
    ivr_str_init(&c->room_id);
    ivr_str_init(&c->call_id);
    ivr_str_init(&c->command_type);
    ivr_str_init(&c->args_json);
}

void ivr_command_free(ivr_command_t *c) {
    ivr_str_free(&c->message_id);
    ivr_str_free(&c->worker_id);
    ivr_str_free(&c->room_id);
    ivr_str_free(&c->call_id);
    ivr_str_free(&c->command_type);
    ivr_str_free(&c->args_json);
}

int ivr_command_set_from_view(ivr_command_t *c, const ivr_command_view_t *view) {
    if (!c || !view) {
        return -1;
    }
    if (ivr_str_assign(&c->message_id, view->message_id.data, view->message_id.size) < 0 ||
        ivr_str_assign(&c->command_type, view->command_type.data, view->command_type.size) < 0 ||
        ivr_str_assign(&c->room_id, view->call.room_id.data, view->call.room_id.size) < 0 ||
        ivr_str_assign(&c->call_id, view->call.call_id.data, view->call.call_id.size) < 0 ||
        ivr_str_assign(&c->args_json, view->args_json.data, view->args_json.size) < 0) {
        return -1;
    }
    c->call_generation = view->call.call_generation;
    c->expected_room_version = view->call.expected_room_version;
    return 0;
}
