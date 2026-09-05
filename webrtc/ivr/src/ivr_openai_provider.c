/*
 * ivr_openai_provider.c - Remote TTS/ASR providers over an OpenAI-compatible
 * audio API. See ivr_openai_provider.h for the protocol and threading model.
 */
#include "ivr_openai_provider.h"
#include "ivr_thread.h"
#include "http_client.h"
#include "platform.h"
#include <json_parser.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_OPENAI_DEFAULT_TTS_PATH "/v1/audio/speech"
#define IVR_OPENAI_DEFAULT_ASR_PATH "/v1/audio/transcriptions"
#define IVR_OPENAI_DEFAULT_TTS_MODEL "tts-1"
#define IVR_OPENAI_DEFAULT_TTS_VOICE "alloy"
#define IVR_OPENAI_DEFAULT_ASR_MODEL "whisper-1"
#define IVR_OPENAI_NATIVE_TTS_RATE 24000
#define IVR_OPENAI_DEFAULT_SAMPLE_RATE 16000
#define IVR_OPENAI_DEFAULT_CHANNELS 1
#define IVR_OPENAI_DEFAULT_BITS 16
#define IVR_OPENAI_DEFAULT_FRAME_BYTES 3200u
#define IVR_OPENAI_DEFAULT_MAX_TTS_INPUT (64u * 1024u)
#define IVR_OPENAI_DEFAULT_MAX_ASR_BUFFER (4u * 1024u * 1024u)
#define IVR_OPENAI_DEFAULT_MAX_RESPONSE (16u * 1024u * 1024u)
#define IVR_OPENAI_DEFAULT_TIMEOUT_MS 30000
#define IVR_OPENAI_DEFAULT_CONNECT_TIMEOUT_MS 5000
#define IVR_OPENAI_DEFAULT_USER_AGENT "TurboMediaIVR/1.0"
#define IVR_OPENAI_WAV_HEADER_SIZE 44u
#define IVR_OPENAI_ERROR_MESSAGE_MAX 256

static atomic_uint_least64_t g_active_tts_instances;
static atomic_uint_least64_t g_active_asr_instances;
static atomic_uint_least64_t g_live_provider_threads;
static atomic_uint_least64_t g_retained_input_bytes;
static atomic_uint_least64_t g_retained_response_bytes;

static void ivr_openai_observe_request(
    const ivr_openai_config_t *config, ivr_openai_request_kind_t kind,
    uint64_t started_at_ms) {
    uint64_t finished_at_ms;
    if (!config || !config->observer.on_request_complete) {
        return;
    }
    finished_at_ms = salts_monotonic_ms();
    config->observer.on_request_complete(
        config->observer.context, kind,
        finished_at_ms >= started_at_ms ? finished_at_ms - started_at_ms : 0);
}

static void ivr_openai_retain_input(size_t bytes) {
    atomic_fetch_add_explicit(&g_retained_input_bytes, (uint64_t)bytes,
                              memory_order_relaxed);
}

static void ivr_openai_release_input(size_t bytes) {
    atomic_fetch_sub_explicit(&g_retained_input_bytes, (uint64_t)bytes,
                              memory_order_relaxed);
}

static void ivr_openai_retain_response(const http_response_t *response) {
    if (response) {
        atomic_fetch_add_explicit(&g_retained_response_bytes,
                                  (uint64_t)response->body_len,
                                  memory_order_relaxed);
    }
}

static void ivr_openai_response_free(http_response_t *response) {
    if (response) {
        atomic_fetch_sub_explicit(&g_retained_response_bytes,
                                  (uint64_t)response->body_len,
                                  memory_order_relaxed);
        http_response_free(response);
    }
}

/* ------------------------------------------------------------------ */
/* little-endian writers                                               */
/* ------------------------------------------------------------------ */

static void le_write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void le_write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* WAV container (PCM fmt)                                             */
/* ------------------------------------------------------------------ */

/* Build a RIFF/WAVE container around raw PCM. Returns 0 and a malloc'd
   buffer on success; -1 on invalid args or allocation failure. */
