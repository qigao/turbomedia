#include "ivr_media_bot.h"
#include "ivr_internal.h"
#include "ivr_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_MEDIA_BOT_DEFAULT_SAMPLE_RATE 16000u

struct ivr_media_bot_s {
    const turbo_tts_provider_t *tts_provider;
    const turbo_asr_provider_t *asr_provider;
    ivr_media_transport_t transport;
    uint32_t sample_rate;
    void (*on_event)(void *ctx, const ivr_event_view_t *event);
    void *event_ctx;
    ivr_mutex_t lock;
    /* signaled when in-flight session borrows drain (stop_bot waits on it) */
    ivr_cond_t cond;
    /* active peer (baseline: one active call per bot). Owned by the media
       control thread; provider callbacks read it under the lock and rely on
       the turbo_speech cancel() quiescence barrier for lifetime. */
    int active;
    /* stop_bot in progress: no new start_bot/borrows until the old sessions
       are torn down (start/stop never overlap). */
    int stopping;
    /* in-flight tts/asr borrows held by feed_caller_audio/play_pcm/
       cancel_input while they call into the provider outside the lock.
       stop_bot waits for this to drain before cancel/destroy. */
    uint32_t session_refs;
    ivr_str_t room_id;
    ivr_str_t call_id;
    uint64_t call_generation;
    turbo_tts_t *tts;
    turbo_asr_t *asr;
    /* diagnostics */
    uint64_t tts_synthesized;
    uint64_t tts_frames;
    uint64_t asr_finals;
    uint64_t dropped_audio;
};

static void bot_call_view(ivr_media_bot_t *b, ivr_call_ref_t *out) {
    memset(out, 0, sizeof(*out));
    out->room_id.data = b->room_id.data;
    out->room_id.size = b->room_id.size;
    out->call_id.data = b->call_id.data;
    out->call_id.size = b->call_id.size;
    out->call_generation = b->call_generation;
}

static int bot_call_matches_locked(ivr_media_bot_t *b,
                                   const ivr_call_ref_t *call) {
    return b->active && call &&
           call->call_generation == b->call_generation &&
           call->room_id.size == b->room_id.size &&
           (call->room_id.size == 0 ||
            memcmp(call->room_id.data, b->room_id.data,
                   call->room_id.size) == 0) &&
           call->call_id.size == b->call_id.size &&
           (call->call_id.size == 0 ||
            memcmp(call->call_id.data, b->call_id.data,
                   call->call_id.size) == 0);
}

/* ------------------------------------------------------------------ */
/* speech session callbacks                                            */
/* ------------------------------------------------------------------ */

static int bot_tts_audio(turbo_tts_t *tts, const turbo_speech_audio_frame_t *frame,
                         void *user_data) {
    ivr_media_bot_t *b = (ivr_media_bot_t *)user_data;
    (void)tts;
    if (!b || !frame || !b->transport.play_audio) {
        return 0;
    }
    ivr_call_ref_t call;
    ivr_mutex_lock(&b->lock);
    if (!b->active) {
        ivr_mutex_unlock(&b->lock);
        return 0;
    }
    bot_call_view(b, &call);
    b->tts_frames++;
    uint32_t sample_rate = b->sample_rate;
    ivr_mutex_unlock(&b->lock);
    return b->transport.play_audio(b->transport.context, &call, frame->data,
                                   frame->len, sample_rate);
}

static void bot_tts_complete(turbo_tts_t *tts, void *user_data) {
    (void)tts;
    (void)user_data;
}

static void bot_emit_provider_error(ivr_media_bot_t *b, const char *provider,
                                    int error_code) {
    static const ivr_bytes_view_t type = {"provider.error", 14};
    ivr_call_ref_t call;
    ivr_event_view_t event;
    ivr_json_builder_t json;
    char payload[96];
    char number[32];

    if (!b || !provider || !b->on_event) {
        return;
    }
    ivr_mutex_lock(&b->lock);
    if (!b->active) {
        ivr_mutex_unlock(&b->lock);
        return;
    }
    bot_call_view(b, &call);
    ivr_mutex_unlock(&b->lock);

    ivr_json_builder_init(&json, payload, sizeof(payload));
    ivr_json_builder_raw(&json, "{\"provider\":");
    ivr_json_builder_string_cstr(&json, provider);
    ivr_json_builder_raw(&json, ",\"error_code\":");
    snprintf(number, sizeof(number), "%d", error_code);
    ivr_json_builder_raw(&json, number);
    ivr_json_builder_raw(&json, "}");
    if (!ivr_json_builder_ok(&json)) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.event_type = type;
    event.call = call;
    event.payload_json.data = payload;
    event.payload_json.size = json.pos;
    b->on_event(b->event_ctx, &event);
}

