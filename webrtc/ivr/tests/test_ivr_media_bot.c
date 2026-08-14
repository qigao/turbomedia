/* test_ivr_media_bot.c - Real IVR media-port (ivr_media_bot) over turbo_speech
 * with mock TTS/ASR providers and a loopback audio transport.
 *
 * Verifies: start_bot opens the peer, play_pcm synthesizes text and emits a
 * completion fact, explicit input windows correlate ASR finals, cancel/stop
 * quiesce and tear down the sessions, and missing providers fail fast. */
#include "ivr_media_bot.h"
#include "ivr_thread.h"
#include "tinytest_compat.h"
#include <string.h>

static const ivr_call_ref_t g_call = {
    .provider_session_id = {"session-42", 10},
    .dialog_id = {"dialog-42", 9},
    .room_id = {"room-42", 7},
    .call_id = {"call-42", 7},
    .call_generation = 1,
    .expected_room_version = 1};

/* ---- mock TTS provider ---- */
typedef struct {
    const turbo_tts_provider_callbacks_t *callbacks;
    void *callback_user_data;
    int synthesize_count;
    int cancel_count;
    int destroy_count;
    int synthesize_error;
    int complete_on_synthesize;
} mock_tts_t;

static int mock_tts_synthesize(void *context, const turbo_tts_request_t *request,
                               const turbo_tts_provider_callbacks_t *callbacks,
                               void *callback_user_data) {
    mock_tts_t *mock = (mock_tts_t *)context;
    mock->callbacks = callbacks;
    mock->callback_user_data = callback_user_data;
    mock->synthesize_count++;
    if (mock->synthesize_error != TURBO_SPEECH_OK) {
        callbacks->on_error(mock->synthesize_error, "tts failed",
                            callback_user_data);
        return TURBO_SPEECH_OK;
    }
    /* emit 3 frames of deterministic 16-bit mono PCM then complete */
    static const uint16_t samples[16] = {
        100, 200, 300, 400, 500, 600, 700, 800,
        900, 800, 700, 600, 500, 400, 300, 200};
    (void)request;
    turbo_speech_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.format.sample_rate = 16000;
    frame.format.channels = 1;
    frame.format.bits_per_sample = 16;
    frame.data = (const uint8_t *)samples;
    for (int i = 0; i < 3; i++) {
        frame.len = sizeof(samples);
        frame.timestamp_us = (uint64_t)i * 1000u;
        if (callbacks->on_audio(&frame, callback_user_data) != 0) {
            break;
        }
    }
    if (mock->complete_on_synthesize) {
        callbacks->on_complete(callback_user_data);
    }
    /* async-style provider: leave the session RUNNING until cancel/destroy
       so the TTS cancel path is exercisable */
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

static turbo_tts_provider_t make_tts_provider(mock_tts_t *mock) {
    turbo_tts_provider_t provider = {
        .abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION,
        .context = mock,
        .synthesize = mock_tts_synthesize,
        .cancel = mock_tts_cancel,
        .destroy = mock_tts_destroy};
    return provider;
}

/* ---- mock ASR provider ---- */
typedef struct {
    const turbo_asr_provider_callbacks_t *callbacks;
    void *callback_user_data;
    int start_count;
    int write_count;
    int finish_count;
    int cancel_count;
    int destroy_count;
    size_t last_len;
    int final_emitted;
} mock_asr_t;

static int mock_asr_start(void *context, const turbo_asr_config_t *config,
                          const turbo_asr_provider_callbacks_t *callbacks,
                          void *callback_user_data) {
    mock_asr_t *mock = (mock_asr_t *)context;
    (void)config;
    mock->callbacks = callbacks;
    mock->callback_user_data = callback_user_data;
    mock->start_count++;
    mock->write_count = 0;
    mock->final_emitted = 0;
    return TURBO_SPEECH_OK;
}

static int mock_asr_write(void *context, const turbo_speech_audio_frame_t *frame) {
    mock_asr_t *mock = (mock_asr_t *)context;
    mock->write_count++;
    mock->last_len = frame->len;
    if (mock->write_count >= 3 && !mock->final_emitted) {
        /* emit one final transcript */
        static const char text[] = "hello";
        turbo_asr_result_t result = {.text = text,
                                     .text_len = sizeof(text) - 1U,
                                     .start_time_us = 0,
                                     .end_time_us = 1000,
                                     .confidence = 0.9f,
                                     .is_final = 1};
        mock->callbacks->on_result(&result, mock->callback_user_data);
        mock->final_emitted = 1;
    }
    return TURBO_SPEECH_OK;
}

static int mock_asr_finish(void *context) {
    mock_asr_t *mock = (mock_asr_t *)context;
    mock->finish_count++;
    mock->callbacks->on_complete(mock->callback_user_data);
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

static turbo_asr_provider_t make_asr_provider(mock_asr_t *mock) {
    turbo_asr_provider_t provider = {
        .abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION,
        .context = mock,
        .start = mock_asr_start,
        .write = mock_asr_write,
        .finish = mock_asr_finish,
        .cancel = mock_asr_cancel,
        .destroy = mock_asr_destroy};
    return provider;
}

/* ---- loopback transport + event observer ---- */
typedef struct {
    int play_audio_count;
    size_t play_audio_bytes;
    int stop_count;
} loopback_t;

static int loopback_play_audio(void *ctx, const ivr_call_ref_t *call,
                               const uint8_t *pcm, size_t len,
                               uint32_t sample_rate) {
    loopback_t *loop = (loopback_t *)ctx;
    (void)call;
    (void)pcm;
    (void)sample_rate;
    loop->play_audio_count++;
    loop->play_audio_bytes += len;
    return 0;
}

static int loopback_stop(void *ctx, const ivr_call_ref_t *call) {
    loopback_t *loop = (loopback_t *)ctx;
    (void)call;
    loop->stop_count++;
    return 0;
}

typedef struct {
    ivr_mutex_t lock;
    int asr_final_count;
    int playback_finished_count;
    int provider_error_count;
    char input_id[32];
    char input_value[32];
    char provider_error_payload[128];
} observer_t;

static void observe_event(void *ctx, const ivr_event_view_t *event) {
    observer_t *obs = (observer_t *)ctx;
    if (!event) {
        return;
    }
    ivr_mutex_lock(&obs->lock);
    if (event->event_type.size == 9 &&
        memcmp(event->event_type.data, "asr.final", 9) == 0) {
        obs->asr_final_count++;
        int input_id_size = (int)event->input_id.size;
        if (input_id_size > 31) {
            input_id_size = 31;
        }
        memcpy(obs->input_id, event->input_id.data, (size_t)input_id_size);
        obs->input_id[input_id_size] = '\0';
        int n = (int)event->input_value.size;
        if (n > 31) {
            n = 31;
        }
        memcpy(obs->input_value, event->input_value.data, (size_t)n);
        obs->input_value[n] = '\0';
    } else if (event->event_type.size == 17 &&
               memcmp(event->event_type.data, "playback.finished", 17) == 0) {
        obs->playback_finished_count++;
    } else if (event->event_type.size == 14 &&
               memcmp(event->event_type.data, "provider.error", 14) == 0) {
        size_t n = event->payload_json.size;
        obs->provider_error_count++;
        if (n >= sizeof(obs->provider_error_payload)) {
            n = sizeof(obs->provider_error_payload) - 1u;
        }
        memcpy(obs->provider_error_payload, event->payload_json.data, n);
        obs->provider_error_payload[n] = '\0';
    }
    ivr_mutex_unlock(&obs->lock);
}

/* ---- shared fixtures ---- */
static mock_tts_t g_tts;
static mock_asr_t g_asr;
static loopback_t g_loop;
static observer_t g_obs;
static turbo_tts_provider_t g_tts_provider;
static turbo_asr_provider_t g_asr_provider;
static ivr_media_transport_t g_transport;
static ivr_media_bot_t *g_bot = NULL;
static ivr_media_port_ops_t g_ops;

static void setUp(void) {
    memset(&g_tts, 0, sizeof(g_tts));
    memset(&g_asr, 0, sizeof(g_asr));
    memset(&g_loop, 0, sizeof(g_loop));
    memset(&g_obs, 0, sizeof(g_obs));
    ivr_mutex_init(&g_obs.lock);

    /* provider/transport structs must outlive the bot: keep them at file
       scope (the bot borrows them). */
    g_tts_provider = make_tts_provider(&g_tts);
    g_asr_provider = make_asr_provider(&g_asr);
    g_transport.context = &g_loop;
    g_transport.play_audio = loopback_play_audio;
    g_transport.stop = loopback_stop;
    ivr_media_bot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tts_provider = &g_tts_provider;
    cfg.asr_provider = &g_asr_provider;
    cfg.transport = g_transport;
    cfg.sample_rate = 16000;
    cfg.on_event = observe_event;
    cfg.event_ctx = &g_obs;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_media_bot_create(&cfg, &g_bot));
    TEST_ASSERT_NOT_NULL(g_bot);
    ivr_media_bot_get_ops(g_bot, &g_ops);
}

static void tearDown(void) {
    if (g_bot) {
        ivr_media_bot_destroy(g_bot);
        g_bot = NULL;
    }
    ivr_mutex_destroy(&g_obs.lock);
}

void test_start_and_play_tts_pcm(void) {
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    g_tts.complete_on_synthesize = 1;
    static ivr_bytes_view_t text = {"hello there", 11};
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.play_pcm(g_ops.context, &g_call, &text));
    TEST_ASSERT_EQUAL_INT(1, g_tts.synthesize_count);
    /* TTS emitted 3 frames of 32 bytes each to the transport */
    TEST_ASSERT_EQUAL_INT(3, g_loop.play_audio_count);
    TEST_ASSERT_EQUAL_size_t(96u, g_loop.play_audio_bytes);
    TEST_ASSERT_EQUAL_INT(1, g_obs.playback_finished_count);
    TEST_ASSERT_EQUAL_INT(0, g_asr.start_count);
}

