/* ivr_worker - standalone IVR worker executable.
 *
 * Wires the real pieces together: FlowMQ DEALER command gateway (worker.sync
 * registration + typed media commands/results), a media-event sink, and
 * per-call WHIP/WHEP bot media. Iris remains the only XML/JavaScript workflow
 * owner; this process does not consume domain events or content packages.
 *
 * Run loop stops on SIGINT/SIGTERM: begin_drain -> stop subscriber -> destroy
 * gateway -> destroy worker. `--dry-run` validates the config and exercises
 * worker/session creation without touching FlowMQ. */
#include "ivr/ivr_worker.h"
#include "ivr_media_bot.h"
#include "ivr_openai_provider.h"
#include "ivr_speech_session_factory.h"
#include "ivr_flowmq_gateway.h"
#include "ivr_http_media_client.h"
#include "ivr/ivr_acl.h"
#include "flowmq_coronet.h"
#include "ivr_whip_transport.h"
#include "ivr_whep_transport.h"
#include "ivr_media_supervisor.h"
#include "ivr_frame.h"
#include "ivr_internal.h"
#include "ivr_dtmf_rtp.h"
#include "ivr_thread.h"
#include "ivr_worker_health.h"
#include "ivr_worker_http.h"
#include "ivr_worker_control.h"
#include "ivr_worker_metrics.h"
#include "turbomedia_ivr_v1.h"
#include "disruptor.h"
#include "platform.h"
#include "salts_uuid.h"
#include "../config_toml.h"
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define app_strdup _strdup
#else
#define app_strdup strdup
#endif

#define IVR_WORKER_APP_VERSION "1.0.0"
#define IVR_WORKER_APP_MAX_ASSIGN 32
#define IVR_WORKER_REPLY_QUEUE_CAPACITY 16u
#define IVR_WORKER_CONTROL_QUEUE_CAPACITY 32u
#define IVR_WORKER_REPLY_FRAME_CAPACITY (IVR_FRAME_HEADER_SIZE + 64u * 1024u)
#define IVR_WORKER_MEDIA_EVENT_QUEUE_CAPACITY 64u
#define IVR_WORKER_MEDIA_EVENT_PAYLOAD_CAPACITY 4096u
#define IVR_WORKER_DEFAULT_HEARTBEAT_MS 5000u
#define IVR_WORKER_DEFAULT_LEASE_MS 15000u
#define IVR_WORKER_DEFAULT_HEALTH_PORT 18081
#define IVR_WORKER_TRANSPORT_TIMEOUT_HEARTBEATS 2u
#define IVR_MEDIA_RECONNECT_MAX_ATTEMPTS 8u
#define IVR_MEDIA_RECONNECT_INITIAL_BACKOFF_MS 100u
#define IVR_MEDIA_RECONNECT_MAX_BACKOFF_MS 2000u
/* One recovery episode may need both 10-second WHIP/WHEP connection windows
   plus the bounded 7.1-second retry schedule. */
#define IVR_MEDIA_RECONNECT_DEADLINE_MS 30000u
#define IVR_MEDIA_INPUT_INACTIVITY_TIMEOUT_MAX_MS 3600000u
#define IVR_MEDIA_DEFAULT_SAMPLE_RATE 16000
#define IVR_MEDIA_FRAME_DURATION_MS 20u
#define IVR_MEDIA_PCM_BYTES_PER_SAMPLE 2u

/* Remote TTS/ASR (OpenAI-compatible) wiring, enabled explicitly by env:
 *   IVR_OPENAI_BASE_URL   required endpoint, e.g. https://api.openai.com
 *   OPENAI_API_KEY        Bearer credential (optional; endpoints without auth)
 *   IVR_OPENAI_TTS_MODEL / IVR_OPENAI_TTS_VOICE / IVR_OPENAI_ASR_MODEL
 *   IVR_OPENAI_ASR_LANGUAGE / IVR_OPENAI_SAMPLE_RATE / IVR_OPENAI_TIMEOUT_MS
 * Without IVR_OPENAI_BASE_URL active mode fails readiness; only explicit
 * dry-run/shadow modes may use the logging transport. */
static ivr_openai_config_t g_remote_config;
static ivr_openai_speech_factory_t g_remote_speech_factory;
static ivr_speech_session_factory_ops_t g_speech_factory;
static int g_remote_configured = 0;
static int g_env_parse_failed = 0;
static ivr_worker_metrics_t g_metrics;

static void remote_speech_observe_request(void *context,
                                          ivr_openai_request_kind_t kind,
                                          uint64_t duration_ms) {
    ivr_worker_metrics_t *metrics = (ivr_worker_metrics_t *)context;
    if (kind == IVR_OPENAI_REQUEST_TTS) {
        ivr_worker_metrics_observe_ms(metrics,
                                      IVR_WORKER_HISTOGRAM_TTS_PROVIDER,
                                      duration_ms);
    } else if (kind == IVR_OPENAI_REQUEST_ASR) {
        ivr_worker_metrics_observe_ms(metrics,
                                      IVR_WORKER_HISTOGRAM_ASR_PROVIDER,
                                      duration_ms);
    }
}

static void metrics_reply_queue_add(size_t bytes) {
    uint64_t items;
    uint64_t retained;
    if (ivr_worker_metrics_add_gauge(
            &g_metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS, 1, &items) != 0 ||
        ivr_worker_metrics_add_gauge(
            &g_metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, (uint64_t)bytes,
            &retained) != 0) {
        fprintf(stderr, "ivr_worker: reply queue metrics overflow\n");
        return;
    }
    ivr_worker_metrics_set_gauge_max(
        &g_metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS_HIGH_WATER, items);
    ivr_worker_metrics_set_gauge_max(
        &g_metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES_HIGH_WATER, retained);
}

static void metrics_reply_queue_remove(size_t bytes) {
    uint64_t items;
    uint64_t retained;
    if (ivr_worker_metrics_sub_gauge(
            &g_metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS, 1, &items) != 0 ||
        ivr_worker_metrics_sub_gauge(
            &g_metrics, IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, (uint64_t)bytes,
            &retained) != 0) {
        fprintf(stderr, "ivr_worker: reply queue metrics underflow\n");
    }
}

static void metrics_media_peers_add(uint32_t count) {
    uint64_t peers;
    if (ivr_worker_metrics_add_gauge(&g_metrics,
                                     IVR_WORKER_GAUGE_MEDIA_PEERS, count,
                                     &peers) != 0) {
        fprintf(stderr, "ivr_worker: media peer metrics overflow\n");
        return;
    }
    ivr_worker_metrics_set_gauge_max(
        &g_metrics, IVR_WORKER_GAUGE_MEDIA_PEERS_HIGH_WATER, peers);
}

static void metrics_media_peers_remove(uint32_t count) {
    uint64_t peers;
    if (ivr_worker_metrics_sub_gauge(&g_metrics,
                                     IVR_WORKER_GAUGE_MEDIA_PEERS, count,
                                     &peers) != 0) {
        fprintf(stderr, "ivr_worker: media peer metrics underflow\n");
    }
}

static void remote_speech_shutdown(void) {
    if (!g_remote_speech_factory.config_storage) {
        return;
    }
    if (ivr_openai_speech_factory_deinit(&g_remote_speech_factory) != IVR_OK) {
        fprintf(stderr,
                "ivr_worker: speech factory shutdown with active sessions\n");
        return;
    }
    memset(&g_speech_factory, 0, sizeof(g_speech_factory));
    g_remote_configured = 0;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    char *end = NULL;
    long value;
    if (!v || v[0] == '\0') {
        return def;
    }
    errno = 0;
    value = strtol(v, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < INT_MIN ||
        value > INT_MAX) {
        fprintf(stderr, "ivr_worker: invalid integer environment %s=%s\n",
                name, v);
        g_env_parse_failed = 1;
        return def;
    }
    return (int)value;
}

static int remote_speech_init(void) {
    const char *base_url = getenv("IVR_OPENAI_BASE_URL");
    const char *speech_sample_rate = getenv("IVR_OPENAI_SAMPLE_RATE");
    int output_sample_rate;
    size_t output_frame_samples;
    if (!base_url || base_url[0] == '\0') {
        return 0;
    }
    memset(&g_remote_config, 0, sizeof(g_remote_config));
    g_remote_config.base_url = base_url;
    g_remote_config.api_key = getenv("OPENAI_API_KEY");
    g_remote_config.tts_model = getenv("IVR_OPENAI_TTS_MODEL");
    g_remote_config.tts_voice = getenv("IVR_OPENAI_TTS_VOICE");
    g_remote_config.asr_model = getenv("IVR_OPENAI_ASR_MODEL");
    g_remote_config.asr_language = getenv("IVR_OPENAI_ASR_LANGUAGE");
    output_sample_rate =
        speech_sample_rate && speech_sample_rate[0]
            ? env_int("IVR_OPENAI_SAMPLE_RATE", 0)
            : env_int("IVR_MEDIA_SAMPLE_RATE", IVR_MEDIA_DEFAULT_SAMPLE_RATE);
    if (output_sample_rate <= 0 || output_sample_rate > 192000) {
        fprintf(stderr,
                "ivr_worker: invalid remote speech output sample rate\n");
        return -1;
    }
    output_frame_samples =
        (size_t)output_sample_rate * IVR_MEDIA_FRAME_DURATION_MS / 1000u;
    if (output_frame_samples == 0u ||
        output_frame_samples > SIZE_MAX / IVR_MEDIA_PCM_BYTES_PER_SAMPLE) {
        fprintf(stderr, "ivr_worker: invalid remote speech frame size\n");
        return -1;
    }
    g_remote_config.sample_rate = output_sample_rate;
    g_remote_config.tts_frame_bytes =
        output_frame_samples * IVR_MEDIA_PCM_BYTES_PER_SAMPLE;
    g_remote_config.timeout_ms = env_int("IVR_OPENAI_TIMEOUT_MS", 0);
    g_remote_config.max_tts_input_bytes =
        (size_t)env_int("IVR_OPENAI_MAX_TTS_INPUT_BYTES", 64 * 1024);
    g_remote_config.max_asr_buffer_bytes =
        (size_t)env_int("IVR_OPENAI_MAX_ASR_BYTES", 4 * 1024 * 1024);
    g_remote_config.max_response_bytes =
        (size_t)env_int("IVR_OPENAI_MAX_RESPONSE_BYTES", 16 * 1024 * 1024);
    g_remote_config.observer.context = &g_metrics;
    g_remote_config.observer.on_request_complete =
        remote_speech_observe_request;
    if (ivr_openai_speech_factory_init(&g_remote_speech_factory,
                                       &g_remote_config,
                                       NULL,
                                       &g_speech_factory) != IVR_OK) {
        return -1;
    }
    char probe_error[128];
    memset(probe_error, 0, sizeof(probe_error));
    if (g_speech_factory.probe(g_speech_factory.context, probe_error,
                               sizeof(probe_error)) != IVR_OK) {
        fprintf(stderr, "ivr_worker: remote speech probe failed: %s\n",
                probe_error);
        memset(&g_speech_factory, 0, sizeof(g_speech_factory));
        (void)ivr_openai_speech_factory_deinit(&g_remote_speech_factory);
        return -1;
    }
    g_remote_configured = 1;
    if (atexit(remote_speech_shutdown) != 0) {
        remote_speech_shutdown();
        return -1;
    }
    printf("[ivr_worker] remote speech: endpoint=configured tts=%s asr=%s\n",
           g_remote_config.tts_model ? g_remote_config.tts_model : "tts-1",
           g_remote_config.asr_model ? g_remote_config.asr_model
                                     : "whisper-1");
    return 0;
}


typedef struct {
    int shadow;
    const char *worker_id;
    const char *router_host;
    int router_port;
    const char *health_host;
    int health_port;
    uint32_t max_sessions;
    uint64_t timeout_ms;
    uint64_t heartbeat_ms;
    uint64_t lease_ms;
    int fmq_use_tls;
    int fmq_allow_insecure_loopback;
    const char *fmq_ca_file;
    const char *fmq_cert_file;
    const char *fmq_key_file;
    const char *fmq_key_password;
    const char *fmq_server_name;
    /* Optional defense-in-depth scope for authenticated media commands. */
    const char *tenant_id;
    const char *room_scope;
    const char *call_scope;
    int dry_run;
    const char *config_file;
    int assign_count;
    const char *assign_session[IVR_WORKER_APP_MAX_ASSIGN];
    const char *assign_dialog[IVR_WORKER_APP_MAX_ASSIGN];
    const char *assign_room[IVR_WORKER_APP_MAX_ASSIGN];
    const char *assign_call[IVR_WORKER_APP_MAX_ASSIGN];
} ivr_worker_app_config_t;

static ivr_worker_app_config_t g_config;
static rtc_app_toml_document_t g_toml_document;
static char *g_toml_strings[32];
static size_t g_toml_string_count = 0;

static int parse_u64_arg(const char *text, uint64_t *out);
static int parse_u32_arg(const char *text, uint32_t *out);
static int parse_port_arg(const char *text, int *out);

static void toml_strings_cleanup(void) {
    size_t i;
    for (i = 0; i < g_toml_string_count; ++i) {
        free(g_toml_strings[i]);
    }
    g_toml_string_count = 0;
}

static void toml_config_shutdown(void) {
    if (g_toml_document.root || g_toml_document.file.base) {
        rtc_app_toml_document_close(&g_toml_document);
    }
    toml_strings_cleanup();
}

