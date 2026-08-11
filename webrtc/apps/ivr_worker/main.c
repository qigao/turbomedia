/* ivr_worker - standalone IVR worker executable.
 *
 * Wires the real pieces together: FlowMQ DEALER command gateway (worker.sync
 * registration + command replies), FlowMQ SUB domain-event subscriber feeding
 * ivr_worker_submit_event_copy, the per-call TurboXML sessions, and per-call
 * WHIP/WHEP bot media. Live mode forwards commands through FlowMQ by default;
 * explicit `--shadow` captures mutation commands for canary validation.
 *
 * Run loop stops on SIGINT/SIGTERM: begin_drain -> stop subscriber -> destroy
 * gateway -> destroy worker. `--dry-run` validates the config and exercises
 * worker/session creation without touching FlowMQ. */
#include "ivr/ivr_worker.h"
#include "ivr_media_bot.h"
#include "ivr_openai_provider.h"
#include "ivr_speech_session_factory.h"
#include "ivr_flowmq_gateway.h"
#include "ivr/ivr_acl.h"
#include "ivr_fmq_security.h"
#include "ivr_flowmq_subscriber.h"
#include "turbo_flow_fmq.h"
#include "ivr_whip_transport.h"
#include "ivr_whep_transport.h"
#include "ivr_media_supervisor.h"
#include "ivr_frame.h"
#include "ivr_internal.h"
#include "ivr_content.h"
#include "ivr_dtmf_rtp.h"
#include "ivr_thread.h"
#include "ivr_worker_health.h"
#include "ivr_worker_http.h"
#include "ivr_worker_metrics.h"
#include "turbomedia_ivr_v1.h"
#include "disruptor.h"
#include "platform.h"
#include "turbo_uuid.h"
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
#define IVR_WORKER_RESULT_PAYLOAD_CAPACITY 3072u
#define IVR_WORKER_COMMAND_RESULT_EVENT "command.result"
#define IVR_WORKER_REPLY_QUEUE_CAPACITY 16u
#define IVR_WORKER_REPLY_FRAME_CAPACITY (IVR_FRAME_HEADER_SIZE + 64u * 1024u)
#define IVR_WORKER_DEFAULT_HEARTBEAT_MS 5000u
#define IVR_WORKER_DEFAULT_LEASE_MS 15000u
#define IVR_WORKER_DEFAULT_HEALTH_PORT 18081
#define IVR_WORKER_TRANSPORT_TIMEOUT_HEARTBEATS 2u
#define IVR_MEDIA_RECONNECT_MAX_ATTEMPTS 3u
#define IVR_MEDIA_RECONNECT_INITIAL_BACKOFF_MS 100u
#define IVR_MEDIA_RECONNECT_MAX_BACKOFF_MS 2000u
#define IVR_MEDIA_RECONNECT_DEADLINE_MS 10000u
#define IVR_MEDIA_ID_CAPACITY 128u

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