void test_caller_audio_produces_asr_final(void) {
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    static const ivr_bytes_view_t input_id = {"input-42", 8};
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.begin_input(g_ops.context, &g_call,
                                               &input_id, 1));
    static const uint8_t pcm[32] = {0};
    TEST_ASSERT_EQUAL(IVR_OK, ivr_media_bot_feed_caller_audio(g_bot, &g_call,
                                                              pcm, sizeof(pcm)));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_media_bot_feed_caller_audio(g_bot, &g_call,
                                                              pcm, sizeof(pcm)));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_media_bot_feed_caller_audio(g_bot, &g_call,
                                                              pcm, sizeof(pcm)));
    /* ASR emits its final on the 3rd write -> asr.final event */
    ivr_mutex_lock(&g_obs.lock);
    int finals = g_obs.asr_final_count;
    ivr_mutex_unlock(&g_obs.lock);
    TEST_ASSERT_EQUAL_INT(1, finals);
    TEST_ASSERT_EQUAL_STRING("hello", g_obs.input_value);
    TEST_ASSERT_EQUAL_STRING("input-42", g_obs.input_id);
    TEST_ASSERT_EQUAL_INT(3, g_asr.write_count);
}

void test_input_end_finishes_and_allows_next_window(void) {
    static const ivr_bytes_view_t first = {"input-1", 7};
    static const ivr_bytes_view_t second = {"input-2", 7};
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      g_ops.begin_input(g_ops.context, &g_call, &first, 1));
    TEST_ASSERT_EQUAL(IVR_OK,
                      g_ops.end_input(g_ops.context, &g_call, &first, 1));
    TEST_ASSERT_EQUAL_INT(1, g_asr.finish_count);
    TEST_ASSERT_EQUAL(IVR_OK,
                      g_ops.begin_input(g_ops.context, &g_call, &second, 2));
    TEST_ASSERT_EQUAL_INT(2, g_asr.start_count);
}