static void bot_tts_error(turbo_tts_t *tts, int error_code, const char *message,
                          void *user_data) {
    (void)tts;
    (void)message;
    bot_emit_provider_error((ivr_media_bot_t *)user_data, "tts", error_code);
}

static void bot_asr_result(turbo_asr_t *asr, const turbo_asr_result_t *result,
                           void *user_data) {
    ivr_media_bot_t *b = (ivr_media_bot_t *)user_data;
    (void)asr;
    if (!b || !result || !result->is_final) {
        return;
    }
    ivr_call_ref_t call;
    ivr_mutex_lock(&b->lock);
    if (!b->active) {
        ivr_mutex_unlock(&b->lock);
        return;
    }
    bot_call_view(b, &call);
    b->asr_finals++;
    ivr_mutex_unlock(&b->lock);
    if (!b->on_event) {
        return;
    }
    static const ivr_bytes_view_t type = {"asr.final", 9};
    char json[512];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"text\":");
    ivr_json_builder_string(&jb, result->text ? result->text : "",
                            result->text ? result->text_len : 0);
    ivr_json_builder_raw(&jb, ",\"confidence\":");
    snprintf(num, sizeof(num), "%.3f", result->confidence);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return;
    }
    ivr_event_view_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = type;
    ev.call = call;
    ev.input_value.data = result->text;
    ev.input_value.size = result->text_len;
    ev.payload_json.data = json;
    ev.payload_json.size = jb.pos;
    b->on_event(b->event_ctx, &ev);
}

static void bot_asr_complete(turbo_asr_t *asr, void *user_data) {
    (void)asr;
    (void)user_data;
}

static void bot_asr_error(turbo_asr_t *asr, int error_code, const char *message,
                          void *user_data) {
    (void)asr;
    (void)message;
    bot_emit_provider_error((ivr_media_bot_t *)user_data, "asr", error_code);
}

/* ------------------------------------------------------------------ */
/* ivr_media_port_ops_t                                                */
/* ------------------------------------------------------------------ */

/* Borrow a live session for an out-of-lock provider call. Returns NULL when
   the bot has no active matching call or the session is gone; otherwise
   increments session_refs so stop_bot waits for the borrow to drain before
   cancel/destroy. Every successful borrow must be released exactly once with
   bot_session_release(). */
static turbo_tts_t *bot_tts_borrow(ivr_media_bot_t *b,
                                   const ivr_call_ref_t *call) {
    ivr_mutex_lock(&b->lock);
    turbo_tts_t *tts = NULL;
    if (bot_call_matches_locked(b, call) && b->tts) {
        tts = b->tts;
        b->session_refs++;
    }
    ivr_mutex_unlock(&b->lock);
    return tts;
}

static turbo_asr_t *bot_asr_borrow(ivr_media_bot_t *b,
                                   const ivr_call_ref_t *call) {
    ivr_mutex_lock(&b->lock);
    turbo_asr_t *asr = NULL;
    if (bot_call_matches_locked(b, call) && b->asr) {
        asr = b->asr;
        b->session_refs++;
    }
    ivr_mutex_unlock(&b->lock);
    return asr;
}

static void bot_session_release(ivr_media_bot_t *b) {
    ivr_mutex_lock(&b->lock);
    if (b->session_refs > 0) {
        b->session_refs--;
    }
    if (b->stopping && b->session_refs == 0) {
        ivr_cond_broadcast(&b->cond);
    }
    ivr_mutex_unlock(&b->lock);
}