static int ivr_openai_build_wav(const uint8_t *pcm, size_t pcm_len,
                                int sample_rate, int channels, int bits,
                                uint8_t **out_wav, size_t *out_wav_len) {
    uint8_t *wav;
    size_t total;
    uint32_t byte_rate;
    uint16_t block_align;

    if (!pcm || !out_wav || !out_wav_len || sample_rate <= 0 || channels <= 0 ||
        bits <= 0 || pcm_len > 0xFFFFFFFFu - IVR_OPENAI_WAV_HEADER_SIZE) {
        return -1;
    }
    total = IVR_OPENAI_WAV_HEADER_SIZE + pcm_len;
    wav = (uint8_t *)malloc(total);
    if (!wav) {
        return -1;
    }
    block_align = (uint16_t)((bits / 8) * channels);
    byte_rate = (uint32_t)sample_rate * block_align;

    memcpy(wav, "RIFF", 4);
    le_write_u32(wav + 4, (uint32_t)(total - 8));
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    le_write_u32(wav + 16, 16u);                 /* fmt chunk size */
    le_write_u16(wav + 20, 1u);                  /* PCM */
    le_write_u16(wav + 22, (uint16_t)channels);
    le_write_u32(wav + 24, (uint32_t)sample_rate);
    le_write_u32(wav + 28, byte_rate);
    le_write_u16(wav + 32, block_align);
    le_write_u16(wav + 34, (uint16_t)bits);
    memcpy(wav + 36, "data", 4);
    le_write_u32(wav + 40, (uint32_t)pcm_len);
    if (pcm_len > 0) {
        memcpy(wav + IVR_OPENAI_WAV_HEADER_SIZE, pcm, pcm_len);
    }
    *out_wav = wav;
    *out_wav_len = total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* multipart/form-data body builder                                     */
/* ------------------------------------------------------------------ */

static int ivr_openai_buf_append(char **buf, size_t *cap, size_t *len,
                                 const void *data, size_t n) {
    char *nb;
    size_t need;
    if (n == 0) {
        return 0;
    }
    need = *len + n;
    if (need > *cap) {
        size_t new_cap = *cap ? *cap : 1024u;
        while (new_cap < need) {
            new_cap *= 2u;
        }
        nb = (char *)realloc(*buf, new_cap);
        if (!nb) {
            return -1;
        }
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 0;
}

static int ivr_openai_buf_append_fmt(char **buf, size_t *cap, size_t *len,
                                     const char *fmt, ...) {
    char stack[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    if ((size_t)n < sizeof(stack)) {
        return ivr_openai_buf_append(buf, cap, len, stack, (size_t)n);
    }
    {
        char *heap = (char *)malloc((size_t)n + 1u);
        int rc;
        if (!heap) {
            return -1;
        }
        va_start(ap, fmt);
        (void)vsnprintf(heap, (size_t)n + 1u, fmt, ap);
        va_end(ap);
        rc = ivr_openai_buf_append(buf, cap, len, heap, (size_t)n);
        free(heap);
        return rc;
    }
}

/* Build a multipart/form-data request body carrying one file part (the WAV)
   plus plain text fields. The boundary is derived from a monotonic counter so
   it is unique per request and cannot collide with the binary payload.
   Returns 0 and malloc'd outputs on success. */
static int ivr_openai_build_multipart(const uint8_t *wav, size_t wav_len,
                                      const char *model, const char *language,
                                      char **out_body, size_t *out_body_len,
                                      char **out_content_type) {
    static _Atomic uint64_t boundary_seq = 0;
    char boundary[64];
    char *body = NULL;
    size_t cap = 0;
    size_t len = 0;
    char *content_type;
    uint64_t seq = atomic_fetch_add(&boundary_seq, 1u) + 1u;

    if (!wav || !model || !out_body || !out_body_len || !out_content_type) {
        return -1;
    }
    snprintf(boundary, sizeof(boundary),
             "----TurboMediaIVR%016llx", (unsigned long long)seq);
    if (ivr_openai_buf_append_fmt(
            &body, &cap, &len,
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"file\"; "
            "filename=\"audio.wav\"\r\n"
            "Content-Type: audio/wav\r\n\r\n",
            boundary) != 0 ||
        ivr_openai_buf_append(&body, &cap, &len, wav, wav_len) != 0 ||
        ivr_openai_buf_append(&body, &cap, &len, "\r\n", 2) != 0 ||
        ivr_openai_buf_append_fmt(
            &body, &cap, &len,
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
            "%s\r\n",
            boundary, model) != 0) {
        free(body);
        return -1;
    }
    if (language && language[0] != '\0') {
        if (ivr_openai_buf_append_fmt(
                &body, &cap, &len,
                "--%s\r\n"
                "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
                "%s\r\n",
                boundary, language) != 0) {
            free(body);
            return -1;
        }
    }
    if (ivr_openai_buf_append_fmt(&body, &cap, &len, "--%s--\r\n",
                                  boundary) != 0) {
        free(body);
        return -1;
    }
    content_type = (char *)malloc(strlen(boundary) + 40u);
    if (!content_type) {
        free(body);
        return -1;
    }
    snprintf(content_type, strlen(boundary) + 40u,
             "multipart/form-data; boundary=%s", boundary);
    *out_body = body;
    *out_body_len = len;
    *out_content_type = content_type;
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

/* Escape a string for embedding in a JSON string literal. Returns a malloc'd
   NUL-terminated buffer; *out_len receives the escaped length. */
static char *ivr_openai_json_escape(const char *s, size_t len, size_t *out_len) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t cap = len + 1;
    char *out;

    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            cap += 1;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            cap += 1;
        } else if (c < 0x20) {
            cap += 5; /* \u00XX */
        }
    }
    out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    {
        size_t w = 0;
        for (i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)s[i];
            switch (c) {
                case '"':
                    out[w++] = '\\';
                    out[w++] = '"';
                    break;
                case '\\':
                    out[w++] = '\\';
                    out[w++] = '\\';
                    break;
                case '\n':
                    out[w++] = '\\';
                    out[w++] = 'n';
                    break;
                case '\r':
                    out[w++] = '\\';
                    out[w++] = 'r';
                    break;
                case '\t':
                    out[w++] = '\\';
                    out[w++] = 't';
                    break;
                default:
                    if (c < 0x20) {
                        out[w++] = '\\';
                        out[w++] = 'u';
                        out[w++] = '0';
                        out[w++] = '0';
                        out[w++] = hex[(c >> 4) & 0xFu];
                        out[w++] = hex[c & 0xFu];
                    } else {
                        out[w++] = (char)c;
                    }
                    break;
            }
        }
        out[w] = '\0';
        if (out_len) {
            *out_len = w;
        }
    }
    return out;
}

/* Build the TTS request JSON body. Returns a malloc'd string. */
static char *ivr_openai_build_tts_body(const char *model, const char *voice,
                                       const char *text, size_t text_len,
                                       float speed) {
    char *escaped = NULL;
    size_t escaped_len = 0;
    size_t cap;
    char *body;

    if (!model || !voice || !text) {
        return NULL;
    }
    escaped = ivr_openai_json_escape(text, text_len, &escaped_len);
    if (!escaped) {
        return NULL;
    }
    /* model + voice + escaped + fixed JSON scaffolding + NUL */
    cap = strlen(model) + strlen(voice) + escaped_len + 96;
    body = (char *)malloc(cap);
    if (body) {
        int n = snprintf(body, cap,
                         "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\","
                         "\"response_format\":\"pcm\",\"speed\":%.3g}",
                         model, escaped, voice, (double)speed);
        if (n < 0 || (size_t)n >= cap) {
            free(body);
            body = NULL;
        }
    }
    free(escaped);
    return body;
}

/* Extract the "text" field from an OpenAI transcription JSON response into a
   malloc'd NUL-terminated string. Returns NULL when absent/not a string. */