void test_tts_failure_produces_provider_error(void) {
    static ivr_bytes_view_t text = {"hello", 5};
    int errors;
    char payload[sizeof(g_obs.provider_error_payload)];

    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    g_tts.synthesize_error = TURBO_SPEECH_ERR_PROVIDER;
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      g_ops.play_pcm(g_ops.context, &g_call, &text));

    ivr_mutex_lock(&g_obs.lock);
    errors = g_obs.provider_error_count;
    memcpy(payload, g_obs.provider_error_payload, sizeof(payload));
    ivr_mutex_unlock(&g_obs.lock);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"provider\":\"tts\""));
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"error_code\":"));
}

void test_audio_for_wrong_call_dropped(void) {
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    static ivr_bytes_view_t text = {"hi", 2};
    static const ivr_call_ref_t other = {
        .provider_session_id = {"session-99", 10},
        .dialog_id = {"dialog-99", 9},
        .room_id = {"room-99", 7},
        .call_id = {"call-99", 7},
        .call_generation = 1};
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      g_ops.play_pcm(g_ops.context, &other, &text));
    static const uint8_t pcm[16] = {0};
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_media_bot_feed_caller_audio(g_bot, &other, pcm,
                                                      sizeof(pcm)));
}

void test_cancel_quiesces_and_stop_tears_down(void) {
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    static const ivr_bytes_view_t input_id = {"input-42", 8};
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.begin_input(g_ops.context, &g_call,
                                               &input_id, 1));
    TEST_ASSERT_EQUAL_INT(1, g_asr.start_count);
    /* a prompt must be running for TTS cancel to reach the provider */
    static ivr_bytes_view_t prompt = {"hello", 5};
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.play_pcm(g_ops.context, &g_call, &prompt));
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.cancel_input(g_ops.context, &g_call));
    TEST_ASSERT_EQUAL_INT(1, g_tts.cancel_count);
    TEST_ASSERT_EQUAL_INT(1, g_asr.cancel_count);

    TEST_ASSERT_EQUAL(IVR_OK, g_ops.stop_bot(g_ops.context, &g_call));
    TEST_ASSERT_EQUAL_INT(1, g_tts.destroy_count);
    TEST_ASSERT_EQUAL_INT(1, g_asr.destroy_count);
    TEST_ASSERT_EQUAL_INT(1, g_loop.stop_count);

    /* after stop the peer is gone: play fails fast */
    static ivr_bytes_view_t text = {"hi", 2};
    TEST_ASSERT_EQUAL(IVR_ESTATE, g_ops.play_pcm(g_ops.context, &g_call, &text));
    /* and the call can be re-started */
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
}

