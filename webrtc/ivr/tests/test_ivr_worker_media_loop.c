/* test_ivr_worker_media_loop.c - Full worker -> session -> media-bot loop.
 *
 * Real ivr_worker + per-call TurboXML session + real ivr_media_bot (mock
 * TTS/ASR providers + loopback transport) as the media port. A
 * connection.alerting event starts the CCXML/VXML conference menu; the prompt
 * is synthesized through TTS and its PCM frames reach the transport; caller
 * PCM fed from the transport is recognized by ASR and the asr.final ("1")
 * routes back into the session's collect_input, which maps it to the
 * conference.join command on the shadow gateway. */
#include "ivr/ivr_worker.h"
#include "ivr_dtmf_rtp.h"
#include "ivr_media_bot.h"
#include "ivr_session.h"
#include "ivr_thread.h"
#include "tinytest_compat.h"
#include "turbo_speech.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef IVR_TEST_CONTENT_ROOT
#define IVR_TEST_CONTENT_ROOT "content"
#endif

static const ivr_call_ref_t g_call = {
    {"room-42", 7}, {"call-42", 7}, 1, 1};
static ivr_dtmf_ingress_t *g_dtmf_ingress = NULL;

/* ---- mock TTS: async-style, emits frames, stays RUNNING ---- */
typedef struct {
    const turbo_tts_provider_callbacks_t *callbacks;
    void *callback_user_data;
    int synthesize_count;
    int cancel_count;
    int destroy_count;
} mock_tts_t;

static int mock_tts_synthesize(void *context, const turbo_tts_request_t *request,
                               const turbo_tts_provider_callbacks_t *callbacks,
                               void *callback_user_data) {
    mock_tts_t *mock = (mock_tts_t *)context;
    (void)request;
    mock->callbacks = callbacks;
    mock->callback_user_data = callback_user_data;
    mock->synthesize_count++;
    static const uint16_t samples[16] = {
        100, 200, 300, 400, 500, 600, 700, 800,
        900, 800, 700, 600, 500, 400, 300, 200};
    turbo_speech_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.format.sample_rate = 16000;
    frame.format.channels = 1;
    frame.format.bits_per_sample = 16;
    frame.data = (const uint8_t *)samples;
    for (int i = 0; i < 2; i++) {
        frame.len = sizeof(samples);
        frame.timestamp_us = (uint64_t)i * 1000u;
        if (callbacks->on_audio(&frame, callback_user_data) != 0) {
            break;
        }
    }
    return TURBO_SPEECH_OK;
}
static int mock_tts_cancel(void *context) {
    mock_tts_t *mock = (mock_tts_t *)context;
    mock->cancel_count++;
    return TURBO_SPEECH_OK;
}
static void mock_tts_destroy(void *context) {
    mock_tts_t *mock = (mock_tts_t *)context;
    mock->destroy_count++;
}

/* ---- mock ASR: final "1" on the 3rd write ---- */
typedef struct {
    const turbo_asr_provider_callbacks_t *callbacks;
    void *callback_user_data;
    int start_count;
    int write_count;
    int cancel_count;
    int destroy_count;
} mock_asr_t;

static int mock_asr_start(void *context, const turbo_asr_config_t *config,
                          const turbo_asr_provider_callbacks_t *callbacks,
                          void *callback_user_data) {
    mock_asr_t *mock = (mock_asr_t *)context;
    (void)config;
    mock->callbacks = callbacks;
    mock->callback_user_data = callback_user_data;
    mock->start_count++;
    return TURBO_SPEECH_OK;
}
static int mock_asr_write(void *context, const turbo_speech_audio_frame_t *frame) {
    mock_asr_t *mock = (mock_asr_t *)context;
    (void)frame;
    mock->write_count++;
    if (mock->write_count >= 3) {
        static const char text[] = "1";
        turbo_asr_result_t result = {.text = text,
                                     .text_len = 1U,
                                     .start_time_us = 0,
                                     .end_time_us = 1000,
                                     .confidence = 0.9f,
                                     .is_final = 1};
        mock->callbacks->on_result(&result, mock->callback_user_data);
    }
    return TURBO_SPEECH_OK;
}
static int mock_asr_finish(void *context) {
    (void)context;
    return TURBO_SPEECH_OK;
}
static int mock_asr_cancel(void *context) {
    mock_asr_t *mock = (mock_asr_t *)context;
    mock->cancel_count++;
    return TURBO_SPEECH_OK;
}
static void mock_asr_destroy(void *context) {
    mock_asr_t *mock = (mock_asr_t *)context;
    mock->destroy_count++;
}