static char *ivr_openai_parse_transcript(const char *json, size_t json_len) {
    json_value_t *root = NULL;
    json_value_t *text_value;
    char *out = NULL;

    if (!json || json_len == 0 ||
        ((root = json_parse((const char *)((const uint8_t *)json), json_len)) ? 0 : -1) != 0 || !root ||
        json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        return NULL;
    }
    text_value = json_object_get(root, "text");
    if (text_value && json_type(text_value) == JSON_STRING) {
        const char *s = json_string(text_value);
        size_t len = json_string_len(text_value);
        out = (char *)malloc(len + 1);
        if (out) {
            if (len > 0) {
                memcpy(out, s, len);
            }
            out[len] = '\0';
        }
    }
    json_free(root);
    root = NULL;
    return out;
}
/* ------------------------------------------------------------------ */
/* int16 mono linear-interpolation resampler                           */
/* ------------------------------------------------------------------ */

/* Resample int16 mono PCM from in_rate to out_rate. Returns a malloc'd
   buffer and sets *out_samples. When rates match, returns a copy (caller
   always owns the result). Returns NULL on invalid input/OOM. */
static int16_t *ivr_openai_resample_i16(const int16_t *in, size_t in_samples,
                                        int in_rate, int out_rate,
                                        size_t *out_samples) {
    size_t i;
    size_t n_out;
    int16_t *out;

    if (!in || in_samples == 0 || in_rate <= 0 || out_rate <= 0 || !out_samples) {
        return NULL;
    }
    if (in_rate == out_rate) {
        out = (int16_t *)malloc(in_samples * sizeof(int16_t));
        if (out) {
            memcpy(out, in, in_samples * sizeof(int16_t));
            *out_samples = in_samples;
        }
        return out;
    }
    /* out_n = round(in_n * out_rate / in_rate) */
    n_out = (size_t)(((uint64_t)in_samples * (uint64_t)out_rate +
                      (uint64_t)in_rate / 2u) /
                     (uint64_t)in_rate);
    if (n_out == 0) {
        n_out = 1;
    }
    out = (int16_t *)malloc(n_out * sizeof(int16_t));
    if (!out) {
        return NULL;
    }
    for (i = 0; i < n_out; ++i) {
        double pos = (double)i * (double)in_rate / (double)out_rate;
        size_t idx = (size_t)pos;
        double frac = pos - (double)idx;
        double a;
        double b;
        if (idx >= in_samples - 1u) {
            idx = in_samples - 1u;
            frac = 0.0;
        }
        a = (double)in[idx];
        b = (double)in[idx + 1u];
        out[i] = (int16_t)(a + (b - a) * frac);
    }
    *out_samples = n_out;
    return out;
}

/* ------------------------------------------------------------------ */
/* http_client setup helpers                                           */
/* ------------------------------------------------------------------ */

static http_client_t *ivr_openai_create_client(const ivr_openai_config_t *config) {
    http_client_t *client;
    turbo_tls_client_config_t tls_config;

    if (!config || !config->base_url || config->base_url[0] == '\0') {
        return NULL;
    }
    client = http_client_create(config->base_url);
    if (!client) {
        return NULL;
    }
    /* 0 in the config selects the provider defaults (never disable the
       timeouts: cancel()/destroy() join the worker, so an unbounded request
       would turn a hang into an unbounded quiescence wait). */
    http_client_set_timeout(client, config->timeout_ms
                                 ? config->timeout_ms
                                 : IVR_OPENAI_DEFAULT_TIMEOUT_MS);
    http_client_set_connect_timeout(
        client, config->connect_timeout_ms
                    ? config->connect_timeout_ms
                    : IVR_OPENAI_DEFAULT_CONNECT_TIMEOUT_MS);
    http_client_set_max_response_size(
        client, config->max_response_bytes ? config->max_response_bytes
                                           : IVR_OPENAI_DEFAULT_MAX_RESPONSE);
    http_client_set_user_agent(client, config->user_agent);
    if (config->api_key && config->api_key[0] != '\0') {
        http_client_set_bearer_token(client, config->api_key);
    }
    if (config->ca_file && config->ca_file[0] != '\0') {
        memset(&tls_config, 0, sizeof(tls_config));
        tls_config.ca_file = config->ca_file;
        tls_config.verify_peer = 1;
        if (http_client_set_tls_client_config(client, &tls_config) != 0) {
            http_client_destroy(client);
            return NULL;
        }
    }
    return client;
}

static int ivr_openai_response_error(const http_response_t *response,
                                     char *out_msg, size_t out_cap) {
    if (!response) {
        snprintf(out_msg, out_cap, "no HTTP response");
        return -1;
    }
    if (response->error_code != HTTP_ERROR_NONE) {
        snprintf(out_msg, out_cap, "HTTP error: %s",
                 http_error_to_str(response->error_code));
        return -1;
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        const char *detail = response->body ? response->body : "";
        size_t detail_len = response->body_len;
        if (detail_len > 160u) {
            detail_len = 160u;
        }
        snprintf(out_msg, out_cap, "HTTP status %d: %.*s", response->status_code,
                 (int)detail_len, detail);
        return -1;
    }
    return 0;
}
/* ------------------------------------------------------------------ */
/* TTS wrapper                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    ivr_openai_tts_t *tts;
    turbo_tts_provider_callbacks_t callbacks;
    void *callback_user_data;
    char *text;
    size_t text_len;
    size_t retained_bytes;
    float rate;
} ivr_openai_tts_job_t;

static void ivr_openai_tts_job_free(ivr_openai_tts_job_t *job) {
    if (!job) {
        return;
    }
    free(job->text);
    ivr_openai_release_input(job->retained_bytes);
    free(job);
}

struct ivr_openai_tts {
    const ivr_openai_config_t *config;
    ivr_mutex_t lock;
    ivr_cond_t cond;
    ivr_thread_t thread;
    int thread_started;
    int shutdown;
    int job_pending;
    ivr_openai_tts_job_t *job; /* owned by the worker once dequeued */
    int worker_running;
    atomic_int cancel_requested;
    /* diagnostics */
    uint64_t synthesized;
    uint64_t delivered_frames;
    uint64_t delivered_bytes;
    uint64_t errors;
};