static int toml_copy_string(const toml_table_t *table, const char *key,
                            const char **target) {
    toml_value_t value;
    char *copy;
    if (!table || !key || !target || !rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = toml_table_string(table, key);
    if (!value.ok || !value.u.s || value.u.sl < 0 ||
        strlen(value.u.s) != (size_t)value.u.sl ||
        g_toml_string_count >= sizeof(g_toml_strings) / sizeof(g_toml_strings[0])) {
        free(value.ok ? value.u.s : NULL);
        return -1;
    }
    copy = app_strdup(value.u.s);
    free(value.u.s);
    if (!copy) {
        return -1;
    }
    g_toml_strings[g_toml_string_count++] = copy;
    *target = copy;
    return 0;
}

static int toml_copy_int(const toml_table_t *table, const char *key, int *target) {
    toml_value_t value;
    if (!table || !key || !target || !rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = toml_table_int(table, key);
    if (!value.ok || value.u.i < INT_MIN || value.u.i > INT_MAX) {
        return -1;
    }
    *target = (int)value.u.i;
    return 0;
}

static int toml_copy_bool(const toml_table_t *table, const char *key,
                          int *target) {
    toml_value_t value;
    if (!table || !key || !target || !rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = toml_table_bool(table, key);
    if (!value.ok) return -1;
    *target = value.u.b ? 1 : 0;
    return 0;
}

static int load_toml_config(const char *filename) {
    toml_table_t *worker = NULL;
    const char *const allowed[] = {
        "worker_id",   "router_host",  "router_port",  "health_host",
        "health_port", "max_sessions", "heartbeat_ms", "lease_ms",
        "fmq_use_tls", "fmq_allow_insecure_loopback", "fmq_ca_file",
        "fmq_cert_file", "fmq_key_file", "fmq_key_password",
        "fmq_server_name",
        "tenant_id", "room_scope", "call_scope"};
    if (!filename || !filename[0]) {
        return 0;
    }
    if (rtc_app_toml_document_open(&g_toml_document, filename) != 0 ||
        rtc_app_toml_get_optional_table(g_toml_document.root, "worker", &worker) != 0) {
        return -1;
    }
    if (!worker || rtc_app_toml_table_keys_valid(
                       worker, "worker", allowed, sizeof(allowed) / sizeof(allowed[0])) != 0 ||
        toml_copy_string(worker, "worker_id", &g_config.worker_id) != 0 ||
        toml_copy_string(worker, "router_host", &g_config.router_host) != 0 ||
        toml_copy_int(worker, "router_port", &g_config.router_port) != 0 ||
        toml_copy_string(worker, "health_host", &g_config.health_host) != 0 ||
        toml_copy_int(worker, "health_port", &g_config.health_port) != 0 ||
        toml_copy_bool(worker, "fmq_use_tls", &g_config.fmq_use_tls) != 0 ||
        toml_copy_bool(worker, "fmq_allow_insecure_loopback",
                       &g_config.fmq_allow_insecure_loopback) != 0 ||
        toml_copy_string(worker, "fmq_ca_file", &g_config.fmq_ca_file) != 0 ||
        toml_copy_string(worker, "fmq_cert_file", &g_config.fmq_cert_file) != 0 ||
        toml_copy_string(worker, "fmq_key_file", &g_config.fmq_key_file) != 0 ||
        toml_copy_string(worker, "fmq_key_password",
                         &g_config.fmq_key_password) != 0 ||
        toml_copy_string(worker, "fmq_server_name",
                         &g_config.fmq_server_name) != 0 ||
        toml_copy_string(worker, "tenant_id", &g_config.tenant_id) != 0 ||
        toml_copy_string(worker, "room_scope", &g_config.room_scope) != 0 ||
        toml_copy_string(worker, "call_scope", &g_config.call_scope) != 0) {
        rtc_app_toml_document_close(&g_toml_document);
        toml_strings_cleanup();
        return -1;
    }
    {
        int value;
        if (toml_copy_int(worker, "max_sessions", &value) != 0 ||
            (rtc_app_toml_table_has_key(worker, "max_sessions") &&
             (value <= 0 || (uint64_t)value > UINT32_MAX))) {
            return -1;
        }
        if (rtc_app_toml_table_has_key(worker, "max_sessions")) {
            g_config.max_sessions = (uint32_t)value;
        }
        if (toml_copy_int(worker, "heartbeat_ms", &value) != 0 ||
            (rtc_app_toml_table_has_key(worker, "heartbeat_ms") && value <= 0)) {
            return -1;
        }
        if (rtc_app_toml_table_has_key(worker, "heartbeat_ms")) {
            g_config.heartbeat_ms = (uint64_t)value;
        }
        if (toml_copy_int(worker, "lease_ms", &value) != 0 ||
            (rtc_app_toml_table_has_key(worker, "lease_ms") && value <= 0)) {
            return -1;
        }
        if (rtc_app_toml_table_has_key(worker, "lease_ms")) {
            g_config.lease_ms = (uint64_t)value;
        }
    }
    return 0;
}

static int apply_env_config(void) {
    const char *v;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int port = 0;
#define ENV_STR(name, field) do { v = getenv(name); if (v && v[0]) g_config.field = v; } while (0)
    ENV_STR("IVR_WORKER_ID", worker_id);
    ENV_STR("IVR_ROUTER_HOST", router_host);
    ENV_STR("IVR_HEALTH_HOST", health_host);
    ENV_STR("IVR_FMQ_CA_FILE", fmq_ca_file);
    ENV_STR("IVR_FMQ_CERT_FILE", fmq_cert_file);
    ENV_STR("IVR_FMQ_KEY_FILE", fmq_key_file);
    ENV_STR("IVR_FMQ_KEY_PASSWORD", fmq_key_password);
    ENV_STR("IVR_FMQ_SERVER_NAME", fmq_server_name);
#undef ENV_STR
    v = getenv("IVR_ROUTER_PORT");
    if (v && v[0] && (parse_port_arg(v, &port) != 0)) return -1;
    if (v && v[0]) g_config.router_port = port;
    v = getenv("IVR_HEALTH_PORT");
    if (v && v[0] && (parse_port_arg(v, &port) != 0)) return -1;
    if (v && v[0]) g_config.health_port = port;
    v = getenv("IVR_MAX_SESSIONS");
    if (v && v[0] && (parse_u32_arg(v, &u32) != 0)) return -1;
    if (v && v[0]) g_config.max_sessions = u32;
    v = getenv("IVR_HEARTBEAT_MS");
    if (v && v[0] && (parse_u64_arg(v, &u64) != 0)) return -1;
    if (v && v[0]) g_config.heartbeat_ms = u64;
    v = getenv("IVR_LEASE_MS");
    if (v && v[0] && (parse_u64_arg(v, &u64) != 0)) return -1;
    if (v && v[0]) g_config.lease_ms = u64;
    v = getenv("IVR_FMQ_USE_TLS");
    if (v && v[0]) {
        if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0) {
            g_config.fmq_use_tls = 1;
        } else if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0) {
            g_config.fmq_use_tls = 0;
        } else {
            return -1;
        }
    }
    v = getenv("IVR_FMQ_ALLOW_INSECURE_LOOPBACK");
    if (v && v[0]) {
        if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0) {
            g_config.fmq_allow_insecure_loopback = 1;
        } else if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0) {
            g_config.fmq_allow_insecure_loopback = 0;
        } else {
            return -1;
        }
    }
    return 0;
}
static ivr_worker_t *g_worker = NULL;
static ivr_flowmq_gateway_t *g_gateway = NULL;
static ivr_command_gateway_ops_t g_real_ops;
static volatile sig_atomic_t g_signal_stop = 0;
static ivr_atomic_int_t g_running;
static int g_synced = 0; /* owner-thread state: worker.sync acknowledged */
static DataBind *g_codec = NULL;  /* process-lifetime codec for dispatch decode */

typedef struct {
    size_t length;
    uint8_t frame[IVR_WORKER_REPLY_FRAME_CAPACITY];
} ivr_worker_reply_entry_t;