/* ---- loopback transport ---- */
typedef struct {
    ivr_mutex_t lock;
    int frames;
    size_t bytes;
    int stops;
} loopback_t;

static int loop_play_audio(void *ctx, const ivr_call_ref_t *call,
                           const uint8_t *pcm, size_t len,
                           uint32_t sample_rate) {
    loopback_t *loop = (loopback_t *)ctx;
    (void)call;
    (void)pcm;
    (void)sample_rate;
    ivr_mutex_lock(&loop->lock);
    loop->frames++;
    loop->bytes += len;
    ivr_mutex_unlock(&loop->lock);
    return 0;
}
static int loop_stop(void *ctx, const ivr_call_ref_t *call) {
    loopback_t *loop = (loopback_t *)ctx;
    (void)call;
    ivr_mutex_lock(&loop->lock);
    loop->stops++;
    ivr_mutex_unlock(&loop->lock);
    return 0;
}

static ivr_status_t loop_begin_input(void *ctx, const ivr_call_ref_t *call,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    ivr_dtmf_ingress_t *ingress = g_dtmf_ingress;
    (void)ctx;
    char id[IVR_DTMF_ID_MAX];
    if (!ingress || !input_id || !input_id->data || input_id->size == 0 ||
        input_id->size >= sizeof(id)) {
        return IVR_EINVAL;
    }
    memcpy(id, input_id->data, input_id->size);
    id[input_id->size] = '\0';
    return ivr_dtmf_ingress_begin_input(ingress, call, id,
                                        input_generation);
}
static ivr_status_t loop_end_input(void *ctx, const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *input_id,
                                   uint64_t input_generation) {
    (void)ctx;
    (void)input_id;
    return ivr_dtmf_ingress_end_input(g_dtmf_ingress, call,
                                      input_generation);
}

/* ---- shadow command gateway (capture, never send) ---- */
static ivr_mutex_t g_cmd_lock;
static char g_cmds[8][64];
static int g_cmd_count;
static uint64_t g_trace_sequence;
static uint64_t g_cancel_sequence;
static uint64_t g_command_sequence;
static uint64_t g_observer_sequence;
static int g_observer_count;
static ivr_session_latency_kind_t g_observer_kind;
static uint64_t g_observer_duration_ms;
static ivr_status_t (*g_bot_cancel_input)(void *ctx,
                                          const ivr_call_ref_t *call);

static ivr_status_t shadow_submit(void *ctx, const ivr_command_view_t *cmd) {
    (void)ctx;
    ivr_mutex_lock(&g_cmd_lock);
    if (g_cmd_count < 8) {
        int n = (int)cmd->command_type.size;
        if (n > 63) {
            n = 63;
        }
        memcpy(g_cmds[g_cmd_count], cmd->command_type.data, (size_t)n);
        g_cmds[g_cmd_count][n] = '\0';
        if (strcmp(g_cmds[g_cmd_count], "conference.join") == 0 ||
            strcmp(g_cmds[g_cmd_count], "conference.leave") == 0) {
            g_command_sequence = ++g_trace_sequence;
        }
        g_cmd_count++;
    }
    ivr_mutex_unlock(&g_cmd_lock);
    return IVR_OK;
}

static ivr_status_t trace_cancel_input(void *ctx,
                                       const ivr_call_ref_t *call) {
    ivr_status_t status = g_bot_cancel_input(ctx, call);
    if (status == IVR_OK) {
        ivr_mutex_lock(&g_cmd_lock);
        g_cancel_sequence = ++g_trace_sequence;
        ivr_mutex_unlock(&g_cmd_lock);
    }
    return status;
}