void test_second_active_call_rejected(void) {
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    static const ivr_call_ref_t other = {
        .provider_session_id = {"session-43", 10},
        .dialog_id = {"dialog-43", 9},
        .room_id = {"room-43", 7},
        .call_id = {"call-43", 7},
        .call_generation = 1,
        .expected_room_version = 1};
    TEST_ASSERT_EQUAL(IVR_ESTATE, g_ops.start_bot(g_ops.context, &other));
}

void test_missing_tts_fails_fast(void) {
    ivr_media_bot_t *bot = NULL;
    ivr_media_bot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.asr_provider = &g_asr_provider;
    cfg.transport = g_transport;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_media_bot_create(&cfg, &bot));
    ivr_media_port_ops_t ops;
    ivr_media_bot_get_ops(bot, &ops);
    TEST_ASSERT_EQUAL(IVR_OK, ops.start_bot(ops.context, &g_call));
    static ivr_bytes_view_t text = {"hi", 2};
    TEST_ASSERT_EQUAL(IVR_ESTATE, ops.play_pcm(ops.context, &g_call, &text));
    TEST_ASSERT_EQUAL_INT(0, g_tts.synthesize_count);
    ivr_media_bot_destroy(bot);
}

/* ---- concurrency: feed/play on a receiver thread vs stop_bot ---- */