typedef struct {
    char event_id[IVR_MEDIA_ID_CAPACITY];
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char event_type[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t expected_room_version;
    uint64_t sequence;
    char input_id[IVR_MEDIA_ID_CAPACITY];
    char input_value[IVR_MEDIA_ID_CAPACITY];
    char payload_json[IVR_WORKER_MEDIA_EVENT_PAYLOAD_CAPACITY];
} ivr_worker_media_event_entry_t;

static disruptor_t *g_reply_queue = NULL;
static disruptor_t *g_media_event_queue = NULL;
static ivr_worker_control_queue_t *g_control_queue = NULL;
static ivr_worker_control_state_t g_control_state;
static ivr_atomic_int_t g_accept_replies;
static ivr_atomic_int_t g_accept_media_events;
static atomic_ullong g_media_event_sequence;
static char g_instance_id[SALTS_UUID_STRING_SIZE];
static const char *g_sfu_base_url = NULL;
static const char *g_sfu_media_token = NULL;
static const char *g_sfu_ca_file = NULL;
static const char *g_sfu_cert_file = NULL;
static const char *g_sfu_key_file = NULL;
static const char *g_sfu_key_password = NULL;
static int g_sfu_allow_plaintext_loopback = 0;
static int g_sfu_allow_loopback = 0;
static uint64_t g_sfu_http_timeout_ms = 0u;
static uint32_t g_media_sample_rate = 16000u;
static uint64_t g_media_input_inactivity_timeout_ms = 0u;
static ivr_worker_health_t g_health;
static int g_health_initialized = 0;
static ivr_worker_http_t *g_management_http = NULL;

static ivr_status_t enqueue_media_event_copy(
    void *context, const ivr_event_view_t *event);

static void metrics_observe_elapsed(ivr_worker_histogram_kind_t kind,
                                    uint64_t started_at_ms) {
    uint64_t finished_at_ms = salts_monotonic_ms();
    ivr_worker_metrics_observe_ms(
        &g_metrics, kind,
        finished_at_ms >= started_at_ms ? finished_at_ms - started_at_ms : 0);
}

static uint64_t worker_now_ms(void *context) {
    (void)context;
    return salts_monotonic_ms();
}

static void management_http_stop(void) {
    if (g_management_http) {
        ivr_worker_http_destroy(g_management_http);
        g_management_http = NULL;
    }
}

static void control_queue_destroy(void) {
    if (g_control_queue) {
        ivr_worker_control_queue_destroy(g_control_queue);
        g_control_queue = NULL;
    }
}

static void health_publish(int content_ready, int schema_ready,
                           int command_ready, int event_ready, int sync_ready,
                           int speech_ready, int sfu_ready, int draining,
                           int ready, const char *reason) {
    ivr_worker_health_snapshot_t value;
    int effective_ready;
    if (!g_health_initialized ||
        ivr_worker_health_snapshot(&g_health, &value) != 0) {
        return;
    }
    value.content_ready = content_ready;
    value.schema_ready = schema_ready;
    value.command_channel_ready = command_ready;
    value.event_channel_ready = event_ready;
    value.sync_ready = sync_ready;
    value.speech_ready = speech_ready;
    value.sfu_ready = sfu_ready;
    value.draining = draining;
    effective_ready =
        ready && content_ready && schema_ready && command_ready &&
        event_ready && sync_ready && speech_ready && sfu_ready && !draining &&
        value.active_sessions + value.reserved_sessions < value.max_sessions;
    value.ready = effective_ready;
    snprintf(value.capabilities, sizeof(value.capabilities),
             "media-executor,flowmq%s%s%s", speech_ready ? ",tts,asr" : "",
             sfu_ready ? ",whip,whep" : "",
             effective_ready ? ",health.ready" : "");
    snprintf(value.reason, sizeof(value.reason), "%s", reason ? reason : "");
    if (ivr_worker_health_update(&g_health, &value) != 0) {
        fprintf(stderr, "ivr_worker: rejected stale health update\n");
    }
}

static int refresh_health_capacity(ivr_worker_health_snapshot_t *out) {
    ivr_worker_health_snapshot_t health;
    int previous_ready;
    memset(&health, 0, sizeof(health));
    if (!out || ivr_worker_health_snapshot(&g_health, &health) != 0) {
        return -1;
    }
    previous_ready = health.ready;
    health.active_sessions = g_worker ? ivr_worker_active_sessions(g_worker) : 0;
    if (ivr_worker_health_update(&g_health, &health) != 0 ||
        ivr_worker_health_snapshot(&g_health, out) != 0) {
        return -1;
    }
    if (out->ready != previous_ready) {
        snprintf(out->capabilities, sizeof(out->capabilities),
                 "media-executor,flowmq%s%s%s",
                 out->speech_ready ? ",tts,asr" : "",
                 out->sfu_ready ? ",whip,whep" : "",
                 out->ready ? ",health.ready" : "");
        snprintf(out->reason, sizeof(out->reason), "%s",
                 out->ready ? "ready" : "capacity exhausted");
        if (ivr_worker_health_update(&g_health, out) != 0 ||
            ivr_worker_health_snapshot(&g_health, out) != 0) {
            return -1;
        }
    }
    return 0;
}

static void fill_worker_status(ivr_worker_status_view_t *status) {
    static char capabilities[256];
    ivr_worker_health_snapshot_t health;
    memset(&health, 0, sizeof(health));
    if (refresh_health_capacity(&health) != 0) {
        fprintf(stderr, "ivr_worker: health capacity refresh failed\n");
    }
    memset(status, 0, sizeof(*status));
    status->instance_id = g_instance_id;
    status->connection_generation = g_control_state.connection_generation;
    status->max_sessions = g_config.max_sessions;
    status->active_sessions = health.active_sessions;
    status->reserved_sessions = health.reserved_sessions;
    status->lease_duration_ms = g_config.lease_ms;
    status->draining = health.draining;
    status->health_generation = health.generation;
    status->health_ready = health.ready;
    snprintf(capabilities, sizeof(capabilities), "%s", health.capabilities);
    status->capabilities = capabilities;
}

static ivr_status_t send_worker_sync_v2(const char *message_id) {
    ivr_worker_status_view_t status;
    fill_worker_status(&status);
    return ivr_flowmq_gateway_send_worker_sync_v2(g_gateway, message_id,
                                                   &status);
}

static ivr_status_t send_worker_heartbeat(const char *message_id) {
    ivr_worker_status_view_t status;
    fill_worker_status(&status);
    return ivr_flowmq_gateway_send_worker_heartbeat(g_gateway, message_id,
                                                     &status);
}

static DataBind *ensure_codec(void) {
    if (!g_codec) {
        DataBindError err = DATA_BIND_ERROR_INIT;
        if (TurboMediaIvrV1_codec_create(&g_codec, &err) != DATA_BIND_OK) {
            g_codec = NULL;
        }
    }
    return g_codec;
}

/* ------------------------------------------------------------------ */
/* per-call media factory                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t audio_bytes;
    uint64_t audio_frames;
} ivr_worker_log_transport_t;

typedef struct ivr_worker_media_instance ivr_worker_media_instance_t;

typedef struct {
    ivr_worker_media_instance_t *instance;
    ivr_media_link_kind_t link;
} ivr_worker_media_state_context_t;

struct ivr_worker_media_instance {
    ivr_media_bot_t *bot;
    ivr_media_port_ops_t bot_ops;
    ivr_whip_transport_t *whip;
    ivr_whep_transport_t *whep;
    ivr_media_supervisor_t *supervisor;
    uint32_t counted_media_peers;
    atomic_int media_link_connected[2];
    ivr_dtmf_ingress_t *dtmf_ingress;
    ivr_worker_media_state_context_t whip_state;
    ivr_worker_media_state_context_t whep_state;
    ivr_worker_log_transport_t log_transport;
    ivr_speech_session_t *speech;
    int rtc_enabled;
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t expected_room_version;
    char input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t input_generation;
    int input_active;
};

static int media_instance_copy_call(ivr_worker_media_instance_t *instance,
                                     const ivr_call_ref_t *call) {
    if (!instance || !call ||
        !((call->tenant_id.size == 0u && !call->tenant_id.data) ||
          (call->tenant_id.data &&
           call->tenant_id.size < IVR_MEDIA_ID_CAPACITY)) ||
        !call->provider_session_id.data ||
        call->provider_session_id.size == 0 ||
        call->provider_session_id.size >= IVR_MEDIA_ID_CAPACITY ||
        !call->dialog_id.data || call->dialog_id.size == 0 ||
        call->dialog_id.size >= IVR_MEDIA_ID_CAPACITY ||
        !call->room_id.data || !call->call_id.data ||
        call->room_id.size == 0 || call->room_id.size >= IVR_MEDIA_ID_CAPACITY ||
        call->call_id.size == 0 || call->call_id.size >= IVR_MEDIA_ID_CAPACITY) {
        return -1;
    }
    if (call->tenant_id.size > 0u) {
        memcpy(instance->tenant_id, call->tenant_id.data,
               call->tenant_id.size);
        instance->tenant_id[call->tenant_id.size] = '\0';
    }
    memcpy(instance->provider_session_id, call->provider_session_id.data,
           call->provider_session_id.size);
    instance->provider_session_id[call->provider_session_id.size] = '\0';
    memcpy(instance->dialog_id, call->dialog_id.data, call->dialog_id.size);
    instance->dialog_id[call->dialog_id.size] = '\0';
    memcpy(instance->room_id, call->room_id.data, call->room_id.size);
    instance->room_id[call->room_id.size] = '\0';
    memcpy(instance->call_id, call->call_id.data, call->call_id.size);
    instance->call_id[call->call_id.size] = '\0';
    instance->call_generation = call->call_generation;
    instance->expected_room_version = call->expected_room_version;
    return 0;
}

static void media_instance_call(const ivr_worker_media_instance_t *instance,
                                ivr_call_ref_t *call) {
    memset(call, 0, sizeof(*call));
    call->tenant_id.data = instance->tenant_id;
    call->tenant_id.size = strlen(instance->tenant_id);
    call->provider_session_id.data = instance->provider_session_id;
    call->provider_session_id.size = strlen(instance->provider_session_id);
    call->dialog_id.data = instance->dialog_id;
    call->dialog_id.size = strlen(instance->dialog_id);
    call->room_id.data = instance->room_id;
    call->room_id.size = strlen(instance->room_id);
    call->call_id.data = instance->call_id;
    call->call_id.size = strlen(instance->call_id);
    call->call_generation = instance->call_generation;
    call->expected_room_version = instance->expected_room_version;
}

static int media_view_matches_text(const ivr_bytes_view_t *view,
                                   const char *text) {
    size_t text_size;
    if (!view || !text || (view->size > 0u && !view->data)) {
        return 0;
    }
    text_size = strlen(text);
    return view->size == text_size &&
           (text_size == 0u || memcmp(view->data, text, text_size) == 0);
}

static int media_call_matches_instance(
    const ivr_worker_media_instance_t *instance,
    const ivr_call_ref_t *call) {
    return instance && call &&
           call->call_generation == instance->call_generation &&
           call->expected_room_version == instance->expected_room_version &&
           media_view_matches_text(&call->tenant_id, instance->tenant_id) &&
           media_view_matches_text(&call->provider_session_id,
                                   instance->provider_session_id) &&
           media_view_matches_text(&call->dialog_id, instance->dialog_id) &&
           media_view_matches_text(&call->room_id, instance->room_id) &&
           media_view_matches_text(&call->call_id, instance->call_id);
}

static void media_state_callback(void *context, const ivr_call_ref_t *call,
                                 uint64_t attempt_generation,
                                 ivr_media_link_state_t state, int error_code) {
    ivr_worker_media_state_context_t *state_context =
        (ivr_worker_media_state_context_t *)context;
    ivr_worker_media_instance_t *instance;
    if (!state_context || !(instance = state_context->instance) ||
        !media_call_matches_instance(instance, call)) {
        return;
    }
    ivr_status_t status = ivr_media_supervisor_submit_state(
        instance->supervisor, state_context->link, attempt_generation, state,
        error_code);
    if (status == IVR_OK &&
        (state_context->link == IVR_MEDIA_LINK_WHIP ||
         state_context->link == IVR_MEDIA_LINK_WHEP)) {
        size_t index = (size_t)state_context->link - 1u;
        int connected = state == IVR_MEDIA_LINK_CONNECTED;
        int terminal = state == IVR_MEDIA_LINK_DISCONNECTED ||
                       state == IVR_MEDIA_LINK_FAILED ||
                       state == IVR_MEDIA_LINK_CLOSED;
        if (connected &&
            atomic_exchange_explicit(&instance->media_link_connected[index],
                                     1, memory_order_relaxed) == 0) {
            uint64_t links;
            if (ivr_worker_metrics_add_gauge(
                    &g_metrics, IVR_WORKER_GAUGE_MEDIA_LINKS_CONNECTED, 1u,
                    &links) == 0) {
                ivr_worker_metrics_set_gauge_max(
                    &g_metrics,
                    IVR_WORKER_GAUGE_MEDIA_LINKS_CONNECTED_HIGH_WATER,
                    links);
            }
        } else if (terminal &&
                   atomic_exchange_explicit(
                       &instance->media_link_connected[index], 0,
                       memory_order_relaxed) != 0) {
            uint64_t links;
            (void)ivr_worker_metrics_sub_gauge(
                &g_metrics, IVR_WORKER_GAUGE_MEDIA_LINKS_CONNECTED, 1u,
                &links);
        }
    }
}

static uint32_t media_restart(void *context, uint64_t attempt_generation) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)context;
    ivr_call_ref_t call;
    ivr_media_transport_t transport;
    uint32_t failed = 0;
    (void)attempt_generation;
    if (!instance || !instance->whip || !instance->whep) {
        return IVR_MEDIA_RESTART_WHIP_FAILED | IVR_MEDIA_RESTART_WHEP_FAILED;
    }
    media_instance_call(instance, &call);
    (void)ivr_whep_transport_stop(instance->whep, &call);
    ivr_whip_transport_get_transport(instance->whip, &transport);
    if (transport.stop) {
        (void)transport.stop(transport.context, &call);
    }
    if (ivr_whip_transport_start(instance->whip, &call) != IVR_OK) {
        failed |= IVR_MEDIA_RESTART_WHIP_FAILED;
    }
    {
        char participant[IVR_MEDIA_ID_CAPACITY];
        int length = snprintf(participant, sizeof(participant), "%s-rx",
                              instance->call_id);
        if (length < 0 || (size_t)length >= sizeof(participant) ||
            ivr_whep_transport_start(instance->whep, &call, participant) !=
                IVR_OK) {
            failed |= IVR_MEDIA_RESTART_WHEP_FAILED;
        }
    }
    return failed;
}

static void media_supervisor_event(void *context,
                                   ivr_media_reconnect_event_t event,
                                   uint64_t attempt_generation) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)context;
    ivr_call_ref_t call;
    ivr_event_view_t view;
    ivr_bytes_view_t event_type;
    ivr_bytes_view_t payload;
    char payload_json[96];
    const char *name = NULL;
    if (!instance || !g_worker) {
        return;
    }
    switch (event) {
        case IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED:
            name = "rtc.disconnected";
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_MEDIA_DISCONNECTED);
            break;
        case IVR_MEDIA_RECONNECT_EVENT_RECONNECTED:
            name = "rtc.reconnected";
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_MEDIA_RECONNECTED);
            break;
        case IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED:
            name = "rtc.retry_exhausted";
            ivr_worker_metrics_inc(
                &g_metrics, IVR_WORKER_METRIC_MEDIA_RETRY_EXHAUSTED);
            break;
        case IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED:
            name = "media.input_stalled";
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_MEDIA_INPUT_STALLED);
            break;
        default:
            return;
    }
    media_instance_call(instance, &call);
    snprintf(payload_json, sizeof(payload_json),
             "{\"attempt_generation\":%llu}",
             (unsigned long long)attempt_generation);
    memset(&view, 0, sizeof(view));
    event_type.data = name;
    event_type.size = strlen(name);
    payload.data = payload_json;
    payload.size = strlen(payload_json);
    view.event_type = event_type;
    view.call = call;
    view.sequence = 0;
    view.payload_json = payload;
    (void)enqueue_media_event_copy(NULL, &view);
}

static int log_transport_play_audio(void *ctx, const ivr_call_ref_t *call,
                                    const uint8_t *pcm, size_t len,
                                    uint32_t sample_rate) {
    ivr_worker_log_transport_t *transport =
        (ivr_worker_log_transport_t *)ctx;
    (void)call;
    (void)pcm;
    (void)sample_rate;
    if (!transport) {
        return -1;
    }
    transport->audio_frames++;
    transport->audio_bytes += len;
    return 0;
}

static int media_transport_stop(void *ctx, const ivr_call_ref_t *call) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    int result = 0;

    if (!instance || !call) {
        return -1;
    }
    if (instance->supervisor) {
        ivr_media_supervisor_stop(instance->supervisor);
    }
    if (instance->whep &&
        ivr_whep_transport_stop(instance->whep, call) != 0) {
        result = -1;
    }
    if (instance->whip) {
        ivr_media_transport_t transport;
        ivr_whip_transport_get_transport(instance->whip, &transport);
        if (!transport.stop || transport.stop(transport.context, call) != 0) {
            result = -1;
        }
    }
    if (!instance->rtc_enabled) {
        printf("[ivr_worker] media: stop_bot room=%.*s call=%.*s\n",
               (int)call->room_id.size, call->room_id.data,
               (int)call->call_id.size, call->call_id.data);
    }
    return result;
}

static int media_transport_play_audio(void *ctx, const ivr_call_ref_t *call,
                                      const uint8_t *pcm, size_t len,
                                      uint32_t sample_rate) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    if (!instance || !call) {
        return -1;
    }
    if (instance->whip) {
        ivr_media_transport_t transport;
        ivr_whip_transport_get_transport(instance->whip, &transport);
        return transport.play_audio(transport.context, call, pcm, len,
                                    sample_rate);
    }
    return log_transport_play_audio(&instance->log_transport, call, pcm, len,
                                    sample_rate);
}

/* Bot media facts are copied to the owner-loop event queue for Iris. */
static void media_bot_on_event(void *ctx, const ivr_event_view_t *event) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    ivr_event_view_t outbound;
    ivr_call_ref_t call;
    if (!instance || !event) {
        return;
    }
    if (event->event_type.size == sizeof("provider.error") - 1u &&
        memcmp(event->event_type.data, "provider.error",
               sizeof("provider.error") - 1u) == 0) {
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_PROVIDER_ERROR);
    }
    outbound = *event;
    media_instance_call(instance, &call);
    outbound.call = call;
    if (enqueue_media_event_copy(NULL, &outbound) != IVR_OK) {
        fprintf(stderr, "ivr_worker: rejected media event type=%.*s\n",
                (int)event->event_type.size, event->event_type.data);
    }
}