static int ivr_openai_tts_deliver(ivr_openai_tts_t *tts,
                                  ivr_openai_tts_job_t *job,
                                  const int16_t *samples, size_t sample_count,
                                  int sample_rate, int channels, int bits) {
    size_t frame_bytes =
        tts->config->tts_frame_bytes ? tts->config->tts_frame_bytes
                                     : IVR_OPENAI_DEFAULT_FRAME_BYTES;
    size_t bytes_per_sample = (size_t)((bits / 8) * channels);
    size_t total_bytes = sample_count * bytes_per_sample;
    const uint8_t *data = (const uint8_t *)samples;
    size_t offset = 0;
    uint64_t ts = 0;

    while (offset < total_bytes) {
        size_t n = total_bytes - offset;
        turbo_speech_audio_frame_t frame;

        if (atomic_load(&tts->cancel_requested)) {
            return TURBO_SPEECH_ERR_STATE;
        }
        if (n > frame_bytes) {
            n = frame_bytes;
        }
        memset(&frame, 0, sizeof(frame));
        frame.data = data + offset;
        frame.len = n;
        frame.format.sample_rate = sample_rate;
        frame.format.channels = channels;
        frame.format.bits_per_sample = bits;
        frame.timestamp_us = ts;
        if (job->callbacks.on_audio(&frame, job->callback_user_data) !=
            TURBO_SPEECH_OK) {
            return TURBO_SPEECH_ERR_PROVIDER;
        }
        tts->delivered_frames++;
        tts->delivered_bytes += n;
        ts += (uint64_t)n * 1000000ULL /
              ((uint64_t)sample_rate * bytes_per_sample);
        offset += n;
    }
    return TURBO_SPEECH_OK;
}

static void ivr_openai_tts_run_job(ivr_openai_tts_t *tts,
                                   ivr_openai_tts_job_t *job) {
    const ivr_openai_config_t *config = tts->config;
    http_client_t *client = NULL;
    http_response_t *response = NULL;
    char *body = NULL;
    char error_msg[IVR_OPENAI_ERROR_MESSAGE_MAX];
    int16_t *resampled = NULL;
    size_t resampled_count = 0;
    int native_rate;
    int out_rate;
    int channels;
    int bits;
    const char *model;
    const char *voice;
    const char *path;

    if (atomic_load(&tts->cancel_requested)) {
        return;
    }
    model = config->tts_model ? config->tts_model : IVR_OPENAI_DEFAULT_TTS_MODEL;
    voice = config->tts_voice ? config->tts_voice : IVR_OPENAI_DEFAULT_TTS_VOICE;
    path = config->tts_path ? config->tts_path : IVR_OPENAI_DEFAULT_TTS_PATH;
    native_rate = config->tts_sample_rate ? config->tts_sample_rate
                                          : IVR_OPENAI_NATIVE_TTS_RATE;
    out_rate = config->sample_rate ? config->sample_rate
                                   : IVR_OPENAI_DEFAULT_SAMPLE_RATE;
    channels = config->channels ? config->channels : IVR_OPENAI_DEFAULT_CHANNELS;
    bits = config->bits_per_sample ? config->bits_per_sample
                                   : IVR_OPENAI_DEFAULT_BITS;

    body = ivr_openai_build_tts_body(model, voice, job->text, job->text_len,
                                     job->rate);
    if (!body) {
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_NOMEM,
                                    "TTS: request body build failed",
                                    job->callback_user_data);
        }
        return;
    }
    client = ivr_openai_create_client(config);
    if (!client) {
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER,
                                    "TTS: http client create failed",
                                    job->callback_user_data);
        }
        free(body);
        return;
    }
    response = http_post_json(client, path, body);
    free(body);
    if (!response) {
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER,
                                    "TTS: no HTTP response",
                                    job->callback_user_data);
        }
        http_client_destroy(client);
        return;
    }
    ivr_openai_retain_response(response);
    if (ivr_openai_response_error(response, error_msg, sizeof(error_msg)) != 0) {
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER, error_msg,
                                    job->callback_user_data);
        }
        ivr_openai_response_free(response);
        http_client_destroy(client);
        return;
    }
    if (response->body_len == 0 ||
        response->body_len % sizeof(int16_t) != 0) {
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_FORMAT,
                                    "TTS: invalid 16-bit PCM response",
                                    job->callback_user_data);
        }
        ivr_openai_response_free(response);
        http_client_destroy(client);
        return;
    }
    resampled = ivr_openai_resample_i16(
        (const int16_t *)response->body, response->body_len / sizeof(int16_t),
        native_rate, out_rate, &resampled_count);
    if (!resampled) {
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_NOMEM,
                                    "TTS: resample failed",
                                    job->callback_user_data);
        }
        ivr_openai_response_free(response);
        http_client_destroy(client);
        return;
    }
    if (ivr_openai_tts_deliver(tts, job, resampled, resampled_count, out_rate,
                               channels, bits) != TURBO_SPEECH_OK) {
        free(resampled);
        ivr_openai_response_free(response);
        http_client_destroy(client);
        if (!atomic_load(&tts->cancel_requested)) {
            tts->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER,
                                    "TTS: audio sink rejected frame",
                                    job->callback_user_data);
        }
        return;
    }
    free(resampled);
    ivr_openai_response_free(response);
    http_client_destroy(client);
    if (!atomic_load(&tts->cancel_requested)) {
        job->callbacks.on_complete(job->callback_user_data);
    }
}