static void observe_session_latency(void *ctx,
                                    ivr_session_latency_kind_t kind,
                                    uint64_t duration_ms) {
    (void)ctx;
    ivr_mutex_lock(&g_cmd_lock);
    g_observer_count++;
    g_observer_kind = kind;
    g_observer_duration_ms = duration_ms;
    g_observer_sequence = ++g_trace_sequence;
    ivr_mutex_unlock(&g_cmd_lock);
}

/* ---- fixtures ---- */
static mock_tts_t g_tts;
static mock_asr_t g_asr;
static loopback_t g_loop;
static turbo_tts_provider_t g_tts_provider;
static turbo_asr_provider_t g_asr_provider;
static ivr_media_transport_t g_transport;
static ivr_media_bot_t *g_bot = NULL;
static ivr_media_port_ops_t g_media_ops;
static ivr_media_port_factory_ops_t g_media_factory;
static ivr_worker_t *g_worker = NULL;
static ivr_session_t *g_session = NULL;

static void bot_on_event(void *ctx, const ivr_event_view_t *event) {
    (void)ctx;
    (void)ivr_worker_submit_event_copy(g_worker, event);
}

static ivr_status_t media_factory_create(void *ctx,
                                         const ivr_call_ref_t *call,
                                         ivr_media_port_ops_t *out_media,
                                         void **out_instance) {
    (void)call;
    if (!ctx || !out_media || !out_instance) {
        return IVR_EINVAL;
    }
    *out_media = g_media_ops;
    *out_instance = ctx;
    return IVR_OK;
}

static void media_factory_destroy(void *ctx, void *instance) {
    (void)ctx;
    (void)instance;
}

static int wait_frames(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_mutex_lock(&g_loop.lock);
        int frames = g_loop.frames;
        ivr_mutex_unlock(&g_loop.lock);
        if (frames > 0) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

static int wait_command(const char *expected, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_mutex_lock(&g_cmd_lock);
        int found = 0;
        for (int j = 0; j < g_cmd_count; j++) {
            if (strcmp(g_cmds[j], expected) == 0) {
                found = 1;
                break;
            }
        }
        ivr_mutex_unlock(&g_cmd_lock);
        if (found) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

static int wait_observation(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; ++i) {
        int count;
        ivr_mutex_lock(&g_cmd_lock);
        count = g_observer_count;
        ivr_mutex_unlock(&g_cmd_lock);
        if (count > 0) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
    }
    return 0;
}

static int business_command_count(void) {
    int count = 0;
    ivr_mutex_lock(&g_cmd_lock);
    for (int i = 0; i < g_cmd_count; ++i) {
        if (strcmp(g_cmds[i], "conference.join") == 0 ||
            strcmp(g_cmds[i], "conference.leave") == 0) {
            count++;
        }
    }
    ivr_mutex_unlock(&g_cmd_lock);
    return count;
}

static int wait_input_window(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; ++i) {
        int active;
        ivr_mutex_lock(&g_session->input_mutex);
        active = g_session->input_waiting &&
                 g_session->input_window_id.size > 0;
        ivr_mutex_unlock(&g_session->input_mutex);
        if (active) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
    }
    return 0;
}

static ivr_status_t route_dtmf_packet(void) {
    static const uint8_t payload[] = {2, 0x80u, 0x00u, 0xa0u};
    static const ivr_bytes_view_t event_type = {"dtmf.final", 10};
    ivr_dtmf_input_t input;
    ivr_event_view_t event;
    ivr_call_ref_t call;
    char digit[2];
    char payload_json[160];
    int length;
    ivr_status_t status = ivr_dtmf_ingress_submit_rtp(
        g_dtmf_ingress, &g_call, 1, 9000, payload, sizeof(payload), &input);
    if (status != IVR_OK) {
        return status;
    }
    length = snprintf(payload_json, sizeof(payload_json),
                      "{\"digit\":\"%c\",\"duration\":%u,"
                      "\"source_generation\":%llu}",
                      input.digit, (unsigned)input.duration,
                      (unsigned long long)input.source_generation);
    if (length < 0 || (size_t)length >= sizeof(payload_json)) {
        return IVR_ENOSPC;
    }
    memset(&call, 0, sizeof(call));
    call.room_id.data = input.room_id;
    call.room_id.size = strlen(input.room_id);
    call.call_id.data = input.call_id;
    call.call_id.size = strlen(input.call_id);
    call.call_generation = input.call_generation;
    digit[0] = input.digit;
    digit[1] = '\0';
    memset(&event, 0, sizeof(event));
    event.event_type = event_type;
    event.call = call;
    event.payload_json.data = payload_json;
    event.payload_json.size = (size_t)length;
    event.input_id.data = input.input_id;
    event.input_id.size = strlen(input.input_id);
    event.input_value.data = digit;
    event.input_value.size = 1;
    return ivr_worker_submit_event_copy(g_worker, &event);
}