static ivr_status_t media_start_bot(void *ctx, const ivr_call_ref_t *call) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    if (!instance || !call) {
        return IVR_EINVAL;
    }
    if (instance->bot_ops.start_bot(instance->bot_ops.context, call) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (!instance->rtc_enabled) {
        return IVR_OK;
    }
    if (!instance->supervisor ||
        ivr_media_supervisor_start(instance->supervisor) != IVR_OK) {
        (void)instance->bot_ops.stop_bot(instance->bot_ops.context, call);
        return IVR_ESTATE;
    }
    return IVR_OK;
}

static ivr_status_t media_play_pcm(void *ctx, const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *text) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    return instance && instance->bot_ops.play_pcm
               ? instance->bot_ops.play_pcm(instance->bot_ops.context, call,
                                            text)
               : IVR_EINVAL;
}

static ivr_status_t media_cancel_input(void *ctx, const ivr_call_ref_t *call) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    ivr_status_t bot_status;
    ivr_status_t dtmf_status = IVR_OK;
    if (!instance || !instance->bot_ops.cancel_input) {
        return IVR_EINVAL;
    }
    bot_status = instance->bot_ops.cancel_input(instance->bot_ops.context,
                                                call);
    if (instance->dtmf_ingress && instance->input_active) {
        dtmf_status = ivr_dtmf_ingress_end_input(
            instance->dtmf_ingress, call, instance->input_generation);
    }
    instance->input_id[0] = '\0';
    instance->input_generation = 0;
    instance->input_active = 0;
    return bot_status != IVR_OK ? bot_status : dtmf_status;
}

static ivr_status_t media_stop_bot(void *ctx, const ivr_call_ref_t *call) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    return instance && instance->bot_ops.stop_bot
               ? instance->bot_ops.stop_bot(instance->bot_ops.context, call)
               : IVR_EINVAL;
}

static ivr_status_t media_begin_input(void *ctx, const ivr_call_ref_t *call,
                                      const ivr_bytes_view_t *input_id,
                                      uint64_t input_generation) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    char input_id_copy[IVR_DTMF_ID_MAX];
    if (!instance || !call || !input_id || !input_id->data ||
        input_id->size == 0 || input_id->size >= sizeof(input_id_copy) ||
        input_generation == 0) {
        return IVR_EINVAL;
    }
    if (!instance->bot_ops.begin_input || instance->input_active) {
        return IVR_EBUSY;
    }
    memcpy(input_id_copy, input_id->data, input_id->size);
    input_id_copy[input_id->size] = '\0';
    if (instance->dtmf_ingress &&
        ivr_dtmf_ingress_begin_input(instance->dtmf_ingress, call,
                                     input_id_copy,
                                     input_generation) != IVR_OK) {
        return IVR_ESTATE;
    }
    ivr_status_t status = instance->bot_ops.begin_input(
        instance->bot_ops.context, call, input_id, input_generation);
    if (status != IVR_OK) {
        if (instance->dtmf_ingress) {
            (void)ivr_dtmf_ingress_end_input(instance->dtmf_ingress, call,
                                             input_generation);
        }
        return status;
    }
    memcpy(instance->input_id, input_id_copy, input_id->size + 1u);
    instance->input_generation = input_generation;
    instance->input_active = 1;
    return IVR_OK;
}

static ivr_status_t media_end_input(void *ctx, const ivr_call_ref_t *call,
                                    const ivr_bytes_view_t *input_id,
                                    uint64_t input_generation) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    ivr_status_t bot_status;
    ivr_status_t dtmf_status = IVR_OK;
    if (!instance || !call || !input_id || !input_id->data ||
        input_generation == 0 || !instance->input_active ||
        instance->input_generation != input_generation ||
        input_id->size != strlen(instance->input_id) ||
        memcmp(input_id->data, instance->input_id, input_id->size) != 0 ||
        !instance->bot_ops.end_input) {
        return IVR_EINVAL;
    }
    bot_status = instance->bot_ops.end_input(instance->bot_ops.context, call,
                                             input_id, input_generation);
    if (instance->dtmf_ingress) {
        dtmf_status = ivr_dtmf_ingress_end_input(instance->dtmf_ingress, call,
                                                 input_generation);
    }
    instance->input_id[0] = '\0';
    instance->input_generation = 0;
    instance->input_active = 0;
    return bot_status != IVR_OK ? bot_status : dtmf_status;
}

static int whep_audio_to_bot(void *ctx, const ivr_call_ref_t *call,
                             const uint8_t *pcm, size_t length,
                             uint32_t sample_rate, uint64_t timestamp) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    (void)sample_rate;
    (void)timestamp;
    if (!instance || !instance->bot) {
        return -1;
    }
    return ivr_media_bot_feed_caller_audio(instance->bot, call, pcm, length) ==
                   IVR_OK
               ? 0
               : -1;
}

static int whep_rtp_to_dtmf(void *ctx, const ivr_call_ref_t *call,
                            uint8_t payload_type, uint32_t rtp_timestamp,
                            const uint8_t *payload, size_t payload_length,
                            uint64_t source_generation) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    ivr_dtmf_input_t input;
    ivr_event_view_t event;
    ivr_call_ref_t event_call;
    char digit[2];
    char payload_json[160];
    static const ivr_bytes_view_t event_type = {"dtmf.final", 10};
    int length;
    (void)payload_type;
    if (!instance || !instance->dtmf_ingress || !call || !g_worker) {
        return -1;
    }
    if (ivr_dtmf_ingress_submit_rtp(
            instance->dtmf_ingress, call, source_generation, rtp_timestamp,
            payload, payload_length, &input) != IVR_OK) {
        return -1;
    }
    length = snprintf(payload_json, sizeof(payload_json),
                      "{\"digit\":\"%c\",\"duration\":%u,"
                      "\"source_generation\":%llu}",
                      input.digit, (unsigned int)input.duration,
                      (unsigned long long)input.source_generation);
    if (length < 0 || (size_t)length >= sizeof(payload_json)) {
        return -1;
    }
    memset(&event_call, 0, sizeof(event_call));
    event_call.room_id.data = input.room_id;
    event_call.room_id.size = strlen(input.room_id);
    event_call.call_id.data = input.call_id;
    event_call.call_id.size = strlen(input.call_id);
    event_call.call_generation = input.call_generation;
    event_call.tenant_id.data = instance->tenant_id;
    event_call.tenant_id.size = strlen(instance->tenant_id);
    event_call.provider_session_id.data = input.provider_session_id;
    event_call.provider_session_id.size = strlen(input.provider_session_id);
    event_call.dialog_id.data = input.dialog_id;
    event_call.dialog_id.size = strlen(input.dialog_id);
    digit[0] = input.digit;
    digit[1] = '\0';
    memset(&event, 0, sizeof(event));
    event.event_type = event_type;
    event.call = event_call;
    event.sequence = 0;
    event.payload_json.data = payload_json;
    event.payload_json.size = (size_t)length;
    event.input_id.data = input.input_id;
    event.input_id.size = strlen(input.input_id);
    event.input_value.data = digit;
    event.input_value.size = 1;
    return enqueue_media_event_copy(NULL, &event) == IVR_OK ? 0 : -1;
}

static ivr_status_t media_factory_create(void *ctx, const ivr_call_ref_t *call,
                                         ivr_media_port_ops_t *out_media,
                                         void **out_instance) {
    ivr_worker_media_instance_t *instance;
    ivr_media_bot_config_t bot_config;
    ivr_media_transport_t media_transport;

    ivr_speech_session_factory_ops_t *speech_factory =
        (ivr_speech_session_factory_ops_t *)ctx;
    if (!call || !out_media || !out_instance) {
        return IVR_EINVAL;
    }
    instance = (ivr_worker_media_instance_t *)calloc(1, sizeof(*instance));
    if (!instance) {
        return IVR_ENOSPC;
    }
    if (media_instance_copy_call(instance, call) != 0) {
        free(instance);
        return IVR_EINVAL;
    }
    atomic_init(&instance->media_link_connected[0], 0);
    atomic_init(&instance->media_link_connected[1], 0);
    instance->rtc_enabled = g_sfu_base_url && g_sfu_base_url[0];
    if (g_remote_configured &&
        (!speech_factory || !speech_factory->create ||
         speech_factory->create(speech_factory->context, call,
                                &instance->speech) != IVR_OK)) {
        free(instance);
        return IVR_ESTATE;
    }
    memset(&media_transport, 0, sizeof(media_transport));
    media_transport.context = instance;
    media_transport.play_audio = media_transport_play_audio;
    media_transport.stop = media_transport_stop;
    memset(&bot_config, 0, sizeof(bot_config));
    bot_config.tts_provider = ivr_speech_session_tts(instance->speech);
    bot_config.asr_provider = ivr_speech_session_asr(instance->speech);
    bot_config.transport = media_transport;
    bot_config.sample_rate = g_media_sample_rate;
    bot_config.on_event = media_bot_on_event;
    bot_config.event_ctx = instance;
    if (ivr_media_bot_create(&bot_config, &instance->bot) != IVR_OK) {
        if (speech_factory && speech_factory->destroy) {
            speech_factory->destroy(speech_factory->context,
                                    instance->speech);
        }
        free(instance);
        return IVR_ENOSPC;
    }
    ivr_media_bot_get_ops(instance->bot, &instance->bot_ops);
    if (instance->rtc_enabled) {
        ivr_whip_transport_config_t whip_config;
        ivr_whep_transport_config_t whep_config;
        memset(&whip_config, 0, sizeof(whip_config));
        whip_config.sfu_base_url = g_sfu_base_url;
        whip_config.media_token = g_sfu_media_token;
        whip_config.ca_file = g_sfu_ca_file;
        whip_config.cert_file = g_sfu_cert_file;
        whip_config.key_file = g_sfu_key_file;
        whip_config.key_password = g_sfu_key_password;
        whip_config.allow_plaintext_loopback =
            g_sfu_allow_plaintext_loopback;
        whip_config.allow_loopback = g_sfu_allow_loopback;
        whip_config.sample_rate = g_media_sample_rate;
        whip_config.http_timeout_ms = g_sfu_http_timeout_ms;
        instance->whip_state.instance = instance;
        instance->whip_state.link = IVR_MEDIA_LINK_WHIP;
        whip_config.on_state = media_state_callback;
        whip_config.state_context = &instance->whip_state;
        memset(&whep_config, 0, sizeof(whep_config));
        whep_config.sfu_base_url = g_sfu_base_url;
        whep_config.media_token = g_sfu_media_token;
        whep_config.ca_file = g_sfu_ca_file;
        whep_config.cert_file = g_sfu_cert_file;
        whep_config.key_file = g_sfu_key_file;
        whep_config.key_password = g_sfu_key_password;
        whep_config.allow_plaintext_loopback =
            g_sfu_allow_plaintext_loopback;
        whep_config.allow_loopback = g_sfu_allow_loopback;
        whep_config.sample_rate = g_media_sample_rate;
        whep_config.http_timeout_ms = g_sfu_http_timeout_ms;
        whep_config.input_inactivity_timeout_ms =
            g_media_input_inactivity_timeout_ms;
        instance->whep_state.instance = instance;
        instance->whep_state.link = IVR_MEDIA_LINK_WHEP;
        whep_config.on_state = media_state_callback;
        whep_config.state_context = &instance->whep_state;
        whep_config.on_audio = whep_audio_to_bot;
        whep_config.audio_context = instance;
        whep_config.telephone_event_payload_type = 126u;
        whep_config.on_rtp = whep_rtp_to_dtmf;
        whep_config.rtp_context = instance;
        {
            ivr_dtmf_ingress_config_t dtmf_config;
            memset(&dtmf_config, 0, sizeof(dtmf_config));
            dtmf_config.window_capacity = 1;
            if (ivr_dtmf_ingress_create(&dtmf_config,
                                        &instance->dtmf_ingress) != IVR_OK) {
                ivr_media_bot_destroy(instance->bot);
                if (speech_factory && speech_factory->destroy) {
                    speech_factory->destroy(speech_factory->context,
                                            instance->speech);
                }
                free(instance);
                return IVR_ENOSPC;
            }
        }
        if (ivr_whip_transport_create(&whip_config, &instance->whip) != IVR_OK ||
            ivr_whep_transport_create(&whep_config, &instance->whep) != IVR_OK) {
            ivr_whep_transport_destroy(instance->whep);
            ivr_whip_transport_destroy(instance->whip);
            ivr_dtmf_ingress_destroy(instance->dtmf_ingress);
            ivr_media_bot_destroy(instance->bot);
            if (speech_factory && speech_factory->destroy) {
                speech_factory->destroy(speech_factory->context,
                                        instance->speech);
            }
            free(instance);
            return IVR_ENOSPC;
        }
        ivr_media_supervisor_config_t supervisor_config;
        memset(&supervisor_config, 0, sizeof(supervisor_config));
        supervisor_config.reconnect.max_attempts =
            IVR_MEDIA_RECONNECT_MAX_ATTEMPTS;
        supervisor_config.reconnect.initial_backoff_ms =
            IVR_MEDIA_RECONNECT_INITIAL_BACKOFF_MS;
        supervisor_config.reconnect.max_backoff_ms =
            IVR_MEDIA_RECONNECT_MAX_BACKOFF_MS;
        supervisor_config.reconnect.total_deadline_ms =
            IVR_MEDIA_RECONNECT_DEADLINE_MS;
        supervisor_config.restart = media_restart;
        supervisor_config.restart_context = instance;
        supervisor_config.on_event = media_supervisor_event;
        supervisor_config.event_context = instance;
        if (ivr_media_supervisor_create(&supervisor_config,
                                        &instance->supervisor) != IVR_OK) {
            ivr_whep_transport_destroy(instance->whep);
            ivr_whip_transport_destroy(instance->whip);
            ivr_dtmf_ingress_destroy(instance->dtmf_ingress);
            ivr_media_bot_destroy(instance->bot);
            if (speech_factory && speech_factory->destroy) {
                speech_factory->destroy(speech_factory->context,
                                        instance->speech);
            }
            free(instance);
            return IVR_ENOSPC;
        }
    }
    memset(out_media, 0, sizeof(*out_media));
    out_media->abi_version = IVR_WORKER_ABI_VERSION;
    out_media->context = instance;
    out_media->start_bot = media_start_bot;
    out_media->play_pcm = media_play_pcm;
    out_media->cancel_input = media_cancel_input;
    out_media->stop_bot = media_stop_bot;
    out_media->begin_input = media_begin_input;
    out_media->end_input = media_end_input;
    *out_instance = instance;
    if (instance->rtc_enabled) {
        instance->counted_media_peers = 2u;
        metrics_media_peers_add(instance->counted_media_peers);
    }
    return IVR_OK;
}