static void *ivr_openai_tts_thread(void *opaque) {
    ivr_openai_tts_t *tts = (ivr_openai_tts_t *)opaque;

    atomic_fetch_add_explicit(&g_live_provider_threads, 1u,
                              memory_order_relaxed);

    for (;;) {
        ivr_openai_tts_job_t *job;

        ivr_mutex_lock(&tts->lock);
        while (!tts->job_pending && !tts->shutdown) {
            ivr_cond_wait(&tts->cond, &tts->lock);
        }
        if (tts->shutdown && !tts->job_pending) {
            ivr_mutex_unlock(&tts->lock);
            atomic_fetch_sub_explicit(&g_live_provider_threads, 1u,
                                      memory_order_relaxed);
            return NULL;
        }
        job = tts->job;
        tts->job = NULL;
        tts->job_pending = 0;
        tts->worker_running = 1;
        ivr_mutex_unlock(&tts->lock);

        if (job) {
            uint64_t started_at_ms = salts_monotonic_ms();
            ivr_openai_tts_run_job(tts, job);
            ivr_openai_observe_request(tts->config, IVR_OPENAI_REQUEST_TTS,
                                       started_at_ms);
            ivr_openai_tts_job_free(job);
        }

        ivr_mutex_lock(&tts->lock);
        tts->worker_running = 0;
        ivr_cond_broadcast(&tts->cond);
        ivr_mutex_unlock(&tts->lock);
    }
}

static int ivr_openai_tts_synthesize(void *context,
                                     const turbo_tts_request_t *request,
                                     const turbo_tts_provider_callbacks_t *callbacks,
                                     void *callback_user_data) {
    ivr_openai_tts_t *tts = (ivr_openai_tts_t *)context;
    ivr_openai_tts_job_t *job;
    size_t input_cap;

    if (!tts || !request || (!request->text && request->text_len > 0) ||
        !callbacks || !callbacks->on_audio) {
        return TURBO_SPEECH_ERR_INVALID;
    }
    input_cap = tts->config->max_tts_input_bytes
                    ? tts->config->max_tts_input_bytes
                    : IVR_OPENAI_DEFAULT_MAX_TTS_INPUT;
    if (request->text_len > input_cap || request->text_len == SIZE_MAX) {
        return TURBO_SPEECH_ERR_BUSY;
    }
    job = (ivr_openai_tts_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        return TURBO_SPEECH_ERR_NOMEM;
    }
    job->text = (char *)malloc(request->text_len + 1);
    if (!job->text) {
        free(job);
        return TURBO_SPEECH_ERR_NOMEM;
    }
    memcpy(job->text, request->text, request->text_len);
    job->text[request->text_len] = '\0';
    job->text_len = request->text_len;
    job->retained_bytes = request->text_len + 1u;
    ivr_openai_retain_input(job->retained_bytes);
    job->rate = request->rate;
    job->tts = tts;
    job->callbacks = *callbacks;
    job->callback_user_data = callback_user_data;

    ivr_mutex_lock(&tts->lock);
    if (tts->job_pending || tts->worker_running) {
        ivr_mutex_unlock(&tts->lock);
        ivr_openai_tts_job_free(job);
        return TURBO_SPEECH_ERR_BUSY;
    }
    atomic_store(&tts->cancel_requested, 0);
    tts->job = job;
    tts->job_pending = 1;
    tts->synthesized++;
    ivr_cond_signal(&tts->cond);
    ivr_mutex_unlock(&tts->lock);
    return TURBO_SPEECH_OK;
}

static int ivr_openai_tts_cancel(void *context) {
    ivr_openai_tts_t *tts = (ivr_openai_tts_t *)context;

    if (!tts) {
        return TURBO_SPEECH_ERR_INVALID;
    }
    atomic_store(&tts->cancel_requested, 1);
    ivr_mutex_lock(&tts->lock);
    /* A queued-but-not-started job must not run after cancel returns (the
       session may be freed right after the quiescence barrier). */
    if (tts->job_pending && !tts->worker_running) {
        ivr_openai_tts_job_t *job = tts->job;
        tts->job = NULL;
        tts->job_pending = 0;
        if (job) {
            ivr_openai_tts_job_free(job);
        }
    }
    while (tts->worker_running) {
        ivr_cond_wait(&tts->cond, &tts->lock);
    }
    ivr_mutex_unlock(&tts->lock);
    return TURBO_SPEECH_OK;
}

static void ivr_openai_tts_destroy(void *context) {
    ivr_openai_tts_t *tts = (ivr_openai_tts_t *)context;

    if (!tts) {
        return;
    }
    /* Quiesce only: the wrapper is app-owned and may be reused across
       sessions, so the persistent worker thread is kept alive. */
    (void)ivr_openai_tts_cancel(tts);
}
/* ------------------------------------------------------------------ */
/* ASR wrapper                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *pcm;
    size_t pcm_len;
    size_t pcm_capacity;
    turbo_speech_audio_format_t format;
    uint64_t start_time_us;
    turbo_asr_provider_callbacks_t callbacks;
    void *callback_user_data;
} ivr_openai_asr_job_t;

static void ivr_openai_asr_job_free(ivr_openai_asr_job_t *job) {
    if (!job) {
        return;
    }
    free(job->pcm);
    ivr_openai_release_input(job->pcm_capacity);
    free(job);
}

struct ivr_openai_asr {
    const ivr_openai_config_t *config;
    ivr_mutex_t lock;
    ivr_cond_t cond;
    ivr_thread_t thread;
    int thread_started;
    int shutdown;
    int finish_pending;
    ivr_openai_asr_job_t *job; /* owned by the worker once dequeued */
    int worker_running;
    atomic_int cancel_requested;
    /* session state (write() may race with finish(), so it is protected by
       the lock) */
    int running;
    turbo_asr_provider_callbacks_t callbacks;
    void *callback_user_data;
    turbo_speech_audio_format_t format;
    uint8_t *buffer;
    size_t buffer_len;
    size_t buffer_cap;
    /* diagnostics */
    uint64_t sessions;
    uint64_t rejected_bytes;
    uint64_t errors;
};