void setUp(void) {
    ivr_dtmf_ingress_config_t dtmf_config;
    memset(&g_tts, 0, sizeof(g_tts));
    memset(&g_asr, 0, sizeof(g_asr));
    memset(&g_loop, 0, sizeof(g_loop));
    ivr_mutex_init(&g_loop.lock);
    ivr_mutex_init(&g_cmd_lock);
    g_cmd_count = 0;
    g_trace_sequence = 0;
    g_cancel_sequence = 0;
    g_command_sequence = 0;
    g_observer_sequence = 0;
    g_observer_count = 0;
    g_observer_kind = IVR_SESSION_LATENCY_DTMF_TO_CANCEL;
    g_observer_duration_ms = 0;
    memset(&dtmf_config, 0, sizeof(dtmf_config));
    dtmf_config.window_capacity = 2;
    dtmf_config.max_duration = 8000;
    TEST_ASSERT_EQUAL(
        IVR_OK,
        ivr_dtmf_ingress_create(&dtmf_config, &g_dtmf_ingress));

    g_tts_provider.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION;
    g_tts_provider.context = &g_tts;
    g_tts_provider.synthesize = mock_tts_synthesize;
    g_tts_provider.cancel = mock_tts_cancel;
    g_tts_provider.destroy = mock_tts_destroy;
    g_asr_provider.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION;
    g_asr_provider.context = &g_asr;
    g_asr_provider.start = mock_asr_start;
    g_asr_provider.write = mock_asr_write;
    g_asr_provider.finish = mock_asr_finish;
    g_asr_provider.cancel = mock_asr_cancel;
    g_asr_provider.destroy = mock_asr_destroy;
    g_transport.context = &g_loop;
    g_transport.play_audio = loop_play_audio;
    g_transport.stop = loop_stop;

    ivr_media_bot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tts_provider = &g_tts_provider;
    cfg.asr_provider = &g_asr_provider;
    cfg.transport = g_transport;
    cfg.on_event = bot_on_event;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_media_bot_create(&cfg, &g_bot));
    ivr_media_bot_get_ops(g_bot, &g_media_ops);
    g_bot_cancel_input = g_media_ops.cancel_input;
    g_media_ops.cancel_input = trace_cancel_input;
    g_media_ops.begin_input = loop_begin_input;
    g_media_ops.end_input = loop_end_input;
    memset(&g_media_factory, 0, sizeof(g_media_factory));
    g_media_factory.abi_version = IVR_WORKER_ABI_VERSION;
    g_media_factory.context = g_bot;
    g_media_factory.create = media_factory_create;
    g_media_factory.destroy = media_factory_destroy;

    ivr_command_gateway_ops_t shadow_ops;
    memset(&shadow_ops, 0, sizeof(shadow_ops));
    shadow_ops.abi_version = 1;
    shadow_ops.context = NULL;
    shadow_ops.submit_copy = shadow_submit;

    ivr_worker_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.abi_version = IVR_WORKER_ABI_VERSION;
    wcfg.worker_id = "ivr-worker-test";
    wcfg.max_sessions_per_worker = 1;
    wcfg.session_inbox_capacity = 8;
    wcfg.max_event_bytes = 65536;
    wcfg.max_command_bytes = 16384;
    wcfg.content_root = IVR_TEST_CONTENT_ROOT;
    wcfg.drain_deadline_ms = 5000;
    wcfg.observer.abi_version = IVR_SESSION_OBSERVER_ABI_VERSION;
    wcfg.observer.on_latency = observe_session_latency;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_create(&wcfg, &shadow_ops, &g_media_factory,
                                        &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting",
                                                &g_session));
    TEST_ASSERT_NOT_NULL(g_session);
    /* the app opens the bot peer for this call (the media transport join) */
    TEST_ASSERT_EQUAL(IVR_OK, g_media_ops.start_bot(g_media_ops.context,
                                                    &g_call));

    /* drive the session through the happy-path sequence so the CCXML/VXML
       conference menu starts and plays its prompt */
    static const ivr_bytes_view_t payload = {"{}", 2};
    static const struct {
        const char *name;
        uint64_t seq;
    } seqs[] = {{"room.assigned", 1}, {"rtc.connected", 2},
                {"connection.alerting", 3}};
    for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        static ivr_bytes_view_t type = {"", 0};
        type.data = seqs[i].name;
        type.size = strlen(seqs[i].name);
        ivr_event_view_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event_type = type;
        ev.call = g_call;
        ev.sequence = seqs[i].seq;
        ev.payload_json = payload;
        TEST_ASSERT_EQUAL(IVR_OK,
                          ivr_worker_submit_event_copy(g_worker, &ev));
    }
    /* the menu prompt is synthesized through TTS and its PCM reaches the
       transport before collect_input blocks */
    TEST_ASSERT_TRUE(wait_frames(8000));
}