static void session_observe_latency(void *context,
                                    ivr_session_latency_kind_t kind,
                                    uint64_t duration_ms) {
    ivr_worker_metrics_t *metrics = (ivr_worker_metrics_t *)context;
    if (kind == IVR_SESSION_LATENCY_DTMF_TO_CANCEL) {
        ivr_worker_metrics_observe_ms(
            metrics, IVR_WORKER_HISTOGRAM_DTMF_TO_CANCEL, duration_ms);
    } else if (kind == IVR_SESSION_LATENCY_ASR_TO_COMMAND) {
        ivr_worker_metrics_observe_ms(
            metrics, IVR_WORKER_HISTOGRAM_ASR_TO_COMMAND, duration_ms);
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
    g_remote_config.sample_rate = env_int("IVR_OPENAI_SAMPLE_RATE", 0);
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
    const char *content_root;
    const char *router_host;
    int router_port;
    const char *pub_host;
    int pub_port;
    const char *pub_topic;
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
    const char *fmq_shared_secret;
    int fmq_tls_rotation_generation;
    /* P0-04.4 worker-side scope: the worker rejects dispatches outside its
       configured tenant/room/call scope or content package allowlist even if
       RoomService already filters selection. NULL = unrestricted. */
    const char *tenant_id;
    const char *room_scope;
    const char *call_scope;
    const char *content_capabilities;
    int dry_run;
    const char *config_file;
    int assign_count;
    const char *assign_room[IVR_WORKER_APP_MAX_ASSIGN];
    const char *assign_call[IVR_WORKER_APP_MAX_ASSIGN];
    const char *assign_pkg[IVR_WORKER_APP_MAX_ASSIGN];
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

static int toml_copy_string(const turbo_toml_t *table, const char *key,
                            const char **target) {
    turbo_toml_value_t value;
    char *copy;
    if (!table || !key || !target || !rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = turbo_toml_string(table, key);
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

static int toml_copy_int(const turbo_toml_t *table, const char *key, int *target) {
    turbo_toml_value_t value;
    if (!table || !key || !target || !rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = turbo_toml_int(table, key);
    if (!value.ok || value.u.i < INT_MIN || value.u.i > INT_MAX) {
        return -1;
    }
    *target = (int)value.u.i;
    return 0;
}

static int toml_copy_bool(const turbo_toml_t *table, const char *key,
                          int *target) {
    turbo_toml_value_t value;
    if (!table || !key || !target || !rtc_app_toml_table_has_key(table, key)) {
        return 0;
    }
    value = turbo_toml_bool(table, key);
    if (!value.ok) return -1;
    *target = value.u.b ? 1 : 0;
    return 0;
}

static int load_toml_config(const char *filename) {
    turbo_toml_t *worker = NULL;
    const char *const allowed[] = {
        "worker_id",   "content_root", "router_host",  "router_port",
        "pub_host",    "pub_port",     "pub_topic",    "health_host",
        "health_port", "max_sessions", "heartbeat_ms", "lease_ms",
        "fmq_use_tls", "fmq_allow_insecure_loopback", "fmq_ca_file",
        "fmq_cert_file", "fmq_key_file", "fmq_key_password",
        "fmq_server_name", "fmq_shared_secret", "fmq_rotation_generation",
        "tenant_id", "room_scope", "call_scope", "content_capabilities"};
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
        toml_copy_string(worker, "content_root", &g_config.content_root) != 0 ||
        toml_copy_string(worker, "router_host", &g_config.router_host) != 0 ||
        toml_copy_int(worker, "router_port", &g_config.router_port) != 0 ||
        toml_copy_string(worker, "pub_host", &g_config.pub_host) != 0 ||
        toml_copy_int(worker, "pub_port", &g_config.pub_port) != 0 ||
        toml_copy_string(worker, "pub_topic", &g_config.pub_topic) != 0 ||
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
        toml_copy_string(worker, "fmq_shared_secret",
                         &g_config.fmq_shared_secret) != 0 ||
        toml_copy_int(worker, "fmq_rotation_generation",
                      &g_config.fmq_tls_rotation_generation) != 0 ||
        toml_copy_string(worker, "tenant_id", &g_config.tenant_id) != 0 ||
        toml_copy_string(worker, "room_scope", &g_config.room_scope) != 0 ||
        toml_copy_string(worker, "call_scope", &g_config.call_scope) != 0 ||
        toml_copy_string(worker, "content_capabilities",
                         &g_config.content_capabilities) != 0) {
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
    ENV_STR("IVR_CONTENT_ROOT", content_root);
    ENV_STR("IVR_ROUTER_HOST", router_host);
    ENV_STR("IVR_PUB_HOST", pub_host);
    ENV_STR("IVR_PUB_TOPIC", pub_topic);
    ENV_STR("IVR_HEALTH_HOST", health_host);
    ENV_STR("IVR_FMQ_CA_FILE", fmq_ca_file);
    ENV_STR("IVR_FMQ_CERT_FILE", fmq_cert_file);
    ENV_STR("IVR_FMQ_KEY_FILE", fmq_key_file);
    ENV_STR("IVR_FMQ_KEY_PASSWORD", fmq_key_password);
    ENV_STR("IVR_FMQ_SERVER_NAME", fmq_server_name);
    ENV_STR("IVR_FMQ_SHARED_SECRET", fmq_shared_secret);
#undef ENV_STR
    v = getenv("IVR_ROUTER_PORT");
    if (v && v[0] && (parse_port_arg(v, &port) != 0)) return -1;
    if (v && v[0]) g_config.router_port = port;
    v = getenv("IVR_PUB_PORT");
    if (v && v[0] && (parse_port_arg(v, &port) != 0)) return -1;
    if (v && v[0]) g_config.pub_port = port;
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
    v = getenv("IVR_FMQ_ROTATION_GENERATION");
    if (v && v[0] && (parse_u32_arg(v, &u32) != 0 || u32 > INT_MAX)) return -1;
    if (v && v[0]) g_config.fmq_tls_rotation_generation = (int)u32;
    return 0;
}
static ivr_worker_t *g_worker = NULL;
static ivr_flowmq_gateway_t *g_gateway = NULL;
static ivr_command_gateway_ops_t g_real_ops;
static ivr_flowmq_subscriber_t *g_subscriber = NULL;
static ivr_fmq_security_owner_t *g_fmq_security = NULL;
static volatile int g_running = 1;
static volatile int g_synced = 0; /* worker.sync acknowledged */
static DataBind *g_codec = NULL;  /* process-lifetime codec for dispatch decode */

typedef struct {
    size_t length;
    uint8_t frame[IVR_WORKER_REPLY_FRAME_CAPACITY];
} ivr_worker_reply_entry_t;

static disruptor_t *g_reply_queue = NULL;
static ivr_atomic_int_t g_accept_replies;
static ivr_atomic_int_t g_command_connected;
static ivr_atomic_int_t g_event_connected;
static char g_instance_id[TURBO_UUID_STRING_SIZE];
static const uint64_t g_connection_generation = 1;
static const char *g_sfu_host = NULL;
static const char *g_sfu_media_token = NULL;
static int g_sfu_port = 0;
static int g_sfu_allow_loopback = 0;
static uint32_t g_media_sample_rate = 16000u;
static ivr_worker_health_t g_health;
static int g_health_initialized = 0;
static ivr_worker_http_t *g_management_http = NULL;

static void metrics_observe_elapsed(ivr_worker_histogram_kind_t kind,
                                    uint64_t started_at_ms) {
    uint64_t finished_at_ms = turbo_monotonic_ms();
    ivr_worker_metrics_observe_ms(
        &g_metrics, kind,
        finished_at_ms >= started_at_ms ? finished_at_ms - started_at_ms : 0);
}

static void management_http_stop(void) {
    if (g_management_http) {
        ivr_worker_http_destroy(g_management_http);
        g_management_http = NULL;
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
             "turboxml,flowmq%s%s%s", speech_ready ? ",tts,asr" : "",
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
                 "turboxml,flowmq%s%s%s",
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
    status->connection_generation = g_connection_generation;
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
/* shadow / real command gateway                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const ivr_command_gateway_ops_t *real;
    int shadow;
} ivr_worker_cli_gateway_t;
static ivr_worker_cli_gateway_t g_cli_gateway;

static ivr_status_t cli_gateway_submit(void *ctx, const ivr_command_view_t *cmd) {
    ivr_worker_cli_gateway_t *g = (ivr_worker_cli_gateway_t *)ctx;
    printf("[ivr_worker] %s command: %.*s (room=%.*s call=%.*s gen=%llu)\n",
           g->shadow ? "[shadow]" : "[send]",
           (int)cmd->command_type.size, cmd->command_type.data,
           (int)cmd->call.room_id.size, cmd->call.room_id.data,
           (int)cmd->call.call_id.size, cmd->call.call_id.data,
           (unsigned long long)cmd->call.call_generation);
    if (g->shadow) {
        return IVR_OK; /* record only; no mutation command is sent */
    }
    if (!g->real || !g->real->submit_copy) {
        return IVR_ESTATE;
    }
    return g->real->submit_copy(g->real->context, cmd);
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
    ivr_dtmf_ingress_t *dtmf_ingress;
    ivr_worker_media_state_context_t whip_state;
    ivr_worker_media_state_context_t whep_state;
    ivr_worker_log_transport_t log_transport;
    ivr_speech_session_t *speech;
    int rtc_enabled;
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
};

static int media_instance_copy_call(ivr_worker_media_instance_t *instance,
                                     const ivr_call_ref_t *call) {
    if (!instance || !call || !call->room_id.data || !call->call_id.data ||
        call->room_id.size == 0 || call->room_id.size >= IVR_MEDIA_ID_CAPACITY ||
        call->call_id.size == 0 || call->call_id.size >= IVR_MEDIA_ID_CAPACITY) {
        return -1;
    }
    memcpy(instance->room_id, call->room_id.data, call->room_id.size);
    instance->room_id[call->room_id.size] = '\0';
    memcpy(instance->call_id, call->call_id.data, call->call_id.size);
    instance->call_id[call->call_id.size] = '\0';
    instance->call_generation = call->call_generation;
    return 0;
}

static void media_instance_call(const ivr_worker_media_instance_t *instance,
                                ivr_call_ref_t *call) {
    memset(call, 0, sizeof(*call));
    call->room_id.data = instance->room_id;
    call->room_id.size = strlen(instance->room_id);
    call->call_id.data = instance->call_id;
    call->call_id.size = strlen(instance->call_id);
    call->call_generation = instance->call_generation;
}

static void media_state_callback(void *context, const ivr_call_ref_t *call,
                                 uint64_t attempt_generation,
                                 ivr_media_link_state_t state, int error_code) {
    ivr_worker_media_state_context_t *state_context =
        (ivr_worker_media_state_context_t *)context;
    ivr_worker_media_instance_t *instance;
    if (!state_context || !(instance = state_context->instance) || !call ||
        call->call_generation != instance->call_generation ||
        call->room_id.size != strlen(instance->room_id) ||
        call->call_id.size != strlen(instance->call_id) ||
        memcmp(call->room_id.data, instance->room_id, call->room_id.size) != 0 ||
        memcmp(call->call_id.data, instance->call_id, call->call_id.size) != 0) {
        return;
    }
    (void)ivr_media_supervisor_submit_state(
        instance->supervisor, state_context->link, attempt_generation, state,
        error_code);
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
    (void)ivr_worker_submit_event_copy(g_worker, &view);
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

/* Bot media events (asr.final, ...) are routed into the per-call session. */
static void media_bot_on_event(void *ctx, const ivr_event_view_t *event) {
    (void)ctx;
    if (event && event->event_type.size == sizeof("provider.error") - 1u &&
        memcmp(event->event_type.data, "provider.error",
               sizeof("provider.error") - 1u) == 0) {
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_PROVIDER_ERROR);
    }
    ivr_status_t rc = ivr_worker_submit_event_copy(g_worker, event);
    printf("[ivr_worker] media event: type=%.*s room=%.*s call=%.*s routed=%s\n",
           (int)event->event_type.size, event->event_type.data,
           (int)event->call.room_id.size, event->call.room_id.data,
           (int)event->call.call_id.size, event->call.call_id.data,
           rc == IVR_OK ? "yes" : "no");
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
    return instance && instance->bot_ops.cancel_input
               ? instance->bot_ops.cancel_input(instance->bot_ops.context, call)
               : IVR_EINVAL;
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
    if (!instance->dtmf_ingress) {
        return IVR_OK;
    }
    memcpy(input_id_copy, input_id->data, input_id->size);
    input_id_copy[input_id->size] = '\0';
    return ivr_dtmf_ingress_begin_input(instance->dtmf_ingress, call,
                                        input_id_copy, input_generation);
}

static ivr_status_t media_end_input(void *ctx, const ivr_call_ref_t *call,
                                    const ivr_bytes_view_t *input_id,
                                    uint64_t input_generation) {
    ivr_worker_media_instance_t *instance =
        (ivr_worker_media_instance_t *)ctx;
    (void)input_id;
    if (!instance || !call || input_generation == 0) {
        return IVR_EINVAL;
    }
    if (!instance->dtmf_ingress) {
        return IVR_OK;
    }
    return ivr_dtmf_ingress_end_input(instance->dtmf_ingress, call,
                                      input_generation);
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
    return ivr_worker_submit_event_copy(g_worker, &event) == IVR_OK ? 0 : -1;
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
    instance->rtc_enabled = g_sfu_host && g_sfu_host[0] && g_sfu_port > 0;
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
        whip_config.sfu_host = g_sfu_host;
        whip_config.sfu_port = g_sfu_port;
        whip_config.media_token = g_sfu_media_token;
        whip_config.allow_loopback = g_sfu_allow_loopback;
        whip_config.sample_rate = g_media_sample_rate;
        instance->whip_state.instance = instance;
        instance->whip_state.link = IVR_MEDIA_LINK_WHIP;
        whip_config.on_state = media_state_callback;
        whip_config.state_context = &instance->whip_state;
        memset(&whep_config, 0, sizeof(whep_config));
        whep_config.sfu_host = g_sfu_host;
        whep_config.sfu_port = g_sfu_port;
        whep_config.media_token = g_sfu_media_token;
        whep_config.allow_loopback = g_sfu_allow_loopback;
        whep_config.sample_rate = g_media_sample_rate;
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
    ivr_media_supervisor_destroy(instance->supervisor);
    ivr_whep_transport_destroy(instance->whep);
    ivr_whip_transport_destroy(instance->whip);
    ivr_dtmf_ingress_destroy(instance->dtmf_ingress);
    ivr_media_bot_destroy(instance->bot);
    if (speech_factory && speech_factory->destroy) {
        speech_factory->destroy(speech_factory->context, instance->speech);
    }
    free(instance);
}

static void configure_media_from_environment(void) {
    g_sfu_host = getenv("IVR_SFU_HOST");
    g_sfu_media_token = getenv("IVR_SFU_MEDIA_TOKEN");
    g_sfu_port = env_int("IVR_SFU_PORT", 0);
    g_sfu_allow_loopback = env_int("IVR_SFU_ALLOW_LOOPBACK", 0);
    g_media_sample_rate = (uint32_t)env_int("IVR_MEDIA_SAMPLE_RATE", 16000);
    if (g_sfu_host && g_sfu_host[0] && g_sfu_port > 0) {
        printf("[ivr_worker] media: WebRTC WHIP/WHEP enabled at %s:%d\n",
               g_sfu_host, g_sfu_port);
    } else {
        printf("[ivr_worker] media: SFU endpoint not configured; dry-run/logging transport\n");
    }
}

/* ------------------------------------------------------------------ */
/* DEALER reply / SUB event ingress                                    */
/* ------------------------------------------------------------------ */

static const char *dispatch_error_code(ivr_status_t status) {
    switch (status) {
    case IVR_ENOSPC:
        return "worker_capacity";
    case IVR_ECLOSED:
        return "worker_closed";
    case IVR_EINVAL:
        return "invalid_dispatch";
    case IVR_ESTATE:
        return "assignment_state";
    case IVR_EVERSION:
        return "stale_worker_generation";
    case IVR_ESTALE:
        return "dispatch.deadline";
    case IVR_EAUTH:
        return "dispatch.out_of_scope";
    default:
        return "assignment_failed";
    }
}

static void send_dispatch_result(const ivr_call_dispatch_t *dispatch,
                                 ivr_status_t status) {
    const char *error_code = status == IVR_OK ? "" : dispatch_error_code(status);
    const char *error_message =
        status == IVR_OK ? "" : "worker could not create the call session";
    ivr_status_t send_rc;
    if (dispatch->wire_version == 2u) {
        send_rc = ivr_flowmq_gateway_send_dispatch_result_v2(
            g_gateway, dispatch, status, ivr_worker_active_sessions(g_worker),
            g_config.max_sessions, error_code, error_message);
    } else {
        send_rc = ivr_flowmq_gateway_send_dispatch_result(
            g_gateway, dispatch, status, error_code, error_message);
    }
    if (send_rc != IVR_OK) {
        printf("[ivr_worker] dispatch result %s/%s status=%d: send failed (%d)\n",
               dispatch->room_id, dispatch->call_id, status, send_rc);
        return;
    }
    printf("[ivr_worker] dispatch result sent %s/%s status=%d\n",
           dispatch->room_id, dispatch->call_id, status);
}

static void send_release_result(const ivr_call_release_t *release,
                                ivr_status_t status) {
    const char *error_code = status == IVR_OK ? "" : "release_failed";
    const char *error_message =
        status == IVR_OK ? "" : "worker could not release the call session";
    ivr_status_t send_rc = ivr_flowmq_gateway_send_release_result(
        g_gateway, release, status, error_code, error_message);
    if (send_rc != IVR_OK) {
        printf("[ivr_worker] release result %s/%s status=%d: send failed (%d)\n",
               release->room_id, release->call_id, status, send_rc);
        return;
    }
    printf("[ivr_worker] release result sent %s/%s status=%d\n",
           release->room_id, release->call_id, status);
}

/* P0-04.4 worker-side dispatch admission: the worker must be configured for
   the dispatched tenant/room/call scope and the content package before it
   accepts a session. This is defense in depth on top of RoomService ACL
   selection; a mismatch fails fast with a structured reject. */
static int worker_dispatch_in_scope(const ivr_call_dispatch_t *dispatch) {
    if (g_config.tenant_id && g_config.tenant_id[0] &&
        !ivr_acl_tenant_allows(g_config.tenant_id, dispatch->room_id)) {
        return 0;
    }
    if (g_config.room_scope && g_config.room_scope[0] &&
        !ivr_acl_scope_allows(g_config.room_scope, dispatch->room_id)) {
        return 0;
    }
    if (g_config.call_scope && g_config.call_scope[0] &&
        !ivr_acl_scope_allows(g_config.call_scope, dispatch->call_id)) {
        return 0;
    }
    if (g_config.content_capabilities && g_config.content_capabilities[0] &&
        !ivr_acl_scope_allows(g_config.content_capabilities,
                              dispatch->content_package)) {
        return 0;
    }
    return 1;
}

static void handle_reply(const uint8_t *frame, size_t len) {
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_REPLY_INVALID);
        printf("[ivr_worker] reply: malformed frame\n");
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        (info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V1 ||
         info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V2)) {
        /* RoomService pushed a call to this worker: create a session. */
        ivr_call_dispatch_t dispatch;
        DataBind *codec = ensure_codec();
        if (!codec ||
            ivr_flowmq_gateway_decode_dispatch(codec, frame, len, &dispatch) !=
                IVR_OK) {
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_REPLY_INVALID);
            printf("[ivr_worker] dispatch: decode failed\n");
            return;
        }
        if (strcmp(dispatch.worker_id, g_config.worker_id) != 0) {
            printf("[ivr_worker] dispatch: not for this worker (%s)\n",
                   dispatch.worker_id);
            return;
        }
        uint64_t dispatch_started_at_ms = turbo_monotonic_ms();
        if (dispatch.wire_version == 2u &&
            (strcmp(dispatch.worker_instance_id, g_instance_id) != 0 ||
             dispatch.worker_connection_generation !=
                 g_connection_generation)) {
            printf("[ivr_worker] dispatch %s/%s: stale worker generation\n",
                   dispatch.room_id, dispatch.call_id);
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_ASSIGN_REJECTED);
            send_dispatch_result(&dispatch, IVR_EVERSION);
            metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_DISPATCH,
                                    dispatch_started_at_ms);
            return;
        }
        /* P0-04.5: the dispatch carries a relative deadline TTL evaluated on
           worker receipt; an expired dispatch must never create a session. */
        if (!ivr_flowmq_gateway_dispatch_deadline_ok(
                &dispatch, dispatch_started_at_ms, turbo_monotonic_ms())) {
            printf("[ivr_worker] dispatch %s/%s: deadline expired\n",
                   dispatch.room_id, dispatch.call_id);
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_ASSIGN_REJECTED);
            send_dispatch_result(&dispatch, IVR_ESTALE);
            metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_DISPATCH,
                                    dispatch_started_at_ms);
            return;
        }
        if (!worker_dispatch_in_scope(&dispatch)) {
            printf("[ivr_worker] dispatch %s/%s: out of worker scope\n",
                   dispatch.room_id, dispatch.call_id);
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_ASSIGN_REJECTED);
            send_dispatch_result(&dispatch, IVR_EAUTH);
            metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_DISPATCH,
                                    dispatch_started_at_ms);
            return;
        }
        ivr_call_ref_t call;
        memset(&call, 0, sizeof(call));
        call.room_id.data = dispatch.room_id;
        call.room_id.size = strlen(dispatch.room_id);
        call.call_id.data = dispatch.call_id;
        call.call_id.size = strlen(dispatch.call_id);
        call.call_generation = dispatch.call_generation;
        call.expected_room_version = dispatch.expected_room_version;
        if (ivr_worker_has_session(g_worker, &call)) {
            printf("[ivr_worker] dispatch %s/%s: already assigned (idempotent)\n",
                   dispatch.room_id, dispatch.call_id);
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_ASSIGN_ACCEPTED);
            send_dispatch_result(&dispatch, IVR_OK);
            metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_DISPATCH,
                                    dispatch_started_at_ms);
            return;
        }
        ivr_session_t *session = NULL;
        ivr_status_t rc = ivr_worker_assign_session(
            g_worker, &call,
            dispatch.content_package[0] ? dispatch.content_package
                                        : "conference-greeting",
            &session);
        if (rc == IVR_OK) {
            ivr_worker_health_snapshot_t health;
            if (refresh_health_capacity(&health) != 0) {
                fprintf(stderr,
                        "ivr_worker: assignment health refresh failed\n");
            }
        }
        ivr_worker_metrics_inc(
            &g_metrics, rc == IVR_OK ? IVR_WORKER_METRIC_ASSIGN_ACCEPTED
                                     : IVR_WORKER_METRIC_ASSIGN_REJECTED);
        printf("[ivr_worker] dispatch %s/%s (pkg=%s) -> %s\n",
               dispatch.room_id, dispatch.call_id,
               dispatch.content_package[0] ? dispatch.content_package
                                           : "conference-greeting",
               rc == IVR_OK ? "assigned" : "rejected");
        send_dispatch_result(&dispatch, rc);
        metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_DISPATCH,
                                dispatch_started_at_ms);
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        info.schema_type_id == IVR_TYPE_CALL_RELEASE_COMMAND_V1) {
        ivr_call_release_t release;
        DataBind *codec = ensure_codec();
        if (!codec ||
            ivr_flowmq_gateway_decode_release(codec, frame, len, &release) !=
                IVR_OK) {
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_REPLY_INVALID);
            printf("[ivr_worker] release: decode failed\n");
            return;
        }
        if (strcmp(release.worker_id, g_config.worker_id) != 0) {
            printf("[ivr_worker] release: not for this worker (%s)\n",
                   release.worker_id);
            return;
        }
        ivr_call_ref_t call;
        memset(&call, 0, sizeof(call));
        call.room_id.data = release.room_id;
        call.room_id.size = strlen(release.room_id);
        call.call_id.data = release.call_id;
        call.call_id.size = strlen(release.call_id);
        call.call_generation = release.call_generation;
        ivr_status_t rc = ivr_worker_release_call(g_worker, &call);
        if (rc == IVR_OK) {
            ivr_worker_health_snapshot_t health;
            if (refresh_health_capacity(&health) != 0) {
                fprintf(stderr, "ivr_worker: release health refresh failed\n");
            }
        }
        ivr_worker_metrics_inc(
            &g_metrics, rc == IVR_OK ? IVR_WORKER_METRIC_RELEASE_ACCEPTED
                                     : IVR_WORKER_METRIC_RELEASE_REJECTED);
        printf("[ivr_worker] release %s/%s -> %s\n",
               release.room_id, release.call_id,
               rc == IVR_OK ? "released" : "rejected");
        send_release_result(&release, rc);
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_IVR_COMMAND_RESULT_V1) {
        ivr_command_result_envelope_t result;
        DataBind *codec = ensure_codec();
        if (!codec ||
            ivr_flowmq_gateway_decode_result(codec, frame, len, &result) !=
                IVR_OK) {
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_REPLY_INVALID);
            printf("[ivr_worker] command result: decode failed\n");
            return;
        }
        if (strcmp(result.worker_id, g_config.worker_id) != 0) {
            printf("[ivr_worker] command result: not for this worker (%s)\n",
                   result.worker_id);
            return;
        }
        char payload[IVR_WORKER_RESULT_PAYLOAD_CAPACITY];
        char number[32];
        ivr_json_builder_t json;
        ivr_json_builder_init(&json, payload, sizeof(payload));
        ivr_json_builder_raw(&json, "{\"message_id\":");
        ivr_json_builder_string_cstr(&json, result.message_id);
        ivr_json_builder_raw(&json, ",\"status_code\":");
        snprintf(number, sizeof(number), "%d", result.status_code);
        ivr_json_builder_raw(&json, number);
        ivr_json_builder_raw(&json, ",\"room_version\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)result.room_version);
        ivr_json_builder_raw(&json, number);
        ivr_json_builder_raw(&json, ",\"sequence\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)result.sequence);
        ivr_json_builder_raw(&json, number);
        ivr_json_builder_raw(&json, ",\"error_code\":");
        ivr_json_builder_string_cstr(&json, result.error_code);
        ivr_json_builder_raw(&json, ",\"error_message\":");
        ivr_json_builder_string_cstr(&json, result.error_message);
        ivr_json_builder_raw(&json, "}");
        if (!ivr_json_builder_ok(&json)) {
            printf("[ivr_worker] command result: payload too large\n");
            return;
        }
        ivr_event_view_t event;
        memset(&event, 0, sizeof(event));
        event.event_id.data = result.message_id;
        event.event_id.size = strlen(result.message_id);
        event.event_type.data = IVR_WORKER_COMMAND_RESULT_EVENT;
        event.event_type.size = strlen(IVR_WORKER_COMMAND_RESULT_EVENT);
        event.call.room_id.data = result.room_id;
        event.call.room_id.size = strlen(result.room_id);
        event.call.call_id.data = result.call_id;
        event.call.call_id.size = strlen(result.call_id);
        event.call.call_generation = result.call_generation;
        event.call.expected_room_version = result.room_version;
        event.sequence = result.sequence;
        event.payload_json.data = payload;
        event.payload_json.size = strlen(payload);
        uint64_t event_started_at_ms = turbo_monotonic_ms();
        ivr_status_t rc = ivr_worker_submit_event_copy(g_worker, &event);
        if (rc == IVR_OK) {
            metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_EVENT_TO_INBOX,
                                    event_started_at_ms);
        }
        printf("[ivr_worker] command result: message=%s status=%d routed=%s\n",
               result.message_id, result.status_code,
               rc == IVR_OK ? "yes" : "no");
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
            int startup_sync_rejected =
                status != IVR_OK && strncmp(message_id, "ws-", 3) == 0 &&
                ivr_worker_active_sessions(g_worker) > 0;
            if (startup_sync_rejected) {
                fprintf(stderr,
                        "ivr_worker: active sessions have no RoomService "
                        "recovery snapshot; draining fail-closed\n");
                ivr_worker_begin_drain(g_worker);
                g_running = 0;
            }
            data_bind_object_free(obj);
        } else {
            ivr_worker_metrics_inc(&g_metrics,
                                   IVR_WORKER_METRIC_REPLY_INVALID);
        }
        if (status == 0) {
            int command_ready = atomic_load_explicit(
                &g_command_connected, memory_order_acquire);
            int event_ready = atomic_load_explicit(
                &g_event_connected, memory_order_acquire);
            g_synced = 1;
            health_publish(1, 1, command_ready, event_ready, 1,
                           g_remote_configured, g_sfu_host && g_sfu_port > 0,
                           0, command_ready && event_ready,
                           command_ready && event_ready
                               ? "ready"
                               : "channel not connected");
            printf("[ivr_worker] worker.sync acknowledged (readiness)\n");
        } else {
            g_synced = 0;
            health_publish(
                1, 1,
                atomic_load_explicit(&g_command_connected,
                                     memory_order_acquire),
                atomic_load_explicit(&g_event_connected,
                                     memory_order_acquire),
                0,
                           g_remote_configured, g_sfu_host && g_sfu_port > 0,
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
    int previous;
    (void)ctx;
    previous = atomic_exchange_explicit(&g_command_connected, connected,
                                        memory_order_acq_rel);
    if (previous != connected) {
        ivr_worker_metrics_inc(
            &g_metrics, connected ? IVR_WORKER_METRIC_COMMAND_CONNECTED
                                  : IVR_WORKER_METRIC_COMMAND_DISCONNECTED);
    }
}

static void on_event_connection(void *ctx, int connected) {
    int previous;
    (void)ctx;
    previous = atomic_exchange_explicit(&g_event_connected, connected,
                                        memory_order_acq_rel);
    if (previous != connected) {
        ivr_worker_metrics_inc(
            &g_metrics, connected ? IVR_WORKER_METRIC_EVENT_CONNECTED
                                  : IVR_WORKER_METRIC_EVENT_DISCONNECTED);
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

static void on_event(void *ctx, const ivr_event_view_t *event) {
    (void)ctx;
    uint64_t event_started_at_ms = turbo_monotonic_ms();
    ivr_status_t rc = ivr_worker_submit_event_copy(g_worker, event);
    if (rc == IVR_OK) {
        metrics_observe_elapsed(IVR_WORKER_HISTOGRAM_EVENT_TO_INBOX,
                                event_started_at_ms);
    }
    printf("[ivr_worker] event: type=%.*s room=%.*s call=%.*s seq=%llu "
           "routed=%s\n",
           (int)event->event_type.size, event->event_type.data,
           (int)event->call.room_id.size, event->call.room_id.data,
           (int)event->call.call_id.size, event->call.call_id.data,
           (unsigned long long)event->sequence,
           rc == IVR_OK ? "yes" : "no");
}

/* ------------------------------------------------------------------ */
/* config / CLI                                                        */
/* ------------------------------------------------------------------ */

static void usage(const char *program) {
    printf("TurboNet IVR Worker v%s\n\n", IVR_WORKER_APP_VERSION);
    printf("Usage: %s [OPTIONS]\n\n", program);
    printf("  --worker-id ID        DEALER identity (default: ivr-worker-1)\n");
    printf("  --config PATH         TOML config (below env and CLI)\n");
    printf("  --content-root PATH   Content packages root (required)\n");
    printf("  --router-host HOST    RoomService ROUTER host (default: 127.0.0.1)\n");
    printf("  --router-port PORT    RoomService ROUTER port (required unless --dry-run)\n");
    printf("  --pub-host HOST       RoomService PUB host (default: 127.0.0.1)\n");
    printf("  --pub-port PORT       RoomService PUB port (required unless --dry-run)\n");
    printf("  --pub-topic TOPIC     SUB prefix (default: room.events)\n");
    printf("  --health-host HOST    Management bind (loopback only)\n");
    printf("  --health-port PORT    Management port (default: 18081)\n");
    printf("  --max-sessions N      Session slots (default: 4)\n");
    printf("  --heartbeat-ms N      Worker heartbeat interval (default: 5000)\n");
    printf("  --lease-ms N          RoomService lease (default: 15000, >=3H)\n");
    printf("  --assign ROOM,CALL[,PKG]  Assign a session at startup (repeatable)\n");
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
    size_t room_len = (size_t)(comma1 - value);
    size_t call_len = (size_t)((comma2 ? comma2 : value + strlen(value)) -
                               (comma1 + 1));
    char room[128];
    char call[128];
    char pkg[128];
    if (room_len == 0 || room_len >= sizeof(room) || call_len == 0 ||
        call_len >= sizeof(call)) {
        return -1;
    }
    memcpy(room, value, room_len);
    room[room_len] = '\0';
    memcpy(call, comma1 + 1, call_len);
    call[call_len] = '\0';
    if (comma2) {
        size_t pkg_len = strlen(comma2 + 1);
        if (pkg_len == 0 || pkg_len >= sizeof(pkg)) {
            return -1;
        }
        memcpy(pkg, comma2 + 1, pkg_len);
        pkg[pkg_len] = '\0';
    } else {
        snprintf(pkg, sizeof(pkg), "%s", "conference-greeting");
    }
    g_config.assign_room[index] = app_strdup(room);
    g_config.assign_call[index] = app_strdup(call);
    g_config.assign_pkg[index] = app_strdup(pkg);
    return 0;
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
        } else if (strcmp(arg, "--content-root") == 0 && ++i < argc) {
            g_config.content_root = argv[i];
        } else if (strcmp(arg, "--router-host") == 0 && ++i < argc) {
            g_config.router_host = argv[i];
        } else if (strcmp(arg, "--router-port") == 0 && ++i < argc) {
            if (parse_port_arg(argv[i], &g_config.router_port) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--pub-host") == 0 && ++i < argc) {
            g_config.pub_host = argv[i];
        } else if (strcmp(arg, "--pub-port") == 0 && ++i < argc) {
            if (parse_port_arg(argv[i], &g_config.pub_port) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--pub-topic") == 0 && ++i < argc) {
            g_config.pub_topic = argv[i];
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
    printf("  content_root: %s\n", g_config.content_root);
    printf("  router: %s:%d\n", g_config.router_host, g_config.router_port);
    printf("  pub: %s:%d topic=%s\n", g_config.pub_host, g_config.pub_port,
           g_config.pub_topic ? g_config.pub_topic : "room.events");
    printf("  health: %s:%d\n", g_config.health_host, g_config.health_port);
    printf("  max_sessions: %u\n", g_config.max_sessions);
    printf("  heartbeat_ms: %llu\n",
           (unsigned long long)g_config.heartbeat_ms);
    printf("  transport_timeout_ms: %llu\n",
           (unsigned long long)g_config.timeout_ms);
    printf("  lease_ms: %llu\n", (unsigned long long)g_config.lease_ms);
    printf("  gateway: %s\n", g_config.shadow ? "shadow (capture)" : "forward");
    printf("  fmq_security: %s rotation_generation=%d\n",
           g_config.fmq_use_tls
               ? "mTLS"
               : (g_config.fmq_allow_insecure_loopback
                      ? "trusted-loopback"
                      : "disabled"),
           g_config.fmq_tls_rotation_generation);
    printf("  assignments: %d\n", g_config.assign_count);
}

static void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    /* logging daemon: never buffer stdout so SIGTERM/force-kill does not lose
       the last diagnostics */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (remote_speech_init() != 0) {
        return 1;
    }
    g_config.shadow = 0;
    g_config.worker_id = "ivr-worker-1";
    g_config.router_host = "127.0.0.1";
    g_config.router_port = 0;
    g_config.pub_host = "127.0.0.1";
    g_config.pub_port = 0;
    g_config.pub_topic = "room.events";
    g_config.health_host = "127.0.0.1";
    g_config.health_port = IVR_WORKER_DEFAULT_HEALTH_PORT;
    g_config.max_sessions = 4;
    g_config.timeout_ms = 0;
    g_config.heartbeat_ms = IVR_WORKER_DEFAULT_HEARTBEAT_MS;
    g_config.lease_ms = IVR_WORKER_DEFAULT_LEASE_MS;
    g_config.fmq_use_tls = 0;
    g_config.fmq_allow_insecure_loopback = 0;
    g_config.fmq_tls_rotation_generation = 1;
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
        (g_config.call_scope && !ivr_acl_scope_valid(g_config.call_scope)) ||
        (g_config.content_capabilities &&
         !ivr_acl_scope_valid(g_config.content_capabilities))) {
        fprintf(stderr,
                "ivr_worker: invalid tenant/room/call/content scope config\n");
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
    if (!g_config.content_root || g_config.content_root[0] == '\0' ||
        !g_config.health_host ||
        (strcmp(g_config.health_host, "127.0.0.1") != 0 &&
         strcmp(g_config.health_host, "::1") != 0) ||
        g_config.health_port <= 0 || g_config.health_port > UINT16_MAX ||
        g_config.max_sessions == 0 || g_config.heartbeat_ms == 0 ||
        g_config.heartbeat_ms > UINT64_MAX / 3u ||
        g_config.lease_ms < g_config.heartbeat_ms * 3u) {
        fprintf(stderr,
                "ivr_worker: content/max-sessions and lease>=3*heartbeat "
                "are required\n");
        ivr_worker_health_destroy(&g_health);
        g_health_initialized = 0;
        return 1;
    }
    /* Keep FlowMQ's idle deadline strictly beyond the heartbeat deadline.
       The lease validation above makes this multiplication overflow-safe. */
    g_config.timeout_ms =
        g_config.heartbeat_ms * IVR_WORKER_TRANSPORT_TIMEOUT_HEARTBEATS;
    if (!g_config.dry_run &&
        (g_config.router_port <= 0 || g_config.pub_port <= 0)) {
        fprintf(stderr,
                "ivr_worker: --router-port and --pub-port required "
                "(or use --dry-run)\n");
        return 1;
    }
    if (g_config.fmq_use_tls) {
        size_t secret_size = g_config.fmq_shared_secret
                                 ? strlen(g_config.fmq_shared_secret)
                                 : 0u;
        if (g_config.fmq_allow_insecure_loopback || !g_config.fmq_ca_file ||
            !g_config.fmq_ca_file[0] || !g_config.fmq_cert_file ||
            !g_config.fmq_cert_file[0] || !g_config.fmq_key_file ||
            !g_config.fmq_key_file[0] || !g_config.fmq_server_name ||
            !g_config.fmq_server_name[0] || secret_size < 32u ||
            secret_size > 4096u ||
            g_config.fmq_tls_rotation_generation <= 0) {
            fprintf(stderr,
                    "ivr_worker: complete FlowMQ mTLS identity and shared "
                    "secret configuration is required\n");
            return 1;
        }
    } else if (!g_config.dry_run) {
        if (!g_config.shadow || !g_config.fmq_allow_insecure_loopback ||
            !ivr_worker_is_loopback(g_config.router_host) ||
            !ivr_worker_is_loopback(g_config.pub_host)) {
            fprintf(stderr,
                    "ivr_worker: active mode requires FlowMQ mTLS; plaintext "
                    "is limited to explicit loopback shadow mode\n");
            return 1;
        }
    }
    configure_media_from_environment();
    if (g_env_parse_failed || g_media_sample_rate == 0 ||
        g_media_sample_rate > 192000u) {
        fprintf(stderr, "ivr_worker: invalid media environment configuration\n");
        return 1;
    }
    ivr_content_package_t startup_package;
    memset(&startup_package, 0, sizeof(startup_package));
    if (ivr_content_package_load(g_config.content_root,
                                 "conference-greeting",
                                 &startup_package) != IVR_OK) {
        fprintf(stderr,
                "ivr_worker: default content package is missing or invalid\n");
        return 1;
    }
    if (!g_config.dry_run && !g_config.shadow) {
        char available[128];
        snprintf(available, sizeof(available), "turboxml,flowmq%s%s",
                 g_remote_configured ? ",tts,asr" : "",
                 g_sfu_host && g_sfu_host[0] && g_sfu_port > 0
                     ? ",whip,whep"
                     : "");
        if (!g_remote_configured ||
            !(g_sfu_host && g_sfu_host[0] && g_sfu_port > 0) ||
            ivr_content_capabilities_satisfied(&startup_package, available) !=
                IVR_OK) {
            fprintf(stderr,
                    "ivr_worker: active mode dependencies are not ready "
                    "(speech/SFU/content capabilities)\n");
            ivr_content_package_free(&startup_package);
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
    ivr_content_package_free(&startup_package);
    print_config();

    if (g_config.dry_run) {
        /* exercise worker + session creation (and content loading) without
           touching FlowMQ, then drain and exit */
        ivr_command_gateway_ops_t shadow_ops;
        memset(&shadow_ops, 0, sizeof(shadow_ops));
        shadow_ops.abi_version = 1;
        shadow_ops.context = &g_cli_gateway;
        shadow_ops.submit_copy = cli_gateway_submit;
        g_cli_gateway.real = NULL;
        g_cli_gateway.shadow = 1;
        ivr_media_port_factory_ops_t media_factory;
        memset(&media_factory, 0, sizeof(media_factory));
        media_factory.abi_version = IVR_WORKER_ABI_VERSION;
        media_factory.context = &g_speech_factory;
        media_factory.create = media_factory_create;
        media_factory.destroy = media_factory_destroy;

        ivr_worker_config_t wcfg;
        memset(&wcfg, 0, sizeof(wcfg));
        wcfg.abi_version = IVR_WORKER_ABI_VERSION;
        wcfg.worker_id = g_config.worker_id;
        wcfg.max_sessions_per_worker = g_config.max_sessions;
        wcfg.session_inbox_capacity = 8;
        wcfg.max_event_bytes = 65536;
        wcfg.max_command_bytes = 16384;
        wcfg.content_root = g_config.content_root;
        wcfg.drain_deadline_ms = 5000;
        wcfg.observer.abi_version = IVR_SESSION_OBSERVER_ABI_VERSION;
        wcfg.observer.context = &g_metrics;
        wcfg.observer.on_latency = session_observe_latency;
        if (ivr_worker_create(&wcfg, &shadow_ops, &media_factory, &g_worker) !=
            IVR_OK) {
            fprintf(stderr, "ivr_worker: dry-run worker create failed\n");
            return 1;
        }
        ivr_worker_start(g_worker);
        for (int i = 0; i < g_config.assign_count; i++) {
            ivr_call_ref_t call;
            memset(&call, 0, sizeof(call));
            call.room_id.data = g_config.assign_room[i];
            call.room_id.size = strlen(g_config.assign_room[i]);
            call.call_id.data = g_config.assign_call[i];
            call.call_id.size = strlen(g_config.assign_call[i]);
            call.call_generation = 1;
            ivr_session_t *session = NULL;
            if (ivr_worker_assign_session(g_worker, &call,
                                          g_config.assign_pkg[i],
                                          &session) != IVR_OK) {
                fprintf(stderr, "ivr_worker: dry-run assign %s/%s failed\n",
                        g_config.assign_room[i], g_config.assign_call[i]);
                ivr_worker_begin_drain(g_worker);
                ivr_worker_destroy(g_worker);
                return 1;
            }
        }
        printf("ivr_worker: dry-run complete\n");
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        return 0;
    }

    turbo_uuid_t instance_uuid;
    if (turbo_uuid_v4_generate(&instance_uuid) != TURBO_OK ||
        turbo_uuid_format(&instance_uuid, g_instance_id,
                          sizeof(g_instance_id)) != TURBO_OK) {
        fprintf(stderr, "ivr_worker: instance UUID generation failed\n");
        return 1;
    }

    /* ---- live path ---- */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (ivr_worker_http_create(&g_health, &g_management_http) != 0 ||
        ivr_worker_http_set_metrics(g_management_http, &g_metrics) != 0 ||
        ivr_worker_http_start(g_management_http, g_config.health_host,
                              g_config.health_port) != 0) {
        fprintf(stderr, "ivr_worker: management listener start failed on %s:%d\n",
                g_config.health_host, g_config.health_port);
        management_http_stop();
        return 1;
    }

    if (reply_queue_init() != 0) {
        fprintf(stderr, "ivr_worker: FlowMQ reply queue create failed\n");
        management_http_stop();
        return 1;
    }
    atomic_store_explicit(&g_command_connected, 0, memory_order_release);
    atomic_store_explicit(&g_event_connected, 0, memory_order_release);

    turbo_flow_fmq_tls_config_t fmq_tls = TURBO_FLOW_FMQ_TLS_CONFIG_INIT;
    if (g_config.fmq_use_tls) {
        ivr_fmq_client_security_config_t security_config =
            IVR_FMQ_CLIENT_SECURITY_CONFIG_INIT;
        security_config.shared_secret = g_config.fmq_shared_secret;
        if (ivr_fmq_client_security_create(&security_config,
                                           &g_fmq_security) != TURBO_OK) {
            fprintf(stderr, "ivr_worker: FlowMQ security owner create failed\n");
            reply_queue_destroy();
            management_http_stop();
            return 1;
        }
        fmq_tls.ca_file = g_config.fmq_ca_file;
        fmq_tls.cert_file = g_config.fmq_cert_file;
        fmq_tls.key_file = g_config.fmq_key_file;
        fmq_tls.key_password = g_config.fmq_key_password;
        fmq_tls.server_name = g_config.fmq_server_name;
        fmq_tls.verify_peer = 1;
        fmq_tls.require_client_certificate = 0;
        fmq_tls.rotation_generation =
            (uint64_t)g_config.fmq_tls_rotation_generation;
    }
    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = g_config.worker_id;
    gcfg.host = g_config.router_host;
    gcfg.port = g_config.router_port;
    gcfg.timeout_ms = g_config.timeout_ms;
    gcfg.transport = g_config.fmq_use_tls ? TURBO_FLOW_FMQ_TLS
                                          : TURBO_FLOW_FMQ_TCP;
    gcfg.tls = g_config.fmq_use_tls ? &fmq_tls : NULL;
    gcfg.security = ivr_fmq_security_binding(g_fmq_security);
    gcfg.on_reply = on_reply;
    gcfg.on_connection = on_command_connection;
    if (ivr_flowmq_gateway_create(&gcfg, &g_real_ops, &g_gateway) != IVR_OK) {
        fprintf(stderr, "ivr_worker: gateway create failed\n");
        reply_queue_destroy();
        management_http_stop();
        ivr_fmq_security_destroy(g_fmq_security);
        g_fmq_security = NULL;
        return 1;
    }

    if (g_config.pub_port > 0) {
        ivr_flowmq_subscriber_config_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        scfg.identity = g_config.worker_id;
        scfg.host = g_config.pub_host;
        scfg.port = g_config.pub_port;
        scfg.topic = g_config.pub_topic;
        scfg.timeout_ms = g_config.timeout_ms;
        scfg.transport = g_config.fmq_use_tls ? TURBO_FLOW_FMQ_TLS
                                              : TURBO_FLOW_FMQ_TCP;
        scfg.tls = g_config.fmq_use_tls ? &fmq_tls : NULL;
        scfg.security = ivr_fmq_security_binding(g_fmq_security);
        scfg.on_event = on_event;
        scfg.on_connection = on_event_connection;
        if (ivr_flowmq_subscriber_create(&scfg, &g_subscriber) != IVR_OK) {
            fprintf(stderr, "ivr_worker: subscriber create failed\n");
            ivr_flowmq_gateway_destroy(g_gateway);
            reply_queue_destroy();
            management_http_stop();
            ivr_fmq_security_destroy(g_fmq_security);
            g_fmq_security = NULL;
            return 1;
        }
    }

    g_cli_gateway.real = &g_real_ops;
    g_cli_gateway.shadow = g_config.shadow;
    ivr_command_gateway_ops_t worker_ops;
    memset(&worker_ops, 0, sizeof(worker_ops));
    worker_ops.abi_version = 1;
    worker_ops.context = &g_cli_gateway;
    worker_ops.submit_copy = cli_gateway_submit;
    ivr_media_port_factory_ops_t media_factory;
    memset(&media_factory, 0, sizeof(media_factory));
    media_factory.abi_version = IVR_WORKER_ABI_VERSION;
    media_factory.context = &g_speech_factory;
    media_factory.create = media_factory_create;
    media_factory.destroy = media_factory_destroy;

    ivr_worker_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.abi_version = IVR_WORKER_ABI_VERSION;
    wcfg.worker_id = g_config.worker_id;
    wcfg.max_sessions_per_worker = g_config.max_sessions;
    wcfg.session_inbox_capacity = 8;
    wcfg.max_event_bytes = 65536;
    wcfg.max_command_bytes = 16384;
    wcfg.content_root = g_config.content_root;
    wcfg.drain_deadline_ms = 5000;
    wcfg.observer.abi_version = IVR_SESSION_OBSERVER_ABI_VERSION;
    wcfg.observer.context = &g_metrics;
    wcfg.observer.on_latency = session_observe_latency;
    if (ivr_worker_create(&wcfg, &worker_ops, &media_factory, &g_worker) != IVR_OK) {
        fprintf(stderr, "ivr_worker: worker create failed\n");
        if (g_subscriber) {
            ivr_flowmq_subscriber_destroy(g_subscriber);
        }
        ivr_flowmq_gateway_destroy(g_gateway);
        reply_queue_destroy();
        management_http_stop();
        ivr_fmq_security_destroy(g_fmq_security);
        g_fmq_security = NULL;
        return 1;
    }
    ivr_worker_start(g_worker);

    if (ivr_flowmq_gateway_start(g_gateway) != IVR_OK ||
        (g_subscriber && ivr_flowmq_subscriber_start(g_subscriber) != IVR_OK)) {
        fprintf(stderr, "ivr_worker: start failed\n");
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        if (g_subscriber) {
            ivr_flowmq_subscriber_destroy(g_subscriber);
        }
        ivr_flowmq_gateway_destroy(g_gateway);
        reply_queue_destroy();
        management_http_stop();
        ivr_fmq_security_destroy(g_fmq_security);
        g_fmq_security = NULL;
        return 1;
    }

    for (int i = 0; i < g_config.assign_count; i++) {
        ivr_call_ref_t call;
        memset(&call, 0, sizeof(call));
        call.room_id.data = g_config.assign_room[i];
        call.room_id.size = strlen(g_config.assign_room[i]);
        call.call_id.data = g_config.assign_call[i];
        call.call_id.size = strlen(g_config.assign_call[i]);
        call.call_generation = 1;
        ivr_session_t *session = NULL;
        if (ivr_worker_assign_session(g_worker, &call,
                                      g_config.assign_pkg[i],
                                      &session) != IVR_OK) {
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
    int observed_event_connected = -1;
    uint64_t heartbeat_sequence = 0;
    uint64_t health_revocation_sequence = 0;
    uint64_t next_heartbeat_ms =
        turbo_monotonic_ms() + g_config.heartbeat_ms;
    int ticks = 0;
    while (g_running) {
        int command_connected;
        int event_connected;
        process_replies();
        ivr_thread_sleep_ms(100);
        command_connected = atomic_load_explicit(
            &g_command_connected, memory_order_acquire);
        event_connected = atomic_load_explicit(
            &g_event_connected, memory_order_acquire);
        if (command_connected != observed_command_connected ||
            event_connected != observed_event_connected) {
            observed_command_connected = command_connected;
            observed_event_connected = event_connected;
            if (!command_connected || !event_connected) {
                int was_synced = g_synced;
                char mid[64];
                health_publish(1, 1, command_connected, event_connected, 0,
                               g_remote_configured,
                               g_sfu_host && g_sfu_port > 0, 0, 0,
                               command_connected
                                   ? "event channel disconnected"
                                   : "command channel disconnected");
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
        if (!g_synced && command_connected && event_connected &&
            (++ticks % 20) == 0) {
            char mid[64];
            snprintf(mid, sizeof(mid), "ws-startup-%d", sync_retries++);
            ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_SYNC_RETRY);
            printf("[ivr_worker] worker.sync retry %s\n", mid);
            if (send_worker_sync_v2(mid) != IVR_OK) {
                printf("[ivr_worker] worker.sync retry %d send failed\n",
                       sync_retries);
            }
        }
        if (g_synced && command_connected && event_connected &&
            turbo_monotonic_ms() >= next_heartbeat_ms) {
            char mid[64];
            snprintf(mid, sizeof(mid), "heartbeat-%llu",
                     (unsigned long long)++heartbeat_sequence);
            if (send_worker_heartbeat(mid) != IVR_OK) {
                ivr_worker_metrics_inc(
                    &g_metrics, IVR_WORKER_METRIC_HEARTBEAT_FAILURE);
                g_synced = 0;
                health_publish(1, 1, 0, event_connected, 0,
                               g_remote_configured,
                               g_sfu_host && g_sfu_port > 0, 0, 0,
                               "heartbeat send failed");
                printf("[ivr_worker] heartbeat send failed (transport_timeout_ms=%llu); "
                       "readiness revoked\n",
                       (unsigned long long)g_config.timeout_ms);
            }
            next_heartbeat_ms =
                turbo_monotonic_ms() + g_config.heartbeat_ms;
        }
    }

    printf("ivr_worker: draining\n");
    ivr_worker_metrics_inc(&g_metrics, IVR_WORKER_METRIC_DRAIN);
    health_publish(1, 1, 0, 0, 0, g_remote_configured,
                   g_sfu_host && g_sfu_port > 0, 1, 0, "draining");
    atomic_store_explicit(&g_accept_replies, 0, memory_order_release);
    if (g_subscriber) {
        ivr_flowmq_subscriber_destroy(g_subscriber);
        g_subscriber = NULL;
    }
    ivr_flowmq_gateway_destroy(g_gateway);
    g_gateway = NULL;
    ivr_fmq_security_destroy(g_fmq_security);
    g_fmq_security = NULL;
    ivr_worker_begin_drain(g_worker);
    ivr_worker_metrics_add(&g_metrics, IVR_WORKER_METRIC_DRAIN_TIMEOUT,
                           ivr_worker_drain_timed_out(g_worker));
    ivr_worker_destroy(g_worker);
    reply_queue_destroy();
    management_http_stop();
    ivr_worker_health_destroy(&g_health);
    g_health_initialized = 0;
    return 0;
}
