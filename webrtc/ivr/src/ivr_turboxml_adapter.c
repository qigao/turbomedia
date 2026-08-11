#include "ivr_turboxml_adapter.h"
#include "ivr_content.h"
#include "ivr_session.h"
#include "turboxml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* send capture                                                         */
/* ------------------------------------------------------------------ */

/* <send target="ivr.command"> is exposed on the execution point with its
   full content (event/target/type/param.*) since turboxml 2026-08-07; the
   adapter turns event+params into the ivr.command intent. */

typedef struct {
    const ivr_engine_sink_t *sink;
    const ivr_content_package_t *content;
    ivr_session_hooks_t hooks;
    ivr_call_ref_t call;
    turboxml_ccxml_t *ccxml;
    int ccxml_ready;
    turboxml_scxml_t *scxml;
    int scxml_ready;
    int scxml_pending;      /* an event was injected; the engine needs stepping */
    int scxml_event_seen;   /* the injected event started processing */
    int scxml_stable;       /* stable reached after the event was processed */
    int terminated;
    uint64_t input_counter;
    uint64_t unauthorized_commands;
    uint64_t unhandled_events;
    ivr_str_t pending_input_value; /* borrowed by the engine until next call */
} ivr_turboxml_adapter_t;

/* ------------------------------------------------------------------ */
/* command / media emission                                            */
/* ------------------------------------------------------------------ */

static ivr_status_t adapter_emit_command(ivr_turboxml_adapter_t *a,
                                         const char *type,
                                         const char *args_json) {
    ivr_status_t status;
    if (a->terminated) {
        return IVR_ECLOSED;
    }
    if (!ivr_content_command_allowed(a->content, type)) {
        a->unauthorized_commands++;
        return IVR_EAUTH;
    }
    ivr_command_t cmd;
    ivr_command_init(&cmd);
    if (ivr_str_assign(&cmd.command_type, type, strlen(type)) < 0 ||
        ivr_str_assign(&cmd.args_json, args_json ? args_json : "{}",
                       args_json ? strlen(args_json) : 2) < 0) {
        ivr_command_free(&cmd);
        return IVR_ENOSPC;
    }
    status = a->sink->command(a->sink->context, &cmd);
    ivr_command_free(&cmd);
    return status;
}

/* ------------------------------------------------------------------ */
/* CCXML platform callbacks                                            */
/* ------------------------------------------------------------------ */

static void ccxml_on_accept(void *user_data) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)user_data;
    (void)adapter_emit_command(a, "accept", "{}");
}

static void ccxml_on_disconnect(void *user_data) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)user_data;
    (void)adapter_emit_command(a, "disconnect", "{}");
}

static void ccxml_on_start_dialog(void *user_data, const char *src) {
    /* The built-in dialog bridge runs the registered VXML automatically;
       nothing to do here beyond validating the source is registered. */
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)user_data;
    if (!ivr_content_find_dialog(a->content, src)) {
        a->unhandled_events++;
    }
}

static void ccxml_on_play_prompt(void *user_data, const char *text) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)user_data;
    ivr_bytes_view_t view;
    view.data = text;
    view.size = text ? strlen(text) : 0;
    (void)a->sink->play_tts(a->sink->context, &a->call, &view);
}

static const char *ccxml_on_collect_input(void *user_data, const char *field_name) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)user_data;
    (void)field_name;
    if (a->terminated || a->hooks.is_terminal(a->hooks.context)) {
        return NULL;
    }
    /* Open a fresh input window with a monotonic id. */
    char window_id[32];
    snprintf(window_id, sizeof(window_id), "w%llu",
             (unsigned long long)++a->input_counter);
    ivr_bytes_view_t wid;
    wid.data = window_id;
    wid.size = strlen(window_id);
    if (a->hooks.begin_input_window(a->hooks.context, &wid) != IVR_OK) {
        return NULL;
    }
    uint64_t timeout = a->content->default_timeout_ms;
    ivr_str_t value;
    ivr_str_init(&value);
    int rc = a->hooks.input_wait(a->hooks.context, timeout, &value);
    if (rc == 1) {
        ivr_status_t cancel_status;
        ivr_status_t command_status = IVR_EAUTH;
        /* barge-in: cancel the current prompt before returning the input */
        cancel_status = a->sink->cancel_input(a->sink->context, &a->call);
        if (a->hooks.input_cancelled) {
            a->hooks.input_cancelled(a->hooks.context,
                                     cancel_status == IVR_OK);
        }
        /* map recognized input to the business command via content manifest */
        const char *cmd = ivr_content_command_for_input(a->content, value.data);
        if (cmd) {
            command_status = adapter_emit_command(a, cmd, "{}");
        } else {
            a->unauthorized_commands++;
        }
        if (a->hooks.input_command_completed) {
            a->hooks.input_command_completed(a->hooks.context,
                                              command_status == IVR_OK);
        }
        /* the engine borrows the returned value only for this call; keep it
           owned in the adapter and reuse the slot on the next input */
        ivr_str_free(&a->pending_input_value);
        a->pending_input_value = value;
        return a->pending_input_value.data;
    }
    if (rc == 0) {
        /* noinput: close the ASR window */
        (void)a->sink->cancel_input(a->sink->context, &a->call);
        ivr_str_free(&value);
        return NULL;
    }
    /* terminal */
    ivr_str_free(&value);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SCXML execution plugin: capture <send target="ivr.command">         */