static void media_factory_destroy(void *ctx, void *opaque_instance) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)opaque_instance;
    ivr_speech_session_factory_ops_t *speech_factory =
        (ivr_speech_session_factory_ops_t *)ctx;
    if (!instance) {
        return;
    }
    if (instance->counted_media_peers > 0) {
        metrics_media_peers_remove(instance->counted_media_peers);
        instance->counted_media_peers = 0;
    }
    /* Stop the restart/event owner first, then quiesce transport callback
       threads while their supervisor target is still allocated. */
    ivr_media_supervisor_stop(instance->supervisor);
    ivr_whep_transport_destroy(instance->whep);
    ivr_whip_transport_destroy(instance->whip);
    {
        uint32_t connected_links = 0u;
        uint64_t remaining;
        for (size_t index = 0; index < 2u; ++index) {
            if (atomic_exchange_explicit(
                    &instance->media_link_connected[index], 0,
                    memory_order_relaxed) != 0) {
                connected_links++;
            }
        }
        if (connected_links > 0u) {
            (void)ivr_worker_metrics_sub_gauge(
                &g_metrics, IVR_WORKER_GAUGE_MEDIA_LINKS_CONNECTED,
                connected_links, &remaining);
        }
    }
    ivr_media_supervisor_destroy(instance->supervisor);
    ivr_dtmf_ingress_destroy(instance->dtmf_ingress);
    ivr_media_bot_destroy(instance->bot);
    if (speech_factory && speech_factory->destroy) {
        speech_factory->destroy(speech_factory->context, instance->speech);
    }
    free(instance);
}

static void configure_media_from_environment(void) {
    ivr_http_media_client_config_t http_config =
        IVR_HTTP_MEDIA_CLIENT_CONFIG_INIT;
    ivr_http_media_client_t *validation_client = NULL;

    g_sfu_base_url = getenv("IVR_SFU_BASE_URL");
    g_sfu_media_token = getenv("IVR_SFU_MEDIA_TOKEN");
    g_sfu_ca_file = getenv("IVR_SFU_CA_FILE");
    g_sfu_cert_file = getenv("IVR_SFU_CERT_FILE");
    g_sfu_key_file = getenv("IVR_SFU_KEY_FILE");
    g_sfu_key_password = getenv("IVR_SFU_KEY_PASSWORD");
    g_sfu_allow_plaintext_loopback =
        env_int("IVR_SFU_ALLOW_PLAINTEXT_LOOPBACK", 0);
    g_sfu_allow_loopback = env_int("IVR_SFU_ALLOW_LOOPBACK", 0);
    g_sfu_http_timeout_ms =
        (uint64_t)env_int("IVR_SFU_HTTP_TIMEOUT_MS", 0);
    g_media_sample_rate = (uint32_t)env_int(
        "IVR_MEDIA_SAMPLE_RATE", IVR_MEDIA_DEFAULT_SAMPLE_RATE);
    g_media_input_inactivity_timeout_ms = (uint64_t)env_int(
        "IVR_MEDIA_INPUT_INACTIVITY_TIMEOUT_MS", 0);
    if (g_sfu_base_url && g_sfu_base_url[0]) {
        http_config.base_url = g_sfu_base_url;
        http_config.media_token = g_sfu_media_token;
        http_config.ca_file = g_sfu_ca_file;
        http_config.cert_file = g_sfu_cert_file;
        http_config.key_file = g_sfu_key_file;
        http_config.key_password = g_sfu_key_password;
        http_config.timeout_ms = g_sfu_http_timeout_ms;
        http_config.allow_plaintext_loopback =
            g_sfu_allow_plaintext_loopback;
        if (ivr_http_media_client_create(&http_config, &validation_client) !=
            0) {
            fprintf(stderr, "ivr_worker: invalid SFU HTTPS configuration\n");
            g_env_parse_failed = 1;
            return;
        }
        ivr_http_media_client_destroy(validation_client);
        printf("[ivr_worker] media: WebRTC WHIP/WHEP endpoint enabled\n");
    } else {
        printf("[ivr_worker] media: SFU endpoint not configured; dry-run/logging transport\n");
    }
}

/* ------------------------------------------------------------------ */
/* DEALER command/result ingress                                      */
/* ------------------------------------------------------------------ */

static const char *media_error_code(ivr_status_t status) {
    switch (status) {
    case IVR_ENOSPC:
        return "media.capacity";
    case IVR_ECLOSED:
        return "media.draining";
    case IVR_EINVAL:
        return "media.invalid";
    case IVR_ESTATE:
        return "media.state";
    case IVR_EVERSION:
        return "media.version";
    case IVR_ESTALE:
        return "media.deadline";
    case IVR_EAUTH:
        return "media.scope";
    case IVR_EBUSY:
        return "media.busy";
    default:
        return "media.rejected";
    }
}

static int media_command_in_scope(const ivr_media_command_t *command) {
    if (g_config.tenant_id && g_config.tenant_id[0] &&
        strcmp(g_config.tenant_id, command->tenant_id) != 0) {
        return 0;
    }
    if (g_config.room_scope && g_config.room_scope[0] &&
        !ivr_acl_scope_allows(g_config.room_scope, command->room_id)) {
        return 0;
    }
    if (g_config.call_scope && g_config.call_scope[0] &&
        !ivr_acl_scope_allows(g_config.call_scope, command->call_id)) {
        return 0;
    }
    return 1;
}

static ivr_status_t execute_media_command(const ivr_media_command_t *command,
                                          uint64_t received_at_ms) {
    ivr_call_ref_t call;
    ivr_media_operation_t operation;
    ivr_bytes_view_t text;
    ivr_bytes_view_t input_id;

    if (!command || !media_command_in_scope(command)) {
        return command ? IVR_EAUTH : IVR_EINVAL;
    }
    memset(&call, 0, sizeof(call));
    call.tenant_id.data = command->tenant_id;
    call.tenant_id.size = strlen(command->tenant_id);
    call.provider_session_id.data = command->provider_session_id;
    call.provider_session_id.size = strlen(command->provider_session_id);
    call.dialog_id.data = command->dialog_id;
    call.dialog_id.size = strlen(command->dialog_id);
    call.room_id.data = command->room_id;
    call.room_id.size = strlen(command->room_id);
    call.call_id.data = command->call_id;
    call.call_id.size = strlen(command->call_id);
    call.call_generation = command->call_generation;

    memset(&operation, 0, sizeof(operation));
    operation.call = call;
    operation.operation_generation = command->operation_generation;
    if (command->deadline_timeout_ms > UINT64_MAX - received_at_ms) {
        return IVR_EINVAL;
    }
    operation.deadline_ms = received_at_ms + command->deadline_timeout_ms;
    text.data = command->text;
    text.size = strlen(command->text);
    input_id.data = command->input_id;
    input_id.size = strlen(command->input_id);

    switch (command->kind) {
    case IVR_MEDIA_COMMAND_SESSION_OPEN:
        return ivr_worker_open_media_operation(g_worker, &operation);
    case IVR_MEDIA_COMMAND_PLAY:
        return ivr_worker_play(g_worker, &operation, &text);
    case IVR_MEDIA_COMMAND_INPUT_START:
        return ivr_worker_begin_input(g_worker, &operation, &input_id,
                                      command->input_generation);
    case IVR_MEDIA_COMMAND_INPUT_STOP:
        return ivr_worker_end_input(g_worker, &operation, &input_id,
                                    command->input_generation);
    case IVR_MEDIA_COMMAND_CANCEL:
        return ivr_worker_cancel_input(g_worker, &operation, &input_id,
                                       command->input_generation);
    case IVR_MEDIA_COMMAND_SESSION_CLOSE:
        return ivr_worker_close_media_call(g_worker, &call);
    default:
        return IVR_EINVAL;
    }
}

static void reject_legacy_session_command(const uint8_t *frame, size_t len,
                                          uint16_t type_id) {
    DataBind *codec = ensure_codec();
    if (!codec) {
        return;
    }
    if (type_id == IVR_TYPE_CALL_RELEASE_COMMAND_V1) {
        ivr_call_release_t release;
        if (ivr_flowmq_gateway_decode_release(codec, frame, len, &release) ==
            IVR_OK && strcmp(release.worker_id, g_config.worker_id) == 0) {
            (void)ivr_flowmq_gateway_send_release_result(
                g_gateway, &release, IVR_EVERSION, "media.protocol_required",
                "use typed media commands");
        }
        return;
    }
    {
        ivr_call_dispatch_t dispatch;
        if (ivr_flowmq_gateway_decode_dispatch(codec, frame, len, &dispatch) ==
            IVR_OK && strcmp(dispatch.worker_id, g_config.worker_id) == 0) {
            (void)ivr_flowmq_gateway_send_dispatch_result(
                g_gateway, &dispatch, IVR_EVERSION,
                "media.protocol_required", "use typed media commands");
        }
    }
}

static void handle_inventory_query(const uint8_t *frame, size_t len) {
    ivr_worker_inventory_request_t request;
    ivr_worker_inventory_envelope_t result;
    DataBind *codec = ensure_codec();
    ivr_status_t status;
    if (!codec) return;
    status = ivr_flowmq_gateway_decode_inventory_query(codec, frame, len,
                                                       &request);
    if (!request.message_id[0] || !request.worker_id[0] ||
        strcmp(request.worker_id, g_config.worker_id) != 0) {
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_REPLY_INVALID);
        return;
    }
    memset(&result, 0, sizeof(result));
    snprintf(result.message_id, sizeof(result.message_id), "%s",
             request.message_id);
    snprintf(result.worker_id, sizeof(result.worker_id), "%s",
             request.worker_id);
    if (status == IVR_OK) {
        status = ivr_worker_query_inventory(g_worker, &request.query,
                                            &result.page);
    }
    result.status_code = status;
    if (status != IVR_OK) {
        result.page.inventory_version = request.query.inventory_version;
        snprintf(result.error_code, sizeof(result.error_code), "%s",
                 status == IVR_EVERSION
                     ? "inventory.version_unsupported"
                     : status == IVR_ESTALE
                           ? "inventory.snapshot_stale"
                           : status == IVR_EINVAL
                                 ? "inventory.query_invalid"
                                 : "inventory.query_failed");
        snprintf(result.error_message, sizeof(result.error_message), "%s",
                 status == IVR_EVERSION
                     ? "unsupported inventory version"
                     : status == IVR_ESTALE
                           ? "inventory revision changed; restart pagination"
                           : status == IVR_EINVAL
                                 ? "invalid inventory cursor or limit"
                                 : "worker inventory unavailable");
    }
    status = ivr_flowmq_gateway_send_inventory_page(g_gateway, &result);
    if (status != IVR_OK) {
        fprintf(stderr,
                "ivr_worker: inventory page send failed message=%s status=%d\n",
                request.message_id, status);
    }
}

