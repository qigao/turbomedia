#include "monitor.h"

#include <chttp_app/application.h>
#include <core/clock.h>
#include <core/fmt.h>
#include <file/fs.h>
#include <libavformat/avformat.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct monitor_web {
    monitor_t *monitor;
    int port;
    tstr assets[3];
    salts_configurator_provider_binding provider;
    salts_configurator_deployment deployment;
} monitor_web_t;

cmeta_reflect_data(monitor_web_t, "turbomedia.monitor.Web", cmeta_field(int, port));
cmeta_component_empty(TurboMonitorWeb);

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static bool parse_number(const char *text, uint64_t max, uint64_t *value) {
    uint64_t number = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        unsigned digit = (unsigned)(unsigned char)*text - '0';
        if (digit > 9 || digit > max || number > (max - digit) / 10) return false;
        number = number * 10 + digit;
    }
    *value = number;
    return true;
}

static int text_reply(const chttp_invocation *call, unsigned status, const char *message) {
    return chttp_invocation_respond(call, status, "text/plain; charset=utf-8",
                                   message, strlen(message));
}

static bool local_request(const monitor_web_t *web, const chttp_invocation *call) {
    const char *host = chttp_server_request_header(chttp_invocation_request(call), "Host");
    uint64_t port;
    return host && strncmp(host, "127.0.0.1:", 10) == 0 &&
           parse_number(host + 10, UINT16_MAX, &port) && port == (uint64_t)web->port;
}

static monitor_source_t *source_from_request(
    monitor_t *monitor, const chttp_invocation *call) {
    const char *id = chttp_server_request_param(chttp_invocation_request(call), "id");
    uint64_t value;
    if (!parse_number(id, monitor->count, &value) || !value) return NULL;
    return &monitor->sources[value - 1];
}