/* ------------------------------------------------------------------ */

/* Extract the quoted value of a JSON object key from the pretty-printed exec
   point the wrapper emits. No allocation and no parser re-entrancy: this runs
   inside the SCXML engine's step, which must never block or allocate on paths
   the engine shares. */
static int exec_point_field(const char *json, const char *key, char *out,
                            size_t out_size) {
    char pat[64];
    size_t klen = strlen(key);
    if (klen + 2 >= sizeof(pat)) {
        return -1;
    }
    pat[0] = '"';
    memcpy(pat + 1, key, klen);
    pat[klen + 1] = '"';
    pat[klen + 2] = '\0';
    const char *p = strstr(json, pat);
    if (!p) {
        return -1;
    }
    p += klen + 2;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) {
        out[n++] = *p++;
    }
    if (*p != '"') return -1;
    out[n] = '\0';
    return 0;
}

/* Build a JSON args object from the "param.<name>" keys of a <send>
   execution point. Simple quoted string literals ('x' or "x") are unquoted;
   everything else is kept as raw expr text. Returns 0 with a valid object or
   -1 when no params were found. */
static int exec_point_build_args(const char *json, char *out, size_t out_size) {
    static const char prefix[] = "\"param.";
    const char *p = json;
    size_t used = 0;
    int first = 1;
    while (p && (p = strstr(p, prefix)) != NULL) {
        const char *name = p + sizeof(prefix) - 1;
        const char *end = strchr(name, '"');
        if (!end || end == name) {
            break;
        }
        const char *v = end + 1;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        if (*v != ':') break;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        if (*v != '"') break;
        v++;
        const char *vstart = v;
        while (*v && *v != '"') v++;
        if (*v != '"') break;
        size_t name_len = (size_t)(end - name);
        size_t val_len = (size_t)(v - vstart);
        /* normalize simple quoted string literals */
        const char *val = vstart;
        size_t val_used = val_len;
        if (val_len >= 2 &&
            ((vstart[0] == '\'' && vstart[val_len - 1] == '\'') ||
             (vstart[0] == '"' && vstart[val_len - 1] == '"'))) {
            val = vstart + 1;
            val_used = val_len - 2;
        }
        /* conservative per-entry budget: brace/comma, quotes, colon,
           value, closing brace and NUL */
        if (used + name_len + val_used + 16 >= out_size) {
            return -1;
        }
        if (first) {
            out[used++] = '{';
            first = 0;
        } else {
            out[used++] = ',';
            out[used++] = ' ';
        }
        out[used++] = '"';
        memcpy(out + used, name, name_len);
        used += name_len;
        out[used++] = '"';
        out[used++] = ':';
        out[used++] = '"';
        /* escape quotes/backslashes minimally */
        for (size_t i = 0; i < val_used; i++) {
            char c = val[i];
            if (c == '"' || c == '\\') {
                out[used++] = '\\';
            }
            out[used++] = c;
        }
        out[used++] = '"';
        p = v + 1;
    }
    if (first) {
        return -1;
    }
    out[used++] = '}';
    out[used] = '\0';
    return 0;
}