typedef struct {
    ivr_media_bot_t *bot;
    ivr_call_ref_t call;
    int iterations;
} bot_stress_t;

static void *feed_thread_main(void *opaque) {
    bot_stress_t *s = (bot_stress_t *)opaque;
    static const uint8_t pcm[32] = {0};
    for (int i = 0; i < s->iterations; i++) {
        (void)ivr_media_bot_feed_caller_audio(s->bot, &s->call, pcm,
                                              sizeof(pcm));
    }
    return NULL;
}

static void *play_thread_main(void *opaque) {
    bot_stress_t *s = (bot_stress_t *)opaque;
    static ivr_bytes_view_t text = {"hello", 5};
    for (int i = 0; i < s->iterations; i++) {
        (void)g_ops.play_pcm(g_ops.context, &s->call, &text);
    }
    return NULL;
}

void test_concurrent_feed_and_stop(void) {
    /* feed_caller_audio is documented thread-safe: a receiver thread feeding
       PCM while stop_bot tears the ASR session down must never touch a freed
    session (ASan build catches any UAF). */
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    static const ivr_bytes_view_t input_id = {"input-stress", 12};
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.begin_input(g_ops.context, &g_call,
                                               &input_id, 1));
    bot_stress_t s = {g_bot, g_call, 20000};
    ivr_thread_t th;
    TEST_ASSERT_EQUAL_INT(0, ivr_thread_create(&th, feed_thread_main, &s));
    ivr_thread_sleep_ms(5);
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.stop_bot(g_ops.context, &g_call));
    ivr_thread_join(&th);
    TEST_ASSERT_EQUAL_INT(1, g_asr.destroy_count);
    /* after stop the peer is gone: feeding fails fast */
    static const uint8_t pcm[16] = {0};
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_media_bot_feed_caller_audio(g_bot, &g_call, pcm,
                                                      sizeof(pcm)));
}

void test_concurrent_play_and_stop(void) {
    /* play_pcm holds a session borrow across synthesize; a concurrent
       stop_bot must wait for it before destroying the TTS session. */
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.start_bot(g_ops.context, &g_call));
    bot_stress_t s = {g_bot, g_call, 20000};
    ivr_thread_t th;
    TEST_ASSERT_EQUAL_INT(0, ivr_thread_create(&th, play_thread_main, &s));
    ivr_thread_sleep_ms(5);
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.stop_bot(g_ops.context, &g_call));
    ivr_thread_join(&th);
    TEST_ASSERT_EQUAL_INT(1, g_tts.destroy_count);
    static ivr_bytes_view_t text = {"hi", 2};
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      g_ops.play_pcm(g_ops.context, &g_call, &text));
}

spec("test_ivr_media_bot") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_start_and_play_tts_pcm);
  TT_TEST(test_caller_audio_produces_asr_final);
  TT_TEST(test_input_end_finishes_and_allows_next_window);
  TT_TEST(test_tts_failure_produces_provider_error);
  TT_TEST(test_audio_for_wrong_call_dropped);
  TT_TEST(test_cancel_quiesces_and_stop_tears_down);
  TT_TEST(test_second_active_call_rejected);
  TT_TEST(test_missing_tts_fails_fast);
  TT_TEST(test_concurrent_feed_and_stop);
  TT_TEST(test_concurrent_play_and_stop);
}