void tearDown(void) {
    if (g_worker) {
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        g_worker = NULL;
    }
    if (g_bot) {
        ivr_media_bot_destroy(g_bot);
        g_bot = NULL;
    }
    ivr_dtmf_ingress_destroy(g_dtmf_ingress);
    g_dtmf_ingress = NULL;
    ivr_mutex_destroy(&g_loop.lock);
    ivr_mutex_destroy(&g_cmd_lock);
}

void test_media_loop_prompt_and_command(void) {
    /* the prompt already reached the transport (setUp) */
    ivr_mutex_lock(&g_loop.lock);
    int frames = g_loop.frames;
    size_t bytes = g_loop.bytes;
    ivr_mutex_unlock(&g_loop.lock);
    TEST_ASSERT_TRUE(frames > 0);
    TEST_ASSERT_TRUE(bytes > 0);
    TEST_ASSERT_EQUAL_INT(1, g_tts.synthesize_count);

    /* transport receive side: feed caller PCM; ASR emits final "1" on the
       3rd write -> asr.final routes back into the session's collect_input,
       which maps "1" to conference.join on the shadow gateway */
    static const uint8_t pcm[64] = {0};
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(IVR_OK,
                          ivr_media_bot_feed_caller_audio(g_bot, &g_call, pcm,
                                                          sizeof(pcm)));
    }
    TEST_ASSERT_TRUE(wait_command("conference.join", 8000));
    TEST_ASSERT_TRUE(wait_observation(8000));
    TEST_ASSERT_EQUAL_INT(3, g_asr.write_count);
    TEST_ASSERT_EQUAL_INT(1, g_asr.start_count);
    ivr_mutex_lock(&g_cmd_lock);
    TEST_ASSERT_EQUAL_INT(1, g_observer_count);
    TEST_ASSERT_EQUAL_INT(IVR_SESSION_LATENCY_ASR_TO_COMMAND, g_observer_kind);
    TEST_ASSERT_TRUE(g_cancel_sequence < g_command_sequence);
    TEST_ASSERT_TRUE(g_command_sequence < g_observer_sequence);
    (void)g_observer_duration_ms;
    ivr_mutex_unlock(&g_cmd_lock);
}

void test_rtp_dtmf_observes_cancel_before_business_command(void) {
    TEST_ASSERT_TRUE(wait_input_window(8000));
    TEST_ASSERT_EQUAL(IVR_OK, route_dtmf_packet());
    TEST_ASSERT_TRUE(wait_command("conference.leave", 8000));
    TEST_ASSERT_TRUE(wait_observation(8000));
    ivr_mutex_lock(&g_cmd_lock);
    TEST_ASSERT_EQUAL_INT(1, g_observer_count);
    TEST_ASSERT_EQUAL_INT(IVR_SESSION_LATENCY_DTMF_TO_CANCEL, g_observer_kind);
    TEST_ASSERT_TRUE(g_cancel_sequence < g_observer_sequence);
    TEST_ASSERT_TRUE(g_observer_sequence < g_command_sequence);
    ivr_mutex_unlock(&g_cmd_lock);
}