static void handle_reply(const uint8_t *frame, size_t len) {
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_REPLY_INVALID);
        printf("[ivr_worker] reply: malformed frame\n");
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        info.schema_type_id == IVR_TYPE_WORKER_MEDIA_INVENTORY_QUERY_V1) {
        handle_inventory_query(frame, len);
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        info.schema_type_id >= IVR_TYPE_MEDIA_SESSION_OPEN_COMMAND_V1 &&
        info.schema_type_id <= IVR_TYPE_MEDIA_SESSION_CLOSE_COMMAND_V1) {
        ivr_media_command_t command;
        DataBind *codec = ensure_codec();
        if (!codec ||
            ivr_flowmq_gateway_decode_media_command(codec, frame, len,
                                                     &command) !=
                IVR_OK) {
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_REPLY_INVALID);
            printf("[ivr_worker] media command: decode failed\n");
            return;
        }
        if (strcmp(command.worker_id, g_config.worker_id) != 0) {
            printf("[ivr_worker] media command: not for this worker (%s)\n",
                   command.worker_id);
            return;
        }
        {
            uint64_t received_at_ms = salts_monotonic_ms();
            ivr_status_t rc = execute_media_command(&command, received_at_ms);
            const char *error_code = rc == IVR_OK ? "" : media_error_code(rc);
            const char *error_message =
                rc == IVR_OK ? "" : "media command rejected";
            if (command.kind == IVR_MEDIA_COMMAND_SESSION_OPEN ||
                command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE) {
                ivr_worker_health_snapshot_t health;
                if (refresh_health_capacity(&health) != 0) {
                    fprintf(stderr,
                            "ivr_worker: media capacity refresh failed\n");
                }
            }
            if (ivr_flowmq_gateway_send_media_result(
                    g_gateway, &command, rc, error_code, error_message) !=
                IVR_OK) {
                fprintf(stderr,
                        "ivr_worker: media result send failed message=%s\n",
                        command.message_id);
            }
            printf("[ivr_worker] media command session=%s operation=%llu status=%d\n",
                   command.provider_session_id,
                   (unsigned long long)command.operation_generation, rc);
        }
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        (info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V1 ||
         info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V2 ||
         info.schema_type_id == IVR_TYPE_CALL_RELEASE_COMMAND_V1)) {
        reject_legacy_session_command(frame, len, info.schema_type_id);
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_IVR_COMMAND_RESULT_V1) {
        /* The media worker never originates RoomService business commands. */
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_REPLY_INVALID);
        printf("[ivr_worker] rejected legacy business result\n");
        return;
    }
    printf("[ivr_worker] reply: type=%s kind=%d\n",
           ivr_frame_type_name(info.schema_type_id)
               ? ivr_frame_type_name(info.schema_type_id)
               : "?",
           info.kind);
    if (info.schema_type_id == IVR_TYPE_WORKER_SYNC_RESULT_V1) {
        DataBind *codec = ensure_codec();
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindObject *obj = NULL;
        int status = -1;
        const char *message_id = "";
        if (codec &&
            data_bind_object_from_bin(codec, "WorkerSyncResultV1",
                                      frame + IVR_FRAME_HEADER_SIZE,
                                      len - IVR_FRAME_HEADER_SIZE, &obj,
                                      &err) == DATA_BIND_OK) {
            const DataBindValue *root = data_bind_object_value(obj);
            const DataBindValue *v = data_bind_value_get(root, "status_code");
            if (v) {
                status = data_bind_value_as_int(v);
            }
            v = data_bind_value_get(root, "message_id");
            if (v && data_bind_value_as_string(v)) {
                message_id = data_bind_value_as_string(v);
            }
            if (status != IVR_OK && strncmp(message_id, "ws-", 3) == 0 &&
                ivr_worker_active_sessions(g_worker) > 0) {
                fprintf(stderr,
                        "ivr_worker: active media calls have no registered route; draining\n");
                ivr_worker_begin_drain(g_worker);
                atomic_store_explicit(&g_running, 0, memory_order_release);
            }
            data_bind_object_free(obj);
        } else {
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_REPLY_INVALID);
        }
        if (status == IVR_OK) {
            int command_ready = g_control_state.connected;
            g_synced = 1;
            health_publish(1, 1, command_ready, command_ready, 1,
                           g_remote_configured,
                           g_sfu_base_url && g_sfu_base_url[0],
                           0, command_ready,
                           command_ready ? "ready" : "channel not connected");
            printf("[ivr_worker] worker.sync acknowledged (readiness)\n");
        } else {
            int command_ready = g_control_state.connected;
            g_synced = 0;
            health_publish(1, 1, command_ready, command_ready, 0,
                           g_remote_configured,
                           g_sfu_base_url && g_sfu_base_url[0],
                           0, 0, "worker.sync rejected");
            printf("[ivr_worker] worker.sync rejected (status=%d)\n", status);
        }
    }
}

static void on_reply(void *ctx, const uint8_t *frame, size_t len) {
    disruptor_cursor_t cursor;
    ivr_worker_reply_entry_t *entry;
    (void)ctx;

    if (!atomic_load_explicit(&g_accept_replies, memory_order_acquire) ||
        !g_reply_queue || !frame || len == 0 ||
        len > IVR_WORKER_REPLY_FRAME_CAPACITY) {
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_REPLY_INVALID);
        fprintf(stderr, "ivr_worker: rejected invalid or closed FlowMQ reply\n");
        return;
    }
    if (!disruptor_publisher_try_claim(g_reply_queue, &cursor)) {
        ivr_worker_metrics_inc(&g_metrics,
                               IVR_WORKER_METRIC_REPLY_QUEUE_FULL);
        fprintf(stderr, "ivr_worker: FlowMQ reply queue full\n");
        return;
    }
    entry = (ivr_worker_reply_entry_t *)disruptor_acquire_entry(g_reply_queue,
                                                                &cursor);
    entry->length = len;
    memcpy(entry->frame, frame, len);
    metrics_reply_queue_add(len);
    (void)disruptor_publisher_publish(g_reply_queue, &cursor);
}

static void on_command_connection(void *ctx, int connected) {
    const ivr_worker_control_event_t event = {
        IVR_WORKER_CONTROL_CONNECTION, connected};
    (void)ctx;
    if (ivr_worker_control_queue_publish(g_control_queue, &event) != 0) {
        ivr_worker_metrics_inc(&g_metrics,
                               IVR_WORKER_METRIC_CONTROL_QUEUE_FULL);
        atomic_store_explicit(&g_running, 0, memory_order_release);
        fprintf(stderr,
                "ivr_worker: connection control queue closed or full; stopping\n");
    }
}

static int owner_advance_epoch(void *context, uint64_t next_epoch) {
    ivr_worker_t *worker = (ivr_worker_t *)context;
    return worker && ivr_worker_advance_epoch(worker, next_epoch) == IVR_OK
               ? 0
               : -1;
}

static void process_control_events(void) {
    ivr_worker_control_event_t event;
    int pop_result;
    while ((pop_result = ivr_worker_control_queue_try_pop(g_control_queue,
                                                           &event)) > 0) {
        int previous = g_control_state.connected;
        if (ivr_worker_control_apply(&g_control_state, &event,
                                     owner_advance_epoch, g_worker) != 0) {
            fprintf(stderr,
                    "ivr_worker: owner control transition failed; stopping\n");
            atomic_store_explicit(&g_running, 0, memory_order_release);
            return;
        }
        if (event.kind == IVR_WORKER_CONTROL_DRAIN) {
            atomic_store_explicit(&g_running, 0, memory_order_release);
            continue;
        }
        if (previous != g_control_state.connected) {
            ivr_worker_metrics_inc(
                &g_metrics,
                g_control_state.connected
                    ? IVR_WORKER_METRIC_COMMAND_CONNECTED
                    : IVR_WORKER_METRIC_COMMAND_DISCONNECTED);
        }
    }
    if (pop_result < 0) {
        atomic_store_explicit(&g_running, 0, memory_order_release);
    }
}

static void process_replies(void) {
    disruptor_cursor_t cursor;

    while (g_reply_queue &&
           disruptor_worker_try_claim(g_reply_queue, &cursor)) {
        const ivr_worker_reply_entry_t *entry =
            (const ivr_worker_reply_entry_t *)disruptor_show_entry(
                g_reply_queue, &cursor);
        handle_reply(entry->frame, entry->length);
        size_t entry_length = entry->length;
        disruptor_worker_release_entry(g_reply_queue, &cursor);
        metrics_reply_queue_remove(entry_length);
    }
}

static int reply_queue_init(void) {
    disruptor_config_t config;
    memset(&config, 0, sizeof(config));
    config.entry_size = sizeof(ivr_worker_reply_entry_t);
    config.capacity = IVR_WORKER_REPLY_QUEUE_CAPACITY;
    config.consumer_capacity = 1;
    config.mode = DISRUPTOR_MODE_WORKER_POOL;
    g_reply_queue = disruptor_create(&config);
    if (!g_reply_queue) {
        return -1;
    }
    ivr_worker_metrics_set_gauge(&g_metrics,
                                 IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS, 0);
    ivr_worker_metrics_set_gauge(&g_metrics,
                                 IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, 0);
    atomic_store_explicit(&g_accept_replies, 1, memory_order_release);
    return 0;
}

static void reply_queue_destroy(void) {
    atomic_store_explicit(&g_accept_replies, 0, memory_order_release);
    if (g_reply_queue) {
        disruptor_destroy(g_reply_queue);
        g_reply_queue = NULL;
    }
    ivr_worker_metrics_set_gauge(&g_metrics,
                                 IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS, 0);
    ivr_worker_metrics_set_gauge(&g_metrics,
                                 IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES, 0);
}

static int copy_event_view(char *out, size_t capacity,
                           const ivr_bytes_view_t *view) {
    if (!out || capacity == 0 || !view ||
        (view->size > 0 && !view->data) || view->size >= capacity) {
        return -1;
    }
    if (view->size > 0) {
        memcpy(out, view->data, view->size);
    }
    out[view->size] = '\0';
    return 0;
}

static ivr_status_t enqueue_media_event_copy(
    void *context, const ivr_event_view_t *event) {
    disruptor_cursor_t cursor;
    ivr_worker_media_event_entry_t *entry;
    ivr_worker_media_event_entry_t value;
    uint64_t sequence;
    (void)context;

    if (!event || !g_media_event_queue ||
        !atomic_load_explicit(&g_accept_media_events, memory_order_acquire) ||
        !event->call.tenant_id.data || event->call.tenant_id.size == 0 ||
        !event->call.provider_session_id.data ||
        event->call.provider_session_id.size == 0 ||
        !event->call.dialog_id.data || event->call.dialog_id.size == 0 ||
        !event->event_type.data || event->event_type.size == 0) {
        return IVR_ECLOSED;
    }
    memset(&value, 0, sizeof(value));
    sequence = atomic_fetch_add_explicit(&g_media_event_sequence, 1,
                                         memory_order_relaxed) + 1;
    if (event->event_id.size > 0) {
        if (copy_event_view(value.event_id, sizeof(value.event_id),
                            &event->event_id) != 0) {
            return IVR_ENOSPC;
        }
    } else {
        int written = snprintf(value.event_id, sizeof(value.event_id),
                               "media-%s-%llu", g_instance_id,
                               (unsigned long long)sequence);
        if (written <= 0 || (size_t)written >= sizeof(value.event_id)) {
            return IVR_ENOSPC;
        }
    }
    if (copy_event_view(value.tenant_id, sizeof(value.tenant_id),
                        &event->call.tenant_id) != 0 ||
        copy_event_view(value.provider_session_id,
                        sizeof(value.provider_session_id),
                        &event->call.provider_session_id) != 0 ||
        copy_event_view(value.dialog_id, sizeof(value.dialog_id),
                        &event->call.dialog_id) != 0 ||
        copy_event_view(value.event_type, sizeof(value.event_type),
                        &event->event_type) != 0 ||
        copy_event_view(value.room_id, sizeof(value.room_id),
                        &event->call.room_id) != 0 ||
        copy_event_view(value.call_id, sizeof(value.call_id),
                        &event->call.call_id) != 0 ||
        copy_event_view(value.input_id, sizeof(value.input_id),
                        &event->input_id) != 0 ||
        copy_event_view(value.input_value, sizeof(value.input_value),
                        &event->input_value) != 0 ||
        copy_event_view(value.payload_json, sizeof(value.payload_json),
                        &event->payload_json) != 0) {
        return IVR_ENOSPC;
    }
    value.call_generation = event->call.call_generation;
    value.expected_room_version = event->call.expected_room_version;
    value.sequence = sequence;
    if (!disruptor_publisher_try_claim(g_media_event_queue, &cursor)) {
        return IVR_ENOSPC;
    }
    entry = (ivr_worker_media_event_entry_t *)disruptor_acquire_entry(
        g_media_event_queue, &cursor);
    *entry = value;
    return disruptor_publisher_publish(g_media_event_queue, &cursor)
               ? IVR_OK
               : IVR_ESTATE;
}

static int media_event_queue_init(void) {
    disruptor_config_t config;
    memset(&config, 0, sizeof(config));
    config.entry_size = sizeof(ivr_worker_media_event_entry_t);
    config.capacity = IVR_WORKER_MEDIA_EVENT_QUEUE_CAPACITY;
    config.consumer_capacity = 1;
    config.mode = DISRUPTOR_MODE_WORKER_POOL;
    g_media_event_queue = disruptor_create(&config);
    if (!g_media_event_queue) {
        return -1;
    }
    atomic_store_explicit(&g_media_event_sequence, 0, memory_order_release);
    atomic_store_explicit(&g_accept_media_events, 1, memory_order_release);
    return 0;
}

static void process_media_events(void) {
    disruptor_cursor_t cursor;
    while (g_media_event_queue &&
           disruptor_worker_try_claim(g_media_event_queue, &cursor)) {
        const ivr_worker_media_event_entry_t *entry =
            (const ivr_worker_media_event_entry_t *)disruptor_show_entry(
                g_media_event_queue, &cursor);
        ivr_event_view_t event;
        memset(&event, 0, sizeof(event));
        event.event_id.data = entry->event_id;
        event.event_id.size = strlen(entry->event_id);
        event.call.tenant_id.data = entry->tenant_id;
        event.call.tenant_id.size = strlen(entry->tenant_id);
        event.call.provider_session_id.data = entry->provider_session_id;
        event.call.provider_session_id.size =
            strlen(entry->provider_session_id);
        event.call.dialog_id.data = entry->dialog_id;
        event.call.dialog_id.size = strlen(entry->dialog_id);
        event.event_type.data = entry->event_type;
        event.event_type.size = strlen(entry->event_type);
        event.call.room_id.data = entry->room_id;
        event.call.room_id.size = strlen(entry->room_id);
        event.call.call_id.data = entry->call_id;
        event.call.call_id.size = strlen(entry->call_id);
        event.call.call_generation = entry->call_generation;
        event.call.expected_room_version = entry->expected_room_version;
        event.sequence = entry->sequence;
        event.input_id.data = entry->input_id;
        event.input_id.size = strlen(entry->input_id);
        event.input_value.data = entry->input_value;
        event.input_value.size = strlen(entry->input_value);
        event.payload_json.data = entry->payload_json;
        event.payload_json.size = strlen(entry->payload_json);
        if (g_gateway &&
            ivr_flowmq_gateway_send_media_event(
                g_gateway, g_config.worker_id, &event,
                salts_monotonic_ms()) != IVR_OK) {
            fprintf(stderr,
                    "ivr_worker: media event send failed session=%s type=%s\n",
                    entry->provider_session_id, entry->event_type);
        }
        disruptor_worker_release_entry(g_media_event_queue, &cursor);
    }
}