static void ivr_openai_asr_run_job(ivr_openai_asr_t *asr,
                                   ivr_openai_asr_job_t *job) {
    const ivr_openai_config_t *config = asr->config;
    http_client_t *client = NULL;
    http_response_t *response = NULL;
    char error_msg[IVR_OPENAI_ERROR_MESSAGE_MAX];
    uint8_t *wav = NULL;
    size_t wav_len = 0;
    char *body = NULL;
    size_t body_len = 0;
    char *content_type = NULL;
    char *transcript = NULL;
    const char *model;
    const char *path;

    if (atomic_load(&asr->cancel_requested)) {
        return;
    }
    model = config->asr_model ? config->asr_model : IVR_OPENAI_DEFAULT_ASR_MODEL;
    path = config->asr_path ? config->asr_path : IVR_OPENAI_DEFAULT_ASR_PATH;
    if (ivr_openai_build_wav(job->pcm, job->pcm_len, job->format.sample_rate,
                             job->format.channels, job->format.bits_per_sample,
                             &wav, &wav_len) != 0) {
        if (!atomic_load(&asr->cancel_requested)) {
            asr->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_FORMAT,
                                    "ASR: WAV build failed",
                                    job->callback_user_data);
        }
        return;
    }
    client = ivr_openai_create_client(config);
    if (!client) {
        if (!atomic_load(&asr->cancel_requested)) {
            asr->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER,
                                    "ASR: http client create failed",
                                    job->callback_user_data);
        }
        free(wav);
        return;
    }
    if (ivr_openai_build_multipart(wav, wav_len, model,
                                   config->asr_language &&
                                           config->asr_language[0] != '\0'
                                       ? config->asr_language
                                       : NULL,
                                   &body, &body_len, &content_type) != 0) {
        if (!atomic_load(&asr->cancel_requested)) {
            asr->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_NOMEM,
                                    "ASR: multipart body build failed",
                                    job->callback_user_data);
        }
        http_client_destroy(client);
        free(wav);
        return;
    }
    http_client_set_default_header(client, "Content-Type", content_type);
    response = http_post(client, path, body, body_len);
    free(body);
    free(content_type);
    free(wav);
    if (!response) {
        if (!atomic_load(&asr->cancel_requested)) {
            asr->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER,
                                    "ASR: no HTTP response",
                                    job->callback_user_data);
        }
        http_client_destroy(client);
        return;
    }
    ivr_openai_retain_response(response);
    if (ivr_openai_response_error(response, error_msg, sizeof(error_msg)) != 0) {
        if (!atomic_load(&asr->cancel_requested)) {
            asr->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_PROVIDER, error_msg,
                                    job->callback_user_data);
        }
        ivr_openai_response_free(response);
        http_client_destroy(client);
        return;
    }
    transcript = ivr_openai_parse_transcript(response->body, response->body_len);
    ivr_openai_response_free(response);
    http_client_destroy(client);
    if (!transcript) {
        if (!atomic_load(&asr->cancel_requested)) {
            asr->errors++;
            job->callbacks.on_error(TURBO_SPEECH_ERR_FORMAT,
                                    "ASR: response has no text field",
                                    job->callback_user_data);
        }
        return;
    }
    if (!atomic_load(&asr->cancel_requested)) {
        turbo_asr_result_t result;

        memset(&result, 0, sizeof(result));
        result.text = transcript;
        result.text_len = strlen(transcript);
        result.start_time_us = job->start_time_us;
        result.end_time_us = 0;
        result.confidence = 1.0f;
        result.is_final = 1;
        job->callbacks.on_result(&result, job->callback_user_data);
        job->callbacks.on_complete(job->callback_user_data);
    }
    free(transcript);
}

static void *ivr_openai_asr_thread(void *opaque) {
    ivr_openai_asr_t *asr = (ivr_openai_asr_t *)opaque;

    atomic_fetch_add_explicit(&g_live_provider_threads, 1u,
                              memory_order_relaxed);

    for (;;) {
        ivr_openai_asr_job_t *job;

        ivr_mutex_lock(&asr->lock);
        while (!asr->finish_pending && !asr->shutdown) {
            ivr_cond_wait(&asr->cond, &asr->lock);
        }
        if (asr->shutdown && !asr->finish_pending) {
            ivr_mutex_unlock(&asr->lock);
            atomic_fetch_sub_explicit(&g_live_provider_threads, 1u,
                                      memory_order_relaxed);
            return NULL;
        }
        job = asr->job;
        asr->job = NULL;
        asr->finish_pending = 0;
        asr->worker_running = 1;
        ivr_mutex_unlock(&asr->lock);

        if (job) {
            uint64_t started_at_ms = salts_monotonic_ms();
            ivr_openai_asr_run_job(asr, job);
            ivr_openai_observe_request(asr->config, IVR_OPENAI_REQUEST_ASR,
                                       started_at_ms);
            ivr_openai_asr_job_free(job);
        }

        ivr_mutex_lock(&asr->lock);
        asr->worker_running = 0;
        ivr_cond_broadcast(&asr->cond);
        ivr_mutex_unlock(&asr->lock);
    }
}
static int ivr_openai_asr_start(void *context, const turbo_asr_config_t *config,
                                const turbo_asr_provider_callbacks_t *callbacks,
                                void *callback_user_data) {
    ivr_openai_asr_t *asr = (ivr_openai_asr_t *)context;

    if (!asr || !config || !callbacks) {
        return TURBO_SPEECH_ERR_INVALID;
    }
    ivr_mutex_lock(&asr->lock);
    if (asr->running) {
        ivr_mutex_unlock(&asr->lock);
        return TURBO_SPEECH_ERR_STATE;
    }
    free(asr->buffer);
    ivr_openai_release_input(asr->buffer_cap);
    asr->buffer = NULL;
    asr->buffer_len = 0;
    asr->buffer_cap = 0;
    asr->format = config->format;
    asr->callbacks = *callbacks;
    asr->callback_user_data = callback_user_data;
    atomic_store(&asr->cancel_requested, 0);
    asr->running = 1;
    asr->sessions++;
    ivr_mutex_unlock(&asr->lock);
    return TURBO_SPEECH_OK;
}