static ivr_status_t bot_start_bot(void *ctx, const ivr_call_ref_t *call) {
    ivr_media_bot_t *b = (ivr_media_bot_t *)ctx;
    if (!b || !call) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&b->lock);
    if (b->active || b->stopping) {
        ivr_mutex_unlock(&b->lock);
        return IVR_ESTATE; /* baseline: one active call per bot */
    }
    if (ivr_str_assign(&b->room_id, call->room_id.data, call->room_id.size) < 0 ||
        ivr_str_assign(&b->call_id, call->call_id.data, call->call_id.size) < 0) {
        ivr_mutex_unlock(&b->lock);
        return IVR_ENOSPC;
    }
    b->call_generation = call->call_generation;
    int rc = 0;
    if (b->tts_provider) {
        turbo_tts_callbacks_t cb = {bot_tts_audio, bot_tts_complete,
                                    bot_tts_error};
        b->tts = turbo_tts_create(b->tts_provider, &cb, b);
        if (!b->tts) {
            rc = -1;
        }
    }
    if (rc == 0 && b->asr_provider) {
        turbo_asr_callbacks_t cb = {bot_asr_result, bot_asr_complete,
                                    bot_asr_error};
        b->asr = turbo_asr_create(b->asr_provider, &cb, b);
        if (!b->asr) {
            rc = -1;
        }
    }
    if (rc == 0 && b->asr) {
        turbo_asr_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.format.sample_rate = (int)b->sample_rate;
        cfg.format.channels = 1;
        cfg.format.bits_per_sample = 16;
        if (turbo_asr_start(b->asr, &cfg) != TURBO_SPEECH_OK) {
            rc = -1;
        }
    }
    if (rc != 0) {
        if (b->asr) {
            turbo_asr_destroy(b->asr);
            b->asr = NULL;
        }
        if (b->tts) {
            turbo_tts_destroy(b->tts);
            b->tts = NULL;
        }
        ivr_str_free(&b->room_id);
        ivr_str_free(&b->call_id);
        ivr_mutex_unlock(&b->lock);
        return IVR_ENOSPC;
    }
    b->active = 1;
    ivr_mutex_unlock(&b->lock);
    return IVR_OK;
}

static ivr_status_t bot_play_pcm(void *ctx, const ivr_call_ref_t *call,
                                 const ivr_bytes_view_t *text) {
    ivr_media_bot_t *b = (ivr_media_bot_t *)ctx;
    if (!b || !call || !text) {
        return IVR_EINVAL;
    }
    /* the borrow keeps the TTS session alive across the provider call even
       when stop_bot runs concurrently on another thread */
    turbo_tts_t *tts = bot_tts_borrow(b, call);
    if (!tts) {
        return IVR_ESTATE; /* no active call / no TTS provider */
    }
    turbo_tts_request_t request;
    memset(&request, 0, sizeof(request));
    request.text = text->data;
    request.text_len = text->size;
    request.rate = 1.0f;
    request.pitch = 1.0f;
    ivr_status_t rc = IVR_ESTATE;
    if (turbo_tts_synthesize(tts, &request) == TURBO_SPEECH_OK) {
        b->tts_synthesized++;
        rc = IVR_OK;
    }
    bot_session_release(b);
    return rc;
}

static ivr_status_t bot_cancel_input(void *ctx, const ivr_call_ref_t *call) {
    ivr_media_bot_t *b = (ivr_media_bot_t *)ctx;
    if (!b || !call) {
        return IVR_EINVAL;
    }
    /* borrow both sessions so a concurrent stop_bot waits for the cancels
       before destroying them */
    turbo_tts_t *tts = bot_tts_borrow(b, call);
    turbo_asr_t *asr = bot_asr_borrow(b, call);
    if (tts) {
        (void)turbo_tts_cancel(tts);
        bot_session_release(b);
    }
    if (asr) {
        (void)turbo_asr_cancel(asr);
        bot_session_release(b);
    }
    return (tts || asr) ? IVR_OK : IVR_ESTATE;
}