static int static_asset(monitor_web_t *web, const chttp_invocation *call,
                        unsigned index, const char *mime) {
    if (!local_request(web, call)) return text_reply(call, 403, "Loopback host required");
    (void)chttp_invocation_set_header(call, "Content-Security-Policy",
        "default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
    (void)chttp_invocation_set_header(call, "X-Content-Type-Options", "nosniff");
    const chttp_server_file_options file = { .path = web->assets[index], .content_type = mime };
    int status = chttp_invocation_serve_file(call, &file);
    return status == 0 ? status : text_reply(call, 500, "Web asset unavailable");
}

CHTTP_ACTION(monitor_ui);
int monitor_ui(void *user, const chttp_invocation *call) {
    return static_asset(user, call, 0, "text/html; charset=utf-8");
}

CHTTP_ACTION(monitor_css);
int monitor_css(void *user, const chttp_invocation *call) {
    return static_asset(user, call, 1, "text/css; charset=utf-8");
}

CHTTP_ACTION(monitor_js);
int monitor_js(void *user, const chttp_invocation *call) {
    return static_asset(user, call, 2, "text/javascript; charset=utf-8");
}

CHTTP_ACTION(monitor_status);
int monitor_status(void *user, const chttp_invocation *call) {
    const monitor_web_t *web = user;
    static const char *const signal_names[] = {"connecting", "live", "offline", "unsupported"};
    static const char *const clip_names[] = {"none", "pending", "ready", "unavailable"};
    if (!local_request(web, call)) return text_reply(call, 403, "Loopback host required");
    tstr json = tstr_new_len("[", 1);
    if (!json) return text_reply(call, 503, "Status unavailable");
    for (unsigned i = 0; i < web->monitor->count; ++i) {
        monitor_source_t *source = &web->monitor->sources[i];
        sync_mutex_lock(&source->mutex);
        monitor_state_t state = source->state;
        sync_mutex_unlock(&source->mutex);
        tstr next = tstr_append_format(json,
            "{}{{\"id\":{},\"name\":\"{}\",\"signal\":\"{}\",\"clip\":\"{}\","
            "\"preview\":{},\"lastFrame\":{},\"incident\":{},\"opened\":{},"
            "\"recovered\":{},\"acknowledged\":{}}}",
            i ? "," : "", source->id, source->name, signal_names[state.signal],
            clip_names[state.clip], state.preview_version, state.last_frame_ms, state.incident,
            state.opened_ms, state.recovered_ms, state.acknowledged ? "true" : "false");
        if (!next) {
            tstr_free(json);
            return text_reply(call, 503, "Status unavailable");
        }
        json = next;
    }
    tstr next = tstr_cat(json, "]");
    if (!next) {
        tstr_free(json);
        return text_reply(call, 503, "Status unavailable");
    }
    json = next;
    (void)chttp_invocation_set_header(call, "Cache-Control", "no-store");
    int status = chttp_invocation_respond(call, 200, "application/json", json, tstr_len(json));
    tstr_free(json);
    return status;
}

CHTTP_ACTION(monitor_preview);
int monitor_preview(void *user, const chttp_invocation *call) {
    const monitor_web_t *web = user;
    if (!local_request(web, call)) return text_reply(call, 403, "Loopback host required");
    monitor_source_t *source = source_from_request(web->monitor, call);
    if (!source) return text_reply(call, 404, "Source not found");
    sync_mutex_lock(&source->mutex);
    mem_buffer_t *preview = source->preview ? mem_buffer_retain(source->preview) : NULL;
    sync_mutex_unlock(&source->mutex);
    if (!preview) return text_reply(call, 404, "Preview unavailable");
    (void)chttp_invocation_set_header(call, "Cache-Control", "no-store");
    int status = chttp_invocation_respond(call, 200, "image/jpeg",
        mem_buffer_const_data(preview), mem_buffer_used(preview));
    mem_buffer_release(preview);
    return status;
}

CHTTP_ACTION(monitor_ack);
int monitor_ack(void *user, const chttp_invocation *call) {
    const monitor_web_t *web = user;
    const chttp_server_request_view *request = chttp_invocation_request(call);
    const char *action = chttp_server_request_header(request, "X-Monitor-Action");
    if (!local_request(web, call) || !action || strcmp(action, "acknowledge") != 0)
        return text_reply(call, 403, "Confirmation header required");
    monitor_source_t *source = source_from_request(web->monitor, call);
    uint64_t incident;
    if (!source || !parse_number(chttp_server_request_param(request, "incident"),
                                 UINT64_MAX, &incident) || !incident)
        return text_reply(call, 404, "Incident not found");
    sync_mutex_lock(&source->mutex);
    bool current = source->state.incident == incident;
    if (current) source->state.acknowledged = true;
    sync_mutex_unlock(&source->mutex);
    return current ? text_reply(call, 200, "Acknowledged")
                   : text_reply(call, 409, "Incident changed");
}

CHTTP_ACTION(monitor_recording);
int monitor_recording(void *user, const chttp_invocation *call) {
    const monitor_web_t *web = user;
    if (!local_request(web, call)) return text_reply(call, 403, "Loopback host required");
    monitor_source_t *source = source_from_request(web->monitor, call);
    const chttp_server_request_view *request = chttp_invocation_request(call);
    uint64_t incident;
    if (!source || !parse_number(chttp_server_request_param(request, "incident"),
                                 UINT64_MAX, &incident) || !incident)
        return text_reply(call, 404, "Recording not found");
    sync_mutex_lock(&source->mutex);
    bool ready = source->state.incident == incident && source->state.clip == MONITOR_CLIP_READY;
    sync_mutex_unlock(&source->mutex);
    if (!ready) return text_reply(call, 404, "Recording not available");
    tstr path = monitor_clip_path(source, incident);
    if (!path) return text_reply(call, 503, "Recording unavailable");
    const chttp_server_file_options file = {.path = path, .content_type = "video/mp4"};
    int status = chttp_invocation_serve_file(call, &file);
    tstr_free(path);
    return status == 0 ? status : text_reply(call, 404, "Recording unavailable");
}

static cmeta_status borrow_web(void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *dependencies, size_t count, cmeta_object_ref *out) {
    (void)data;
    (void)config;
    (void)dependencies;
    if (count) return CMETA_INVALID_ARGUMENT;
    return cmeta_object_borrow(out, user, cmeta_reflected_data(monitor_web_t), NULL);
}

static int start_web(chttp_application *http, monitor_web_t *web) {
    const chttp_action routes[] = {
        CHTTP_GET("/", monitor_ui),
        CHTTP_GET("/monitor.css", monitor_css),
        CHTTP_GET("/monitor.js", monitor_js),
        CHTTP_GET("/api/status", monitor_status),
        CHTTP_GET("/api/preview/:id", monitor_preview),
        CHTTP_GET("/api/recording/:id/:incident", monitor_recording),
        CHTTP_POST("/api/ack/:id/:incident", monitor_ack)
    };
    const chttp_controller controller = {
        .component_id = "TurboMonitorWeb",
        .receiver = cmeta_reflected_data(monitor_web_t),
        .actions = routes, .action_count = sizeof(routes) / sizeof(routes[0])
    };
    web->provider = (salts_configurator_provider_binding){
        sizeof(web->provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(TurboMonitorWeb), web, NULL, borrow_web, NULL, NULL
    };
    web->deployment = (salts_configurator_deployment){&web->provider, NULL, NULL};
    chttp_application_config config = chttp_application_config_default();
    config.http.host = "127.0.0.1";
    config.http.port = (uint16_t)web->port;
    config.http.enable_http2 = 0;
    config.http.h2_stream_capacity = 0;
    config.http.h2_input_buffer_bytes = 0;
    config.http.h2_output_buffer_bytes = 0;
    config.http.h2_hpack_dynamic_table_bytes = 0;
    config.http.h2_max_settings_count = 0;
    config.http.max_response_body_bytes = MONITOR_PREVIEW_BYTES;
    config.http.max_buffered_response_body_bytes = MONITOR_PREVIEW_BYTES;
    config.http.network.max_send_bytes = MONITOR_PREVIEW_BYTES + 16384;
    config.workers.reply_bytes = MONITOR_PREVIEW_BYTES;
    config.components = &web->deployment;
    config.component_count = 1;
    config.dependency_capacity = 1;
    config.controllers = &controller;
    config.controller_count = 1;
    int result = chttp_application_init(http, &config);
    if (result != 0) return result;
    return chttp_application_start(http);
}

static bool valid_name(const char *name) {
    size_t size = strlen(name);
    if (!size || size > 64 || !vstr_utf8_valid(vstr_from_buf(name, size))) return false;
    /* Only printable label bytes go into the JSON string. */
    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = (unsigned char)name[i];
        if (byte < 32 || byte == 127 || byte == '"' || byte == '\\') return false;
    }
    return true;
}

int main(int argc, char **argv) {
    static const char *const names[] = {"index.html", "monitor.css", "monitor.js"};
    monitor_t monitor = {0};
    monitor_web_t web = {.monitor = &monitor, .port = 8088};
    chttp_application http = {0};
    const char *archive = NULL, *assets = NULL;
    uint64_t duration = 0, number, until;
    int result = EXIT_FAILURE;
    bool started_media = false;
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        if (!strcmp(option, "--source") && i + 2 < argc && monitor.count < MONITOR_MAX_SOURCES) {
            const char *name = argv[++i], *url = argv[++i];
            if (!valid_name(name) || strncmp(url, "rtsp://", 7) != 0 || !url[7] ||
                strlen(url) > 2048) goto usage;
            monitor.sources[monitor.count].name = name;
            monitor.sources[monitor.count++].url = url;
        } else if (!strcmp(option, "--archive") && i + 1 < argc && !archive) {
            archive = argv[++i];
        } else if (!strcmp(option, "--web") && i + 1 < argc && !assets) {
            assets = argv[++i];
        } else if (!strcmp(option, "--port") && i + 1 < argc &&
                   parse_number(argv[++i], UINT16_MAX, &number) && number != 0) {
            web.port = (int)number;
        } else if (!strcmp(option, "--run-seconds") && i + 1 < argc &&
                   parse_number(argv[++i], UINT32_MAX, &number) && number != 0) {
            duration = number * 1000;
        } else {
            goto usage;
        }
    }
    if (!monitor.count || !archive || !assets) goto usage;
    for (unsigned i = 0; i < 3; ++i) {
        fs_stat_t info;
        web.assets[i] = tstr_format("{}/{}", assets, names[i]);
        if (!web.assets[i] || fs_stat(web.assets[i], &info) != 0 || !info.is_file) {
            fputs("Monitor web assets are unavailable.\n", stderr);
            goto done;
        }
    }
    monitor.archive = tstr_format("{}/monitor-{}-{}", archive, realtime_ms(), process_id());
    if (!monitor.archive || fs_mkdir(monitor.archive, 0700) != 0) {
        fputs("Cannot create a unique directory under --archive.\n", stderr);
        goto done;
    }
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_network_init() < 0) goto done;
    started_media = true;
    if (!monitor_start(&monitor)) {
        fputs("Cannot start the media workers.\n", stderr);
        goto done;
    }
    int status = start_web(&http, &web);
    if (status != 0) {
        fprintf(stderr, "Cannot start the monitor HTTP service (%d).\n", status);
        goto done;
    }
    printf("http://127.0.0.1:%d/\n", web.port);
    fflush(stdout);
    until = monotonic_ms() + duration;
    while (!stopping && (!duration || monotonic_ms() < until)) sleep_ms(50);
    result = EXIT_SUCCESS;
done:
    if (http.impl && chttp_application_close(&http, 0) != 0) result = EXIT_FAILURE;
    if (http.impl) return EXIT_FAILURE;
    if (started_media) {
        monitor_stop(&monitor);
        avformat_network_deinit();
    }
    tstr_free(monitor.archive);
    for (unsigned i = 0; i < 3; ++i) tstr_free(web.assets[i]);
    return result;
usage:
    fputs("Usage: turbo_monitor_console --web WEB_DIR --archive EXISTING_DIR "
          "[--port PORT] [--run-seconds N] --source NAME RTSP_URL [--source NAME RTSP_URL ...]\n",
          stderr);
    return EXIT_FAILURE;
}