static int ivr_openai_asr_write(void *context,
                                const turbo_speech_audio_frame_t *frame) {
    ivr_openai_asr_t *asr = (ivr_openai_asr_t *)context;
    size_t cap;

    if (!asr || !frame || !frame->data) {
        return TURBO_SPEECH_ERR_INVALID;
    }
    ivr_mutex_lock(&asr->lock);
    if (!asr->running) {
        ivr_mutex_unlock(&asr->lock);
        return TURBO_SPEECH_ERR_STATE;
    }
    cap = asr->config->max_asr_buffer_bytes
              ? asr->config->max_asr_buffer_bytes
              : IVR_OPENAI_DEFAULT_MAX_ASR_BUFFER;
    if (frame->len > cap || asr->buffer_len > cap - frame->len) {
        asr->rejected_bytes += frame->len;
        ivr_mutex_unlock(&asr->lock);
        return TURBO_SPEECH_ERR_BUSY; /* resource cap: reject, never drop silently */
    }
    if (asr->buffer_len + frame->len > asr->buffer_cap) {
        size_t old_cap = asr->buffer_cap;
        size_t new_cap = old_cap ? old_cap : (cap < 4096u ? cap : 4096u);
        uint8_t *nb;

        while (new_cap < asr->buffer_len + frame->len) {
            if (new_cap > cap / 2u) {
                new_cap = cap;
                break;
            }
            new_cap *= 2u;
        }
        nb = (uint8_t *)realloc(asr->buffer, new_cap);
        if (!nb) {
            asr->rejected_bytes += frame->len;
            ivr_mutex_unlock(&asr->lock);
            return TURBO_SPEECH_ERR_NOMEM;
        }
        asr->buffer = nb;
        asr->buffer_cap = new_cap;
        ivr_openai_retain_input(new_cap - old_cap);
    }
    memcpy(asr->buffer + asr->buffer_len, frame->data, frame->len);
    asr->buffer_len += frame->len;
    ivr_mutex_unlock(&asr->lock);
    return TURBO_SPEECH_OK;
}

static int ivr_openai_asr_finish(void *context) {
    ivr_openai_asr_t *asr = (ivr_openai_asr_t *)context;
    ivr_openai_asr_job_t *job = NULL;

    if (!asr) {
        return TURBO_SPEECH_ERR_INVALID;
    }
    ivr_mutex_lock(&asr->lock);
    if (!asr->running) {
        ivr_mutex_unlock(&asr->lock);
        return TURBO_SPEECH_ERR_STATE;
    }
    if (asr->finish_pending || asr->worker_running) {
        ivr_mutex_unlock(&asr->lock);
        return TURBO_SPEECH_ERR_BUSY;
    }
    asr->running = 0;
    if (asr->buffer_len == 0) {
        /* Nothing captured: complete locally, no network round trip. */
        turbo_asr_provider_callbacks_t callbacks = asr->callbacks;
        void *user_data = asr->callback_user_data;
        ivr_mutex_unlock(&asr->lock);
        if (callbacks.on_complete && !atomic_load(&asr->cancel_requested)) {
            callbacks.on_complete(user_data);
        }
        return TURBO_SPEECH_OK;
    }
    job = (ivr_openai_asr_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        ivr_mutex_unlock(&asr->lock);
        return TURBO_SPEECH_ERR_NOMEM;
    }
    job->pcm = asr->buffer;
    job->pcm_len = asr->buffer_len;
    job->pcm_capacity = asr->buffer_cap;
    job->format = asr->format;
    job->start_time_us = 0;
    job->callbacks = asr->callbacks;
    job->callback_user_data = asr->callback_user_data;
    asr->buffer = NULL;
    asr->buffer_len = 0;
    asr->buffer_cap = 0;
    asr->job = job;
    asr->finish_pending = 1;
    ivr_cond_signal(&asr->cond);
    ivr_mutex_unlock(&asr->lock);
    return TURBO_SPEECH_OK;
}

static int ivr_openai_asr_cancel(void *context) {
    ivr_openai_asr_t *asr = (ivr_openai_asr_t *)context;

    if (!asr) {
        return TURBO_SPEECH_ERR_INVALID;
    }
    atomic_store(&asr->cancel_requested, 1);
    ivr_mutex_lock(&asr->lock);
    /* A queued-but-not-started job must not run after cancel returns. */
    if (asr->finish_pending && !asr->worker_running) {
        ivr_openai_asr_job_t *job = asr->job;
        asr->job = NULL;
        asr->finish_pending = 0;
        if (job) {
            ivr_openai_asr_job_free(job);
        }
    }
    while (asr->worker_running) {
        ivr_cond_wait(&asr->cond, &asr->lock);
    }
    free(asr->buffer);
    ivr_openai_release_input(asr->buffer_cap);
    asr->buffer = NULL;
    asr->buffer_len = 0;
    asr->buffer_cap = 0;
    asr->running = 0;
    ivr_mutex_unlock(&asr->lock);
    return TURBO_SPEECH_OK;
}