static void media_event_queue_destroy(void) {
    atomic_store_explicit(&g_accept_media_events, 0, memory_order_release);
    process_media_events();
    if (g_media_event_queue) {
        disruptor_destroy(g_media_event_queue);
        g_media_event_queue = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* config / CLI                                                        */
/* ------------------------------------------------------------------ */

static void usage(const char *program) {
    printf("TurboNet IVR Worker v%s\n\n", IVR_WORKER_APP_VERSION);
    printf("Usage: %s [OPTIONS]\n\n", program);
    printf("  --worker-id ID        DEALER identity (default: ivr-worker-1)\n");
    printf("  --config PATH         TOML config (below env and CLI)\n");
    printf("  --router-host HOST    RoomService ROUTER host (default: 127.0.0.1)\n");
    printf("  --router-port PORT    RoomService ROUTER port (required unless --dry-run)\n");
    printf("  --health-host HOST    Management bind (loopback only)\n");
    printf("  --health-port PORT    Management port (default: 18081)\n");
    printf("  --max-sessions N      Session slots (default: 4)\n");
    printf("  --heartbeat-ms N      Worker heartbeat interval (default: 5000)\n");
    printf("  --lease-ms N          RoomService lease (default: 15000, >=3H)\n");
    printf("  --assign SESSION,DIALOG,ROOM,CALL  Open a media dialog at startup (repeatable)\n");
    printf("  --shadow              Capture mutation commands for canary validation\n");
    printf("  --dry-run             Validate and exercise worker creation, then exit\n");
    printf("  Remote speech (env): IVR_OPENAI_BASE_URL + OPENAI_API_KEY (see source)\n");
    printf("  --help / --version\n");
}

static int parse_u64_arg(const char *text, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;
    if (!text || !text[0] || !out || text[0] == '-') {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static int parse_u32_arg(const char *text, uint32_t *out) {
    uint64_t value;
    if (parse_u64_arg(text, &value) != 0 || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int parse_port_arg(const char *text, int *out) {
    uint32_t value;
    if (parse_u32_arg(text, &value) != 0 || value > 65535u) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int ivr_worker_is_loopback(const char *host) {
    return host &&
           (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
            strcmp(host, "localhost") == 0);
}

static int parse_assign(const char *value, int index) {
    const char *comma1 = strchr(value, ',');
    if (!comma1) {
        return -1;
    }
    const char *comma2 = strchr(comma1 + 1, ',');
    const char *comma3 = comma2 ? strchr(comma2 + 1, ',') : NULL;
    if (!comma2 || !comma3 || strchr(comma3 + 1, ',')) {
        return -1;
    }
    size_t session_len = (size_t)(comma1 - value);
    size_t dialog_len = (size_t)(comma2 - (comma1 + 1));
    size_t room_len = (size_t)(comma3 - (comma2 + 1));
    size_t call_len = strlen(comma3 + 1);
    char session[128];
    char dialog[128];
    char room[128];
    char call[128];
    if (session_len == 0 || session_len >= sizeof(session) ||
        dialog_len == 0 || dialog_len >= sizeof(dialog) || room_len == 0 ||
        room_len >= sizeof(room) || call_len == 0 || call_len >= sizeof(call)) {
        return -1;
    }
    memcpy(session, value, session_len);
    session[session_len] = '\0';
    memcpy(dialog, comma1 + 1, dialog_len);
    dialog[dialog_len] = '\0';
    memcpy(room, comma2 + 1, room_len);
    room[room_len] = '\0';
    memcpy(call, comma3 + 1, call_len);
    call[call_len] = '\0';
    g_config.assign_session[index] = app_strdup(session);
    g_config.assign_dialog[index] = app_strdup(dialog);
    g_config.assign_room[index] = app_strdup(room);
    g_config.assign_call[index] = app_strdup(call);
    return g_config.assign_session[index] && g_config.assign_dialog[index] &&
                   g_config.assign_room[index] && g_config.assign_call[index]
               ? 0
               : -1;
}

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return -1;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            printf("TurboNet IVR Worker v%s\n", IVR_WORKER_APP_VERSION);
            return -1;
        } else if (strcmp(arg, "--config") == 0 && ++i < argc) {
            g_config.config_file = argv[i];
        } else if (strcmp(arg, "--worker-id") == 0 && ++i < argc) {
            g_config.worker_id = argv[i];
        } else if (strcmp(arg, "--router-host") == 0 && ++i < argc) {
            g_config.router_host = argv[i];
        } else if (strcmp(arg, "--router-port") == 0 && ++i < argc) {
            if (parse_port_arg(argv[i], &g_config.router_port) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--health-host") == 0 && ++i < argc) {
            g_config.health_host = argv[i];
        } else if (strcmp(arg, "--health-port") == 0 && ++i < argc) {
            if (parse_port_arg(argv[i], &g_config.health_port) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--max-sessions") == 0 && ++i < argc) {
            if (parse_u32_arg(argv[i], &g_config.max_sessions) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--heartbeat-ms") == 0 && ++i < argc) {
            if (parse_u64_arg(argv[i], &g_config.heartbeat_ms) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--lease-ms") == 0 && ++i < argc) {
            if (parse_u64_arg(argv[i], &g_config.lease_ms) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--assign") == 0 && ++i < argc) {
            if (g_config.assign_count >= IVR_WORKER_APP_MAX_ASSIGN ||
                parse_assign(argv[i], g_config.assign_count) != 0) {
                return -1;
            }
            g_config.assign_count++;
        } else if (strcmp(arg, "--shadow") == 0) {
            g_config.shadow = 1;
        } else if (strcmp(arg, "--dry-run") == 0) {
            g_config.dry_run = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
    }
    return 0;
}

static void print_config(void) {
    printf("IVR Worker Configuration\n");
    printf("  worker_id: %s\n", g_config.worker_id);
    printf("  router: %s:%d\n", g_config.router_host, g_config.router_port);
    printf("  health: %s:%d\n", g_config.health_host, g_config.health_port);
    printf("  max_sessions: %u\n", g_config.max_sessions);
    printf("  heartbeat_ms: %llu\n",
           (unsigned long long)g_config.heartbeat_ms);
    printf("  transport_timeout_ms: %llu\n",
           (unsigned long long)g_config.timeout_ms);
    printf("  lease_ms: %llu\n", (unsigned long long)g_config.lease_ms);
    printf("  gateway: %s\n", g_config.shadow ? "shadow (capture)" : "forward");
    printf("  fmq_security: %s\n",
           g_config.fmq_use_tls
               ? "mTLS"
               : (g_config.fmq_allow_insecure_loopback
                      ? "trusted-loopback"
                      : "disabled"));
    printf("  assignments: %d\n", g_config.assign_count);
}

static void signal_handler(int signum) {
    (void)signum;
    g_signal_stop = 1;
}

static int management_drain_request(void *context) {
    const ivr_worker_control_event_t event = {IVR_WORKER_CONTROL_DRAIN, 0};
    (void)context;
    if (ivr_worker_control_queue_publish(g_control_queue, &event) == 0) {
        return 0;
    }
    ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_CONTROL_QUEUE_FULL);
    atomic_store_explicit(&g_running, 0, memory_order_release);
    return -1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    /* logging daemon: never buffer stdout so SIGTERM/force-kill does not lose
       the last diagnostics */
    setvbuf(stdout, NULL, _IONBF, 0);
    atomic_init(&g_running, 1);
    ivr_worker_control_state_init(&g_control_state, 1u);
    if (remote_speech_init() != 0) {
        return 1;
    }
    g_config.shadow = 0;
    g_config.worker_id = "ivr-worker-1";
    g_config.router_host = "127.0.0.1";
    g_config.router_port = 0;
    g_config.health_host = "127.0.0.1";
    g_config.health_port = IVR_WORKER_DEFAULT_HEALTH_PORT;
    g_config.max_sessions = 4;
    g_config.timeout_ms = 0;
    g_config.heartbeat_ms = IVR_WORKER_DEFAULT_HEARTBEAT_MS;
    g_config.lease_ms = IVR_WORKER_DEFAULT_LEASE_MS;
    g_config.fmq_use_tls = 0;
    g_config.fmq_allow_insecure_loopback = 0;
    if (ivr_worker_health_init(&g_health, g_config.max_sessions) != 0) {
        fprintf(stderr, "ivr_worker: health snapshot init failed\n");
        return 1;
    }
    g_health_initialized = 1;
    ivr_worker_metrics_init(&g_metrics);

    {
        const char *config_path = getenv("IVR_CONFIG");
        for (int i = 1; i + 1 < argc; ++i) {
            if (strcmp(argv[i], "--config") == 0) {
                config_path = argv[i + 1];
                break;
            }
        }
        if (config_path && config_path[0]) {
            g_config.config_file = config_path;
            if (load_toml_config(config_path) != 0) {
                fprintf(stderr, "ivr_worker: invalid TOML config: %s\n",
                        config_path);
                return 1;
            }
            (void)atexit(toml_config_shutdown);
        }
    }
    if (apply_env_config() != 0) {
        fprintf(stderr, "ivr_worker: invalid worker environment configuration\n");
        return 1;
    }

    if (parse_args(argc, argv) != 0) {
        ivr_worker_health_destroy(&g_health);
        g_health_initialized = 0;
        return (argc > 1 &&
                (strcmp(argv[1], "--help") == 0 ||
                 strcmp(argv[1], "--version") == 0))
                   ? 0
                   : 1;
    }
    /* Fail fast on malformed worker-side scope ACL configuration. */
    if ((g_config.tenant_id && (g_config.tenant_id[0] == '\0' ||
                                strlen(g_config.tenant_id) > 63u)) ||
        (g_config.room_scope && !ivr_acl_scope_valid(g_config.room_scope)) ||
        (g_config.call_scope && !ivr_acl_scope_valid(g_config.call_scope))) {
        fprintf(stderr,
                "ivr_worker: invalid tenant/room/call scope config\n");
        ivr_worker_health_destroy(&g_health);
        g_health_initialized = 0;
        return 1;
    }
    {
        ivr_worker_health_snapshot_t health;
        if (ivr_worker_health_snapshot(&g_health, &health) != 0) {
            return 1;
        }
        health.max_sessions = g_config.max_sessions;
        if (ivr_worker_health_update(&g_health, &health) != 0) {
            fprintf(stderr, "ivr_worker: invalid health capacity\n");
            return 1;
        }
    }
    if (!g_config.health_host ||
        (strcmp(g_config.health_host, "127.0.0.1") != 0 &&
         strcmp(g_config.health_host, "::1") != 0) ||
        g_config.health_port <= 0 || g_config.health_port > UINT16_MAX ||
        g_config.max_sessions == 0 || g_config.heartbeat_ms == 0 ||
        g_config.heartbeat_ms > UINT64_MAX / 3u ||
        g_config.lease_ms < g_config.heartbeat_ms * 3u) {
        fprintf(stderr,
                "ivr_worker: max-sessions and lease>=3*heartbeat "
                "are required\n");
        ivr_worker_health_destroy(&g_health);
        g_health_initialized = 0;
        return 1;
    }
    /* Keep FlowMQ's idle deadline strictly beyond the heartbeat deadline.
       The lease validation above makes this multiplication overflow-safe. */
    g_config.timeout_ms =
        g_config.heartbeat_ms * IVR_WORKER_TRANSPORT_TIMEOUT_HEARTBEATS;
    if (!g_config.dry_run && g_config.router_port <= 0) {
        fprintf(stderr,
                "ivr_worker: --router-port required "
                "(or use --dry-run)\n");
        return 1;
    }
    if (g_config.fmq_use_tls) {
        if (g_config.fmq_allow_insecure_loopback || !g_config.fmq_ca_file ||
            !g_config.fmq_ca_file[0] || !g_config.fmq_cert_file ||
            !g_config.fmq_cert_file[0] || !g_config.fmq_key_file ||
            !g_config.fmq_key_file[0] || !g_config.fmq_server_name ||
            !g_config.fmq_server_name[0]) {
            fprintf(stderr,
                    "ivr_worker: complete FlowMQ mTLS identity "
                    "configuration is required\n");
            return 1;
        }
    } else if (!g_config.dry_run) {
        if (!g_config.shadow || !g_config.fmq_allow_insecure_loopback ||
            !ivr_worker_is_loopback(g_config.router_host)) {
            fprintf(stderr,
                    "ivr_worker: active mode requires FlowMQ mTLS; plaintext "
                    "is limited to explicit loopback shadow mode\n");
            return 1;
        }
    }
    configure_media_from_environment();
    if (g_env_parse_failed || g_media_sample_rate == 0 ||
        g_media_sample_rate > 192000u ||
        (g_remote_configured &&
         (uint32_t)g_remote_config.sample_rate != g_media_sample_rate) ||
        g_media_input_inactivity_timeout_ms >
            IVR_MEDIA_INPUT_INACTIVITY_TIMEOUT_MAX_MS) {
        fprintf(stderr, "ivr_worker: invalid media environment configuration\n");
        return 1;
    }
    if (!g_config.dry_run && !g_config.shadow) {
        if (!g_remote_configured ||
            !(g_sfu_base_url && g_sfu_base_url[0])) {
            fprintf(stderr,
                    "ivr_worker: active mode dependencies are not ready "
                    "(speech/SFU)\n");
            return 1;
        }
        health_publish(1, 1, 0, 0, 0, 1, 1, 0, 0,
                       "channels not connected");
    } else if (g_config.shadow) {
        /* Shadow is an explicit test/canary transport. It is visible for
           diagnostics but must never enter the active RoomService pool. */
        health_publish(1, 1, 0, 0, 0, 0, 0, 0, 0,
                       "shadow transport");
    }
    print_config();

    salts_uuid_t instance_uuid;
    if (salts_uuid_v4_generate(&instance_uuid) != SALTS_OK ||
        salts_uuid_format(&instance_uuid, g_instance_id,
                          sizeof(g_instance_id)) != SALTS_OK) {
        fprintf(stderr, "ivr_worker: instance UUID generation failed\n");
        return 1;
    }

    if (g_config.dry_run) {
        /* Exercise media-call admission without touching FlowMQ. */
        ivr_media_port_factory_ops_t media_factory;
        ivr_media_event_sink_ops_t event_sink;
        memset(&media_factory, 0, sizeof(media_factory));
        memset(&event_sink, 0, sizeof(event_sink));
        media_factory.abi_version = IVR_WORKER_ABI_VERSION;
        media_factory.context = &g_speech_factory;
        media_factory.create = media_factory_create;
        media_factory.destroy = media_factory_destroy;
        event_sink.abi_version = IVR_WORKER_ABI_VERSION;
        event_sink.publish_copy = enqueue_media_event_copy;

        ivr_worker_config_t wcfg;
        memset(&wcfg, 0, sizeof(wcfg));
        wcfg.abi_version = IVR_WORKER_ABI_VERSION;
        wcfg.worker_id = g_config.worker_id;
        wcfg.worker_instance_id = g_instance_id;
        wcfg.worker_epoch = g_control_state.connection_generation;
        wcfg.max_sessions_per_worker = g_config.max_sessions;
        wcfg.now_ms = worker_now_ms;
        if (media_event_queue_init() != 0 ||
            ivr_worker_create(&wcfg, &event_sink, &media_factory, &g_worker) !=
                IVR_OK) {
            fprintf(stderr, "ivr_worker: dry-run worker create failed\n");
            media_event_queue_destroy();
            return 1;
        }
        if (ivr_worker_start(g_worker) != IVR_OK) {
            ivr_worker_destroy(g_worker);
            media_event_queue_destroy();
            return 1;
        }
        for (int i = 0; i < g_config.assign_count; i++) {
            ivr_call_ref_t call;
            memset(&call, 0, sizeof(call));
            call.provider_session_id.data = g_config.assign_session[i];
            call.provider_session_id.size = strlen(g_config.assign_session[i]);
            call.dialog_id.data = g_config.assign_dialog[i];
            call.dialog_id.size = strlen(g_config.assign_dialog[i]);
            call.room_id.data = g_config.assign_room[i];
            call.room_id.size = strlen(g_config.assign_room[i]);
            call.call_id.data = g_config.assign_call[i];
            call.call_id.size = strlen(g_config.assign_call[i]);
            call.call_generation = 1;
            if (ivr_worker_open_media_call(g_worker, &call) != IVR_OK) {
                fprintf(stderr, "ivr_worker: dry-run assign %s/%s failed\n",
                        g_config.assign_room[i], g_config.assign_call[i]);
                ivr_worker_begin_drain(g_worker);
                ivr_worker_destroy(g_worker);
                media_event_queue_destroy();
                return 1;
            }
        }
        printf("ivr_worker: dry-run complete\n");
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        g_worker = NULL;
        media_event_queue_destroy();
        return 0;
    }

    /* ---- live path ---- */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (ivr_worker_control_queue_create(IVR_WORKER_CONTROL_QUEUE_CAPACITY,
                                        &g_control_queue) != 0) {
        fprintf(stderr, "ivr_worker: control queue create failed\n");
        return 1;
    }
    if (ivr_worker_http_create(&g_health, &g_management_http) != 0 ||
        ivr_worker_http_set_metrics(g_management_http, &g_metrics) != 0 ||
        ivr_worker_http_set_drain_handler(g_management_http,
                                          management_drain_request,
                                          NULL) != 0 ||
        ivr_worker_http_start(g_management_http, g_config.health_host,
                              g_config.health_port) != 0) {
        fprintf(stderr, "ivr_worker: management listener start failed on %s:%d\n",
                g_config.health_host, g_config.health_port);
        management_http_stop();
        control_queue_destroy();
        return 1;
    }

    if (reply_queue_init() != 0 || media_event_queue_init() != 0) {
        fprintf(stderr, "ivr_worker: bounded queue create failed\n");
        media_event_queue_destroy();
        reply_queue_destroy();
        management_http_stop();
        control_queue_destroy();
        return 1;
    }

    flowmq_coronet_tls_client_config_t fmq_tls;
    memset(&fmq_tls, 0, sizeof(fmq_tls));
    if (g_config.fmq_use_tls) {
        fmq_tls.ca_file = g_config.fmq_ca_file;
        fmq_tls.cert_file = g_config.fmq_cert_file;
        fmq_tls.key_file = g_config.fmq_key_file;
        fmq_tls.key_password = g_config.fmq_key_password;
        fmq_tls.server_name = g_config.fmq_server_name;
        fmq_tls.verify_peer = 1;
    }
    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = g_config.worker_id;
    gcfg.host = g_config.router_host;
    gcfg.port = g_config.router_port;
    gcfg.timeout_ms = g_config.timeout_ms;
    gcfg.transport = g_config.fmq_use_tls ? FLOWMQ_TRANSPORT_TLS
                                          : FLOWMQ_TRANSPORT_TCP;
    gcfg.tls = g_config.fmq_use_tls ? &fmq_tls : NULL;
    gcfg.on_reply = on_reply;
    gcfg.on_connection = on_command_connection;
    if (ivr_flowmq_gateway_create(&gcfg, &g_real_ops, &g_gateway) != IVR_OK) {
        fprintf(stderr, "ivr_worker: gateway create failed\n");
        reply_queue_destroy();
        management_http_stop();
        control_queue_destroy();
        return 1;
    }

    ivr_media_port_factory_ops_t media_factory;
    ivr_media_event_sink_ops_t event_sink;
    memset(&media_factory, 0, sizeof(media_factory));
    memset(&event_sink, 0, sizeof(event_sink));
    media_factory.abi_version = IVR_WORKER_ABI_VERSION;
    media_factory.context = &g_speech_factory;
    media_factory.create = media_factory_create;
    media_factory.destroy = media_factory_destroy;
    event_sink.abi_version = IVR_WORKER_ABI_VERSION;
    event_sink.publish_copy = enqueue_media_event_copy;

    ivr_worker_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.abi_version = IVR_WORKER_ABI_VERSION;
    wcfg.worker_id = g_config.worker_id;
    wcfg.worker_instance_id = g_instance_id;
    wcfg.worker_epoch = g_control_state.connection_generation;
    wcfg.max_sessions_per_worker = g_config.max_sessions;
    wcfg.now_ms = worker_now_ms;
    if (ivr_worker_create(&wcfg, &event_sink, &media_factory, &g_worker) !=
        IVR_OK) {
        fprintf(stderr, "ivr_worker: worker create failed\n");
        ivr_flowmq_gateway_destroy(g_gateway);
        media_event_queue_destroy();
        reply_queue_destroy();
        management_http_stop();
        control_queue_destroy();
        return 1;
    }
    if (ivr_worker_start(g_worker) != IVR_OK) {
        fprintf(stderr, "ivr_worker: worker start failed\n");
        ivr_worker_destroy(g_worker);
        ivr_flowmq_gateway_destroy(g_gateway);
        media_event_queue_destroy();
        reply_queue_destroy();
        management_http_stop();
        control_queue_destroy();
        return 1;
    }

    if (ivr_flowmq_gateway_start(g_gateway) != IVR_OK) {
        fprintf(stderr, "ivr_worker: start failed\n");
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        ivr_flowmq_gateway_destroy(g_gateway);
        media_event_queue_destroy();
        reply_queue_destroy();
        management_http_stop();
        control_queue_destroy();
        return 1;
    }

    for (int i = 0; i < g_config.assign_count; i++) {
        ivr_call_ref_t call;
        memset(&call, 0, sizeof(call));
        call.provider_session_id.data = g_config.assign_session[i];
        call.provider_session_id.size = strlen(g_config.assign_session[i]);
        call.dialog_id.data = g_config.assign_dialog[i];
        call.dialog_id.size = strlen(g_config.assign_dialog[i]);
        call.room_id.data = g_config.assign_room[i];
        call.room_id.size = strlen(g_config.assign_room[i]);
        call.call_id.data = g_config.assign_call[i];
        call.call_id.size = strlen(g_config.assign_call[i]);
        call.call_generation = 1;
        if (ivr_worker_open_media_call(g_worker, &call) != IVR_OK) {
            fprintf(stderr, "ivr_worker: assign %s/%s failed\n",
                    g_config.assign_room[i], g_config.assign_call[i]);
        } else {
            printf("[ivr_worker] assigned %s/%s\n",
                   g_config.assign_room[i], g_config.assign_call[i]);
        }
    }

    /* worker.sync registration (readiness prerequisite); the WorkerSyncResultV1
       reply is logged by on_reply */
    if (send_worker_sync_v2("ws-startup") != IVR_OK) {
        printf("[ivr_worker] worker.sync send failed (will retry on reconnect)\n");
    } else {
        printf("[ivr_worker] worker.sync sent ws-startup\n");
    }

    printf("ivr_worker: running on %s:%d (worker=%s)\n",
           g_config.router_host, g_config.router_port, g_config.worker_id);
    int sync_retries = 0;
    int observed_command_connected = -1;
    uint64_t heartbeat_sequence = 0;
    uint64_t health_revocation_sequence = 0;
    uint64_t next_heartbeat_ms =
        salts_monotonic_ms() + g_config.heartbeat_ms;
    int ticks = 0;
    while (atomic_load_explicit(&g_running, memory_order_acquire) &&
           !g_signal_stop) {
        int command_connected;
        process_control_events();
        if (!atomic_load_explicit(&g_running, memory_order_acquire)) break;
        process_replies();
        process_media_events();
        ivr_thread_sleep_ms(100);
        command_connected = g_control_state.connected;
        if (command_connected != observed_command_connected) {
            observed_command_connected = command_connected;
            if (!command_connected) {
                int was_synced = g_synced;
                char mid[64];
                health_publish(1, 1, command_connected, command_connected, 0,
                               g_remote_configured,
                               g_sfu_base_url && g_sfu_base_url[0], 0, 0,
                               "command channel disconnected");
                if (was_synced && command_connected) {
                    snprintf(mid, sizeof(mid), "health-revoked-%llu",
                             (unsigned long long)
                                 ++health_revocation_sequence);
                    if (send_worker_heartbeat(mid) != IVR_OK) {
                        ivr_worker_metrics_inc(
                            &g_metrics,
                            IVR_WORKER_METRIC_HEARTBEAT_FAILURE);
                        printf("[ivr_worker] readiness revocation send failed\n");
                    }
                }
                g_synced = 0;
            }
        }
        /* retry worker.sync until acknowledged: the DEALER may not be
           connected when the first attempt is sent (registration is the
           readiness prerequisite) */
        if (!g_synced && command_connected && (++ticks % 20) == 0) {
            char mid[64];
            snprintf(mid, sizeof(mid), "ws-startup-%d", sync_retries++);
            ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_SYNC_RETRY);
            printf("[ivr_worker] worker.sync retry %s\n", mid);
            if (send_worker_sync_v2(mid) != IVR_OK) {
                printf("[ivr_worker] worker.sync retry %d send failed\n",
                       sync_retries);
            }
        }
        if (g_synced && command_connected &&
            salts_monotonic_ms() >= next_heartbeat_ms) {
            char mid[64];
            snprintf(mid, sizeof(mid), "heartbeat-%llu",
                     (unsigned long long)++heartbeat_sequence);
            if (send_worker_heartbeat(mid) != IVR_OK) {
                ivr_worker_metrics_inc(
                    &g_metrics, IVR_WORKER_METRIC_HEARTBEAT_FAILURE);
                g_synced = 0;
                health_publish(1, 1, 0, 0, 0,
                               g_remote_configured,
                               g_sfu_base_url && g_sfu_base_url[0], 0, 0,
                               "heartbeat send failed");
                printf("[ivr_worker] heartbeat send failed (transport_timeout_ms=%llu); "
                       "readiness revoked\n",
                       (unsigned long long)g_config.timeout_ms);
            }
            next_heartbeat_ms =
                salts_monotonic_ms() + g_config.heartbeat_ms;
        }
    }

    printf("ivr_worker: draining\n");
    ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_DRAIN);
    health_publish(1, 1, 0, 0, 0, g_remote_configured,
                   g_sfu_base_url && g_sfu_base_url[0], 1, 0, "draining");
    ivr_worker_control_queue_close(g_control_queue);
    atomic_store_explicit(&g_accept_replies, 0, memory_order_release);
    ivr_worker_begin_drain(g_worker);
    process_media_events();
    media_event_queue_destroy();
    ivr_worker_destroy(g_worker);
    g_worker = NULL;
    ivr_flowmq_gateway_destroy(g_gateway);
    g_gateway = NULL;
    reply_queue_destroy();
    management_http_stop();
    control_queue_destroy();
    ivr_worker_health_destroy(&g_health);
    g_health_initialized = 0;
    return 0;
}