static ivr_status_t bot_stop_bot(void *ctx, const ivr_call_ref_t *call) {
    ivr_media_bot_t *b = (ivr_media_bot_t *)ctx;
    if (!b || !call) {
        return IVR_EINVAL;
    }
    ivr_call_ref_t active_call;
    ivr_mutex_lock(&b->lock);
    if (!bot_call_matches_locked(b, call)) {
        ivr_mutex_unlock(&b->lock);
        return IVR_ESTATE;
    }
    b->active = 0;
    b->stopping = 1;
    bot_call_view(b, &active_call);
    turbo_tts_t *tts = b->tts;
    turbo_asr_t *asr = b->asr;
    b->tts = NULL;
    b->asr = NULL;
    /* wait for in-flight feed/play/cancel borrows to drain so no thread is
       inside the provider when cancel/destroy run */
    while (b->session_refs > 0) {
        ivr_cond_wait(&b->cond, &b->lock);
    }
    ivr_mutex_unlock(&b->lock);
    /* cancel() is the callback quiescence barrier: only then destroy and free
       the owned call strings. */
    if (asr) {
        (void)turbo_asr_cancel(asr);
        turbo_asr_destroy(asr);
    }
    if (tts) {
        (void)turbo_tts_cancel(tts);
        turbo_tts_destroy(tts);
    }
    if (b->transport.stop) {
        (void)b->transport.stop(b->transport.context, &active_call);
    }
    ivr_mutex_lock(&b->lock);
    b->stopping = 0;
    ivr_str_free(&b->room_id);
    ivr_str_free(&b->call_id);
    b->call_generation = 0;
    ivr_mutex_unlock(&b->lock);
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

ivr_status_t ivr_media_bot_create(const ivr_media_bot_config_t *config,
                                  ivr_media_bot_t **out_bot) {
    if (!config || !out_bot) {
        return IVR_EINVAL;
    }
    ivr_media_bot_t *b = (ivr_media_bot_t *)calloc(1, sizeof(*b));
    if (!b) {
        return IVR_ENOSPC;
    }
    b->tts_provider = config->tts_provider;
    b->asr_provider = config->asr_provider;
    b->transport = config->transport;
    b->sample_rate = config->sample_rate ? config->sample_rate
                                         : IVR_MEDIA_BOT_DEFAULT_SAMPLE_RATE;
    b->on_event = config->on_event;
    b->event_ctx = config->event_ctx;
    ivr_str_init(&b->room_id);
    ivr_str_init(&b->call_id);
    if (ivr_mutex_init(&b->lock) != 0) {
        free(b);
        return IVR_ENOSPC;
    }
    if (ivr_cond_init(&b->cond) != 0) {
        ivr_mutex_destroy(&b->lock);
        free(b);
        return IVR_ENOSPC;
    }
    *out_bot = b;
    return IVR_OK;
}

void ivr_media_bot_get_ops(ivr_media_bot_t *bot, ivr_media_port_ops_t *ops) {
    if (!bot || !ops) {
        return;
    }
    memset(ops, 0, sizeof(*ops));
    ops->abi_version = IVR_WORKER_ABI_VERSION;
    ops->context = bot;
    ops->start_bot = bot_start_bot;
    ops->play_pcm = bot_play_pcm;
    ops->cancel_input = bot_cancel_input;
    ops->stop_bot = bot_stop_bot;
}

ivr_status_t ivr_media_bot_feed_caller_audio(ivr_media_bot_t *bot,
                                             const ivr_call_ref_t *call,
                                             const uint8_t *pcm, size_t len) {
    if (!bot || !call || (!pcm && len > 0)) {
        return IVR_EINVAL;
    }
    /* the borrow keeps the ASR session alive across write_pcm even when
       stop_bot runs concurrently on another thread */
    turbo_asr_t *asr = bot_asr_borrow(bot, call);
    if (!asr) {
        bot->dropped_audio++;
        return IVR_ESTATE;
    }
    ivr_status_t rc = (turbo_asr_write_pcm(asr, pcm, len, 0) == TURBO_SPEECH_OK)
                          ? IVR_OK
                          : IVR_ESTATE;
    bot_session_release(bot);
    return rc;
}

void ivr_media_bot_destroy(ivr_media_bot_t *bot) {
    if (!bot) {
        return;
    }
    /* fail fast: a live bot must be stopped before destroy */
    ivr_mutex_lock(&bot->lock);
    int active = bot->active;
    ivr_call_ref_t call;
    bot_call_view(bot, &call);
    ivr_mutex_unlock(&bot->lock);
    if (active) {
        ivr_media_port_ops_t ops;
        ivr_media_bot_get_ops(bot, &ops);
        (void)ops.stop_bot(ops.context, &call);
    }
    ivr_str_free(&bot->room_id);
    ivr_str_free(&bot->call_id);
    ivr_cond_destroy(&bot->cond);
    ivr_mutex_destroy(&bot->lock);
    free(bot);
}