static void ivr_openai_asr_destroy(void *context) {
    ivr_openai_asr_t *asr = (ivr_openai_asr_t *)context;

    if (!asr) {
        return;
    }
    (void)ivr_openai_asr_cancel(asr);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int ivr_openai_tts_create(const ivr_openai_config_t *config,
                          ivr_openai_tts_t **out_tts) {
    ivr_openai_tts_t *tts;

    if (out_tts) {
        *out_tts = NULL;
    }
    if (!config || !config->base_url || !out_tts) {
        return -1;
    }
    tts = (ivr_openai_tts_t *)calloc(1, sizeof(*tts));
    if (!tts) {
        return -1;
    }
    tts->config = config;
    atomic_init(&tts->cancel_requested, 0);
    if (ivr_mutex_init(&tts->lock) != 0 || ivr_cond_init(&tts->cond) != 0) {
        if (tts->cond.handle) {
            ivr_cond_destroy(&tts->cond);
        }
        ivr_mutex_destroy(&tts->lock);
        free(tts);
        return -1;
    }
    if (ivr_thread_create(&tts->thread, ivr_openai_tts_thread, tts) != 0) {
        ivr_cond_destroy(&tts->cond);
        ivr_mutex_destroy(&tts->lock);
        free(tts);
        return -1;
    }
    tts->thread_started = 1;
    atomic_fetch_add_explicit(&g_active_tts_instances, 1u,
                              memory_order_relaxed);
    *out_tts = tts;
    return 0;
}

int ivr_openai_asr_create(const ivr_openai_config_t *config,
                          ivr_openai_asr_t **out_asr) {
    ivr_openai_asr_t *asr;

    if (out_asr) {
        *out_asr = NULL;
    }
    if (!config || !config->base_url || !out_asr) {
        return -1;
    }
    asr = (ivr_openai_asr_t *)calloc(1, sizeof(*asr));
    if (!asr) {
        return -1;
    }
    asr->config = config;
    atomic_init(&asr->cancel_requested, 0);
    if (ivr_mutex_init(&asr->lock) != 0 || ivr_cond_init(&asr->cond) != 0) {
        if (asr->cond.handle) {
            ivr_cond_destroy(&asr->cond);
        }
        ivr_mutex_destroy(&asr->lock);
        free(asr);
        return -1;
    }
    if (ivr_thread_create(&asr->thread, ivr_openai_asr_thread, asr) != 0) {
        ivr_cond_destroy(&asr->cond);
        ivr_mutex_destroy(&asr->lock);
        free(asr);
        return -1;
    }
    asr->thread_started = 1;
    atomic_fetch_add_explicit(&g_active_asr_instances, 1u,
                              memory_order_relaxed);
    *out_asr = asr;
    return 0;
}

void ivr_openai_tts_get_provider(ivr_openai_tts_t *tts,
                                 turbo_tts_provider_t *out_provider) {
    if (!tts || !out_provider) {
        return;
    }
    memset(out_provider, 0, sizeof(*out_provider));
    out_provider->abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION;
    out_provider->context = tts;
    out_provider->synthesize = ivr_openai_tts_synthesize;
    out_provider->cancel = ivr_openai_tts_cancel;
    out_provider->destroy = ivr_openai_tts_destroy;
}

void ivr_openai_asr_get_provider(ivr_openai_asr_t *asr,
                                 turbo_asr_provider_t *out_provider) {
    if (!asr || !out_provider) {
        return;
    }
    memset(out_provider, 0, sizeof(*out_provider));
    out_provider->abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION;
    out_provider->context = asr;
    out_provider->start = ivr_openai_asr_start;
    out_provider->write = ivr_openai_asr_write;
    out_provider->finish = ivr_openai_asr_finish;
    out_provider->cancel = ivr_openai_asr_cancel;
    out_provider->destroy = ivr_openai_asr_destroy;
}

void ivr_openai_tts_free(ivr_openai_tts_t *tts) {
    if (!tts) {
        return;
    }
    atomic_store(&tts->cancel_requested, 1);
    ivr_mutex_lock(&tts->lock);
    if (tts->job_pending) {
        ivr_openai_tts_job_t *job = tts->job;
        tts->job = NULL;
        tts->job_pending = 0;
        if (job) {
            ivr_openai_tts_job_free(job);
        }
    }
    tts->shutdown = 1;
    ivr_cond_broadcast(&tts->cond);
    ivr_mutex_unlock(&tts->lock);
    if (tts->thread_started) {
        ivr_thread_join(&tts->thread);
        tts->thread_started = 0;
    }
    ivr_cond_destroy(&tts->cond);
    ivr_mutex_destroy(&tts->lock);
    atomic_fetch_sub_explicit(&g_active_tts_instances, 1u,
                              memory_order_relaxed);
    free(tts);
}

void ivr_openai_asr_free(ivr_openai_asr_t *asr) {
    if (!asr) {
        return;
    }
    atomic_store(&asr->cancel_requested, 1);
    ivr_mutex_lock(&asr->lock);
    if (asr->finish_pending) {
        ivr_openai_asr_job_t *job = asr->job;
        asr->job = NULL;
        asr->finish_pending = 0;
        if (job) {
            ivr_openai_asr_job_free(job);
        }
    }
    free(asr->buffer);
    ivr_openai_release_input(asr->buffer_cap);
    asr->buffer = NULL;
    asr->buffer_len = 0;
    asr->buffer_cap = 0;
    asr->shutdown = 1;
    ivr_cond_broadcast(&asr->cond);
    ivr_mutex_unlock(&asr->lock);
    if (asr->thread_started) {
        ivr_thread_join(&asr->thread);
        asr->thread_started = 0;
    }
    ivr_cond_destroy(&asr->cond);
    ivr_mutex_destroy(&asr->lock);
    atomic_fetch_sub_explicit(&g_active_asr_instances, 1u,
                              memory_order_relaxed);
    free(asr);
}

void ivr_openai_get_resource_snapshot(
    ivr_openai_resource_snapshot_t *out_snapshot) {
    if (!out_snapshot) {
        return;
    }
    out_snapshot->active_tts_instances = atomic_load_explicit(
        &g_active_tts_instances, memory_order_relaxed);
    out_snapshot->active_asr_instances = atomic_load_explicit(
        &g_active_asr_instances, memory_order_relaxed);
    out_snapshot->live_provider_threads = atomic_load_explicit(
        &g_live_provider_threads, memory_order_relaxed);
    out_snapshot->retained_input_bytes = atomic_load_explicit(
        &g_retained_input_bytes, memory_order_relaxed);
    out_snapshot->retained_response_bytes = atomic_load_explicit(
        &g_retained_response_bytes, memory_order_relaxed);
}