static void scxml_on_execution_point(void *user_data, const char *point_json) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)user_data;
    char name[24], phase[16], subject[24], target[64];
    if (!a || a->terminated || !point_json) {
        return;
    }
    if (exec_point_field(point_json, "subject", subject, sizeof(subject)) == 0 &&
        strcmp(subject, "stable") == 0) {
        /* A stable configuration may be reported before the queued event is
           processed (the microstep preamble) and again after processing. Only
           the post-processing stable means the next step would block waiting
           for an external event, so we flag it only after event processing. */
        if (a->scxml_event_seen) {
            a->scxml_stable = 1;
        }
        return;
    }
    /* event-start detection (independent of the send capture below) */
    if (exec_point_field(point_json, "phase", phase, sizeof(phase)) == 0 &&
        strcmp(phase, "before") == 0 &&
        exec_point_field(point_json, "subject", subject, sizeof(subject)) == 0 &&
        strcmp(subject, "event") == 0) {
        a->scxml_event_seen = 1;
        return;
    }
    /* <send target="ivr.command"> capture: command name = send event,
       args = <param name="..." expr="..."/> children. */
    if (exec_point_field(point_json, "name", name, sizeof(name)) != 0 ||
        strcmp(name, "send") != 0 ||
        exec_point_field(point_json, "phase", phase, sizeof(phase)) != 0 ||
        strcmp(phase, "before") != 0 ||
        exec_point_field(point_json, "target", target, sizeof(target)) != 0 ||
        strcmp(target, "ivr.command") != 0) {
        return;
    }
    if (exec_point_field(point_json, "event", phase, sizeof(phase)) != 0 ||
        phase[0] == '\0') {
        return;
    }
    char args[512];
    if (exec_point_build_args(point_json, args, sizeof(args)) != 0) {
        memcpy(args, "{}", 3);
    }
    (void)adapter_emit_command(a, phase, args);
}

/* ------------------------------------------------------------------ */
/* SCXML event injection                                               */
/* ------------------------------------------------------------------ */

static void scxml_fire(ivr_turboxml_adapter_t *a, const char *event_name) {
    if (!a->scxml_ready) {
        a->unhandled_events++;
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"name\":\"%s\"}", event_name);
    turboxml_scxml_receive_event_json(a->scxml, buf);
    a->scxml_pending = 1;
}

/* ------------------------------------------------------------------ */
/* engine ops                                                          */
/* ------------------------------------------------------------------ */

static ivr_xml_engine_t *turboxml_create(const void *content,
                                         const ivr_engine_sink_t *sink,
                                         const ivr_session_hooks_t *hooks,
                                         const ivr_call_ref_t *call) {
    const ivr_content_package_t *pkg = (const ivr_content_package_t *)content;
    if (!pkg || !sink || !hooks) {
        return NULL;
    }
    ivr_turboxml_adapter_t *a =
        (ivr_turboxml_adapter_t *)calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->sink = sink;
    a->content = pkg;
    a->hooks = *hooks;
    if (call) {
        a->call = *call;
    }
    if (pkg->ccxml.size > 0) {
        a->ccxml = turboxml_ccxml_from_xml(pkg->ccxml.data);
        if (!a->ccxml) {
            free(a);
            return NULL;
        }
        turboxml_ccxml_platform_t plat;
        memset(&plat, 0, sizeof(plat));
        plat.user_data = a;
        plat.accept = ccxml_on_accept;
        plat.disconnect = ccxml_on_disconnect;
        plat.start_dialog = ccxml_on_start_dialog;
        plat.play_prompt = ccxml_on_play_prompt;
        plat.collect_input = ccxml_on_collect_input;
        turboxml_ccxml_set_platform(a->ccxml, plat);
        for (size_t i = 0; i < pkg->dialog_count; i++) {
            turboxml_ccxml_register_vxml_document(
                a->ccxml, pkg->dialogs[i].src.data, pkg->dialogs[i].xml.data);
        }
        a->ccxml_ready = 1;
    }
    if (pkg->rtc_scxml.size > 0) {
        a->scxml = turboxml_scxml_from_xml(pkg->rtc_scxml.data);
        if (!a->scxml) {
            if (a->ccxml) {
                turboxml_ccxml_destroy(a->ccxml);
            }
            free(a);
            return NULL;
        }
        turboxml_execution_plugin_t sp;
        memset(&sp, 0, sizeof(sp));
        sp.user_data = a;
        sp.on_execution_point = scxml_on_execution_point;
        if (turboxml_scxml_add_plugin(a->scxml, sp) == 1) {
            /* InterpreterImpl::init runs on the first step(); receive() before
               that crashes on a NULL event queue, so initialize eagerly. */
            turboxml_scxml_step(a->scxml);
            a->scxml_ready = 1;
        }
    }
    return (ivr_xml_engine_t *)a;
}