typedef struct {
    atomic_int start;
    ivr_status_t dtmf_status;
    ivr_status_t asr_status;
} input_race_t;

static void *dtmf_race_thread(void *opaque) {
    input_race_t *race = (input_race_t *)opaque;
    while (!atomic_load_explicit(&race->start, memory_order_acquire)) {
        ivr_thread_sleep_ms(1);
    }
    race->dtmf_status = route_dtmf_packet();
    return NULL;
}

static void *asr_race_thread(void *opaque) {
    input_race_t *race = (input_race_t *)opaque;
    static const uint8_t pcm[64] = {0};
    race->asr_status = IVR_OK;
    while (!atomic_load_explicit(&race->start, memory_order_acquire)) {
        ivr_thread_sleep_ms(1);
    }
    for (int i = 0; i < 3; ++i) {
        ivr_status_t status = ivr_media_bot_feed_caller_audio(
            g_bot, &g_call, pcm, sizeof(pcm));
        if (status != IVR_OK) {
            race->asr_status = status;
            break;
        }
    }
    return NULL;
}

void test_rtp_dtmf_and_asr_first_final_wins(void) {
    input_race_t race;
    ivr_thread_t dtmf_thread;
    ivr_thread_t asr_thread;
    memset(&race, 0, sizeof(race));
    atomic_init(&race.start, 0);
    race.dtmf_status = IVR_ESTATE;
    race.asr_status = IVR_ESTATE;

    TEST_ASSERT_TRUE(wait_input_window(8000));
    TEST_ASSERT_EQUAL_INT(0, ivr_thread_create(&dtmf_thread,
                                               dtmf_race_thread, &race));
    TEST_ASSERT_EQUAL_INT(0, ivr_thread_create(&asr_thread,
                                               asr_race_thread, &race));
    atomic_store_explicit(&race.start, 1, memory_order_release);
    ivr_thread_join(&dtmf_thread);
    ivr_thread_join(&asr_thread);

    TEST_ASSERT_TRUE(race.asr_status == IVR_OK ||
                     race.asr_status == IVR_ESTATE);
    TEST_ASSERT_TRUE(race.dtmf_status == IVR_OK ||
                     race.dtmf_status == IVR_ESTATE);
    for (int i = 0; i < 400 && business_command_count() == 0; ++i) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL_INT(1, business_command_count());
    TEST_ASSERT_TRUE(wait_observation(8000));
    ivr_thread_sleep_ms(100);
    TEST_ASSERT_EQUAL_INT(1, business_command_count());
    ivr_mutex_lock(&g_cmd_lock);
    TEST_ASSERT_EQUAL_INT(1, g_observer_count);
    TEST_ASSERT_TRUE(g_cancel_sequence > 0);
    TEST_ASSERT_TRUE(g_command_sequence > 0);
    if (g_observer_kind == IVR_SESSION_LATENCY_DTMF_TO_CANCEL) {
        TEST_ASSERT_TRUE(g_cancel_sequence < g_observer_sequence);
        TEST_ASSERT_TRUE(g_observer_sequence < g_command_sequence);
    } else {
        TEST_ASSERT_EQUAL_INT(IVR_SESSION_LATENCY_ASR_TO_COMMAND,
                              g_observer_kind);
        TEST_ASSERT_TRUE(g_cancel_sequence < g_command_sequence);
        TEST_ASSERT_TRUE(g_command_sequence < g_observer_sequence);
    }
    ivr_mutex_unlock(&g_cmd_lock);
}

spec("test_ivr_worker_media_loop") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_media_loop_prompt_and_command);
  TT_TEST(test_rtp_dtmf_observes_cancel_before_business_command);
  TT_TEST(test_rtp_dtmf_and_asr_first_final_wins);
}