static ivr_status_t turboxml_submit_event(ivr_xml_engine_t *engine,
                                          const ivr_event_t *event) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)engine;
    if (!a || !event) {
        return IVR_EINVAL;
    }
    if (a->terminated) {
        return IVR_ECLOSED;
    }
    if (!ivr_content_event_allowed(a->content, event->event_type.data)) {
        a->unauthorized_commands++;
        return IVR_ESTATE;
    }
    const char *scxml_name = NULL;
    if (strcmp(event->event_type.data, "connection.alerting") == 0) {
        if (a->ccxml_ready) {
            /* CCXML events use {"name": ...} in this TurboXML build. */
            turboxml_ccxml_receive_event_json(
                a->ccxml, "{\"name\":\"connection.alerting\"}");
        }
    } else if (strcmp(event->event_type.data, "room.call.terminal") == 0) {
        if (a->ccxml_ready) {
            turboxml_ccxml_receive_event_json(
                a->ccxml, "{\"name\":\"room.call.terminal\"}");
        }
        /* SCXML content declares call.terminal; normalize the CCXML alias. */
        scxml_name = "call.terminal";
    } else if (strcmp(event->event_type.data, "call.terminal") == 0) {
        scxml_name = "call.terminal";
    } else if (strcmp(event->event_type.data, "room.assigned") == 0) {
        scxml_name = "room.assigned";
    } else if (strcmp(event->event_type.data, "rtc.connected") == 0) {
        scxml_name = "rtc.connected";
    } else if (strcmp(event->event_type.data, "rtc.disconnected") == 0) {
        scxml_name = "rtc.disconnected";
    } else if (strcmp(event->event_type.data, "rtc.reconnected") == 0) {
        scxml_name = "rtc.reconnected";
    } else if (strcmp(event->event_type.data, "rtc.retry_exhausted") == 0) {
        scxml_name = "rtc.retry_exhausted";
    } else if (strcmp(event->event_type.data, "media.input_stalled") == 0) {
        scxml_name = "media.input_stalled";
    } else if (strcmp(event->event_type.data, "provider.error") == 0) {
        scxml_name = "provider.error";
    } else if (strcmp(event->event_type.data, "rtc.failed") == 0) {
        scxml_name = "rtc.failed";
    } else if (strcmp(event->event_type.data, "rtc.closed") == 0) {
        scxml_name = "rtc.closed";
    } else if (strcmp(event->event_type.data, "command.result") == 0) {
        scxml_name = "command.result";
    } else if (strcmp(event->event_type.data, "room.snapshot.loaded") == 0) {
        /* No transition consumes the snapshot; it only unblocks the session's
           sequence recovery. The engine keeps its current state. */
        scxml_name = "room.snapshot.loaded";
    } else {
        a->unhandled_events++;
        return IVR_OK;
    }
    if (scxml_name) {
        scxml_fire(a, scxml_name);
    }
    return IVR_OK;
}

static ivr_status_t turboxml_step(ivr_xml_engine_t *engine) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)engine;
    if (!a) {
        return IVR_EINVAL;
    }
    if (a->ccxml_ready) {
        /* One step per event: the dialog bridge runs the full current dialog
           (prompt, collect_input, filled) synchronously within the step. */
        turboxml_ccxml_step(a->ccxml);
    }
    if (a->scxml_ready && a->scxml_pending) {
        /* Drain bounded microsteps for the injected event. The engine's
           step() blocks forever when the external queue is empty
           (Interpreter::step defaults to blockMs=max), so we must stop as soon
           as a step reports a stable configuration: the next step would block.
           `scxml_pending` is set by scxml_fire() and consumed here. */
        a->scxml_pending = 0;
        a->scxml_event_seen = 0;
        a->scxml_stable = 0;
        for (int i = 0; i < 16 && !a->terminated; i++) {
            turboxml_scxml_step(a->scxml);
            if (turboxml_scxml_is_finished(a->scxml) ||
                (a->scxml_event_seen && a->scxml_stable)) {
                break;
            }
        }
    }
    return IVR_OK;
}

static void turboxml_request_terminal(ivr_xml_engine_t *engine) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)engine;
    if (!a) {
        return;
    }
    a->terminated = 1;
    /* The blocked CCXML collect_input is woken by the session's terminal latch
       via hooks.is_terminal(); nothing else is safe from this thread. */
}

static void turboxml_destroy(ivr_xml_engine_t *engine) {
    ivr_turboxml_adapter_t *a = (ivr_turboxml_adapter_t *)engine;
    if (!a) {
        return;
    }
    if (a->ccxml) {
        turboxml_ccxml_destroy(a->ccxml);
        a->ccxml = NULL;
    }
    if (a->scxml) {
        turboxml_scxml_destroy(a->scxml);
        a->scxml = NULL;
    }
    ivr_str_free(&a->pending_input_value);
    free(a);
}

const ivr_xml_engine_ops_t ivr_turboxml_engine_ops = {
    .create = turboxml_create,
    .submit_event = turboxml_submit_event,
    .step = turboxml_step,
    .request_terminal = turboxml_request_terminal,
    .destroy = turboxml_destroy,
};
