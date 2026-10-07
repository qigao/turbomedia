/**
 * rtc_live_audio.c - Minimal owner-driven WHIP/WHEP live audio client.
 *
 * Examples:
 *   rtc_live_audio list
 *   rtc_live_audio publish --base-url https://host:port --path /whip/room/alice
 *   rtc_live_audio subscribe --base-url https://host:port --path /whep/room/bob
 *
 * Credentials are read from environment variables and are never logged.
 */

#include "turbo_rtc_capture_source.h"
#include "turbo_rtc_client.h"
#include "turbo_rtc_playback_sink.h"
#include "turbo_rtc_subscriber.h"

#include <salts/thread.h>
#include <salts_capture.h>
#include <salts_playback.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_MAX_ICE_SERVERS TURBO_RTC_CLIENT_MAX_ICE_SERVERS
#define LIVE_POLL_SLEEP_MS 1u

typedef enum live_mode_e {
    LIVE_MODE_NONE = 0,
    LIVE_MODE_LIST,
    LIVE_MODE_PUBLISH,
    LIVE_MODE_SUBSCRIBE
} live_mode_t;

typedef struct live_options_s {
    live_mode_t mode;
    const char *base_url;
    const char *path;
    const char *token_env;
    const char *ca_file;
    const char *server_name;
    const char *capture_device_id;
    int playback_device_index;
    int allow_plaintext_loopback;
    int allow_loopback;

    const char *stun_servers[LIVE_MAX_ICE_SERVERS];
    size_t stun_server_count;
    const char *turn_servers[LIVE_MAX_ICE_SERVERS];
    size_t turn_server_count;
} live_options_t;

static volatile sig_atomic_t g_running = 1;

static void live_signal_handler(int signal_value) {
    (void)signal_value;
    g_running = 0;
}

static void live_print_usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s list\n"
            "  %s publish --base-url URL --path PATH [options]\n"
            "  %s subscribe --base-url URL --path PATH [options]\n"
            "\n"
            "Common options:\n"
            "  --token-env NAME             bearer token environment variable\n"
            "                               (default: TURBO_RTC_TOKEN)\n"
            "  --stun URL                   repeatable STUN URL, max %u\n"
            "  --turn-env NAME              repeatable env var containing TURN URL\n"
            "  --ca-file PATH               TLS CA bundle\n"
            "  --server-name NAME           TLS server-name override\n"
            "  --allow-loopback             allow loopback ICE candidates\n"
            "  --allow-plaintext-loopback   allow http:// only for loopback\n"
            "\n"
            "Publish options:\n"
            "  --capture-device ID          exact ID printed by 'list'\n"
            "\n"
            "Subscribe options:\n"
            "  --playback-device INDEX      exact index printed by 'list'\n",
            program, program, program, (unsigned)LIVE_MAX_ICE_SERVERS);
}

static int live_parse_nonnegative_int(const char *text, int *out_value) {
    char *end = NULL;
    long value;
    if (!text || !text[0] || !out_value) {
        return -1;
    }
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 0 || value > 0x7fffffffL) {
        return -1;
    }
    *out_value = (int)value;
    return 0;
}

static int live_parse_options(int argc, char **argv, live_options_t *options) {
    int i;
    if (!options || argc < 2) {
        return -1;
    }

    memset(options, 0, sizeof(*options));
    options->token_env = "TURBO_RTC_TOKEN";
    options->playback_device_index = -1;

    if (strcmp(argv[1], "list") == 0) {
        options->mode = LIVE_MODE_LIST;
        return argc == 2 ? 0 : -1;
    }
    if (strcmp(argv[1], "publish") == 0) {
        options->mode = LIVE_MODE_PUBLISH;
    } else if (strcmp(argv[1], "subscribe") == 0) {
        options->mode = LIVE_MODE_SUBSCRIBE;
    } else {
        return -1;
    }

    for (i = 2; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--base-url") == 0 && i + 1 < argc) {
            options->base_url = argv[++i];
        } else if (strcmp(arg, "--path") == 0 && i + 1 < argc) {
            options->path = argv[++i];
        } else if (strcmp(arg, "--token-env") == 0 && i + 1 < argc) {
            options->token_env = argv[++i];
        } else if (strcmp(arg, "--ca-file") == 0 && i + 1 < argc) {
            options->ca_file = argv[++i];
        } else if (strcmp(arg, "--server-name") == 0 && i + 1 < argc) {
            options->server_name = argv[++i];
        } else if (strcmp(arg, "--capture-device") == 0 && i + 1 < argc) {
            options->capture_device_id = argv[++i];
        } else if (strcmp(arg, "--playback-device") == 0 && i + 1 < argc) {
            if (live_parse_nonnegative_int(
                    argv[++i], &options->playback_device_index) != 0) {
                return -1;
            }
        } else if (strcmp(arg, "--stun") == 0 && i + 1 < argc) {
            if (options->stun_server_count >= LIVE_MAX_ICE_SERVERS) {
                return -1;
            }
            options->stun_servers[options->stun_server_count++] = argv[++i];
        } else if (strcmp(arg, "--turn-env") == 0 && i + 1 < argc) {
            const char *env_name;
            const char *turn_url;
            if (options->turn_server_count >= LIVE_MAX_ICE_SERVERS) {
                return -1;
            }
            env_name = argv[++i];
            turn_url = getenv(env_name);
            if (!turn_url || !turn_url[0]) {
                fprintf(stderr, "TURN environment variable is empty: %s\n",
                        env_name);
                return -1;
            }
            options->turn_servers[options->turn_server_count++] = turn_url;
        } else if (strcmp(arg, "--allow-loopback") == 0) {
            options->allow_loopback = 1;
        } else if (strcmp(arg, "--allow-plaintext-loopback") == 0) {
            options->allow_plaintext_loopback = 1;
        } else {
            return -1;
        }
    }

    if (!options->base_url || !options->base_url[0] ||
        !options->path || options->path[0] != '/') {
        return -1;
    }
    if (options->mode == LIVE_MODE_PUBLISH &&
        options->playback_device_index >= 0) {
        return -1;
    }
    if (options->mode == LIVE_MODE_SUBSCRIBE &&
        options->capture_device_id) {
        return -1;
    }
    return 0;
}

static int live_list_devices(void) {
    salts_capture_device_t capture_devices[SALTS_CAPTURE_MAX_DEVICES];
    salts_playback_device_t playback_devices[SALTS_PLAYBACK_MAX_DEVICES];
    size_t playback_count = 0u;
    int capture_count;
    int status;
    int i;

    memset(capture_devices, 0, sizeof(capture_devices));
    capture_count = salts_capture_list_audio_devices(
        capture_devices, SALTS_CAPTURE_MAX_DEVICES);
    if (capture_count < 0) {
        fprintf(stderr, "audio capture enumeration failed: %d\n", capture_count);
        return 1;
    }

    printf("Capture devices:\n");
    if (capture_count == 0) {
        printf("  (none)\n");
    }
    for (i = 0; i < capture_count; ++i) {
        printf("  [%d] %s%s  id=%s\n",
               capture_devices[i].index,
               capture_devices[i].name,
               capture_devices[i].is_default ? " (default)" : "",
               capture_devices[i].id);
    }

    memset(playback_devices, 0, sizeof(playback_devices));
    status = salts_playback_list_devices(
        playback_devices, SALTS_PLAYBACK_MAX_DEVICES, &playback_count);
    if (status != SALTS_PLAYBACK_OK) {
        fprintf(stderr, "audio playback enumeration failed: %d\n", status);
        return 1;
    }

    printf("Playback devices:\n");
    if (playback_count == 0u) {
        printf("  (none)\n");
    }
    for (i = 0; i < (int)playback_count; ++i) {
        printf("  [%u] %s%s\n",
               (unsigned)playback_devices[i].index,
               playback_devices[i].name,
               playback_devices[i].is_default ? " (default)" : "");
    }
    return 0;
}

static int live_resolve_playback_device(
    int requested_index, salts_playback_device_t *out_device) {
    salts_playback_device_t devices[SALTS_PLAYBACK_MAX_DEVICES];
    size_t count = 0u;
    size_t i;
    int status;

    if (!out_device || requested_index < 0) {
        return -1;
    }
    memset(devices, 0, sizeof(devices));
    status = salts_playback_list_devices(
        devices, SALTS_PLAYBACK_MAX_DEVICES, &count);
    if (status != SALTS_PLAYBACK_OK) {
        return -1;
    }
    for (i = 0u; i < count; ++i) {
        if (devices[i].index == (uint32_t)requested_index) {
            *out_device = devices[i];
            return 0;
        }
    }
    return -1;
}

static const char *live_token(const live_options_t *options) {
    const char *value;
    if (!options || !options->token_env || !options->token_env[0]) {
        return NULL;
    }
    value = getenv(options->token_env);
    return value && value[0] ? value : NULL;
}

static int live_publisher_poll_transport(turbo_rtc_client_t *client) {
    turbo_rtc_client_status_t status = turbo_rtc_client_poll(client);
    if (status == TURBO_RTC_CLIENT_OK) {
        return 0;
    }
    if (status == TURBO_RTC_CLIENT_EPEER ||
        status == TURBO_RTC_CLIENT_ETIMEDOUT) {
        fprintf(stderr, "publisher transport failed (%d), restarting ICE\n",
                (int)status);
        status = turbo_rtc_client_restart_ice(client);
        if (status == TURBO_RTC_CLIENT_OK) {
            return 0;
        }
        fprintf(stderr, "publisher ICE restart failed: %d\n", (int)status);
        return -1;
    }
    fprintf(stderr, "publisher poll failed: %d\n", (int)status);
    return -1;
}

static int live_subscriber_poll_transport(turbo_rtc_subscriber_t *subscriber) {
    turbo_rtc_client_status_t status = turbo_rtc_subscriber_poll(subscriber);
    if (status == TURBO_RTC_CLIENT_OK) {
        return 0;
    }
    if (status == TURBO_RTC_CLIENT_EPEER ||
        status == TURBO_RTC_CLIENT_ETIMEDOUT) {
        fprintf(stderr, "subscriber transport failed (%d), restarting ICE\n",
                (int)status);
        status = turbo_rtc_subscriber_restart_ice(subscriber);
        if (status == TURBO_RTC_CLIENT_OK) {
            return 0;
        }
        fprintf(stderr, "subscriber ICE restart failed: %d\n", (int)status);
        return -1;
    }
    fprintf(stderr, "subscriber poll failed: %d\n", (int)status);
    return -1;
}

static int live_run_publish(const live_options_t *options) {
    turbo_rtc_client_config_t client_config;
    turbo_rtc_capture_source_config_t capture_config;
    turbo_rtc_client_snapshot_t client_snapshot;
    turbo_rtc_client_t *client = NULL;
    turbo_rtc_capture_source_t *capture = NULL;
    turbo_rtc_client_status_t status;
    const char *token = live_token(options);
    int client_prepared = 0;
    int capture_start_attempted = 0;
    int capture_started = 0;
    int result = 1;

    turbo_rtc_client_config_init(&client_config);
    client_config.whip_base_url = options->base_url;
    client_config.whip_path = options->path;
    client_config.bearer_token = token;
    client_config.ca_file = options->ca_file;
    client_config.server_name = options->server_name;
    client_config.allow_plaintext_loopback = options->allow_plaintext_loopback;
    client_config.allow_loopback = options->allow_loopback;
    client_config.stun_servers = options->stun_servers;
    client_config.stun_server_count = options->stun_server_count;
    client_config.turn_servers = options->turn_servers;
    client_config.turn_server_count = options->turn_server_count;

    turbo_rtc_capture_source_config_init(&capture_config);
    capture_config.device_id = options->capture_device_id;
    capture_config.sample_rate = client_config.sample_rate;
    capture_config.channels = client_config.channels;
    capture_config.frame_size_ms = client_config.frame_size_ms;

    status = turbo_rtc_capture_source_create(&capture_config, &capture);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "capture source create failed: %d\n", (int)status);
        goto cleanup;
    }
    status = turbo_rtc_capture_source_prepare(capture);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "capture source prepare failed: %d\n", (int)status);
        goto cleanup;
    }

    status = turbo_rtc_client_create(&client_config, &client);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "publisher create failed: %d\n", (int)status);
        goto cleanup;
    }
    status = turbo_rtc_client_prepare(client);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "publisher prepare failed: %d\n", (int)status);
        goto cleanup;
    }
    client_prepared = 1;

    status = turbo_rtc_client_start(client);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "WHIP start failed: %d\n", (int)status);
        goto cleanup;
    }

    fprintf(stderr, "publisher started; waiting for ICE/DTLS/SRTP\n");

    while (g_running) {
        if (live_publisher_poll_transport(client) != 0) {
            goto cleanup;
        }

        turbo_rtc_client_snapshot_init(&client_snapshot);
        status = turbo_rtc_client_snapshot(client, &client_snapshot);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "publisher snapshot failed: %d\n", (int)status);
            goto cleanup;
        }

        if (client_snapshot.connected && !capture_started) {
            capture_start_attempted = 1;
            status = turbo_rtc_capture_source_start(capture);
            if (status != TURBO_RTC_CLIENT_OK) {
                fprintf(stderr, "capture source start failed: %d\n", (int)status);
                goto cleanup;
            }
            capture_started = 1;
            fprintf(stderr, "publisher connected; capture is live\n");
        }

        if (capture_started) {
            status = turbo_rtc_capture_source_poll(capture, client);
            if (status != TURBO_RTC_CLIENT_OK &&
                status != TURBO_RTC_CLIENT_ESTATE) {
                fprintf(stderr, "capture source poll failed: %d\n", (int)status);
                goto cleanup;
            }
        }
        cmeta_sleep_ms(LIVE_POLL_SLEEP_MS);
    }

    result = 0;

cleanup:
    if (capture_started || capture_start_attempted) {
        turbo_rtc_capture_source_snapshot_t capture_snapshot;
        turbo_rtc_capture_source_snapshot_init(&capture_snapshot);
        if (turbo_rtc_capture_source_snapshot(capture, &capture_snapshot) ==
                TURBO_RTC_CLIENT_OK &&
            (capture_snapshot.state == TURBO_RTC_CAPTURE_SOURCE_STARTED ||
             capture_snapshot.state == TURBO_RTC_CAPTURE_SOURCE_FAILED)) {
            status = turbo_rtc_capture_source_stop(capture);
            if (status != TURBO_RTC_CLIENT_OK) {
                fprintf(stderr, "capture source stop failed: %d\n", (int)status);
                result = 1;
            }
        }
    }
    if (client_prepared) {
        status = turbo_rtc_client_stop(client);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "publisher stop failed: %d\n", (int)status);
            result = 1;
        }
    }
    if (capture) {
        status = turbo_rtc_capture_source_destroy(capture);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "capture source destroy failed: %d\n", (int)status);
            result = 1;
        }
    }
    if (client) {
        status = turbo_rtc_client_destroy(client);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "publisher destroy failed: %d\n", (int)status);
            result = 1;
        }
    }
    return result;
}

static int live_run_subscribe(const live_options_t *options) {
    turbo_rtc_subscriber_config_t subscriber_config;
    turbo_rtc_playback_sink_config_t playback_config;
    turbo_rtc_subscriber_t *subscriber = NULL;
    turbo_rtc_playback_sink_t *playback = NULL;
    salts_playback_device_t playback_device;
    turbo_rtc_client_status_t status;
    const char *token = live_token(options);
    int subscriber_prepared = 0;
    int playback_started = 0;
    int result = 1;

    turbo_rtc_playback_sink_config_init(&playback_config);
    if (options->playback_device_index >= 0) {
        if (live_resolve_playback_device(
                options->playback_device_index, &playback_device) != 0) {
            fprintf(stderr, "playback device index not found: %d\n",
                    options->playback_device_index);
            return 1;
        }
        playback_config.device = &playback_device;
    }

    status = turbo_rtc_playback_sink_create(&playback_config, &playback);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "playback sink create failed: %d\n", (int)status);
        goto cleanup;
    }
    status = turbo_rtc_playback_sink_prepare(playback);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "playback sink prepare failed: %d\n", (int)status);
        goto cleanup;
    }
    status = turbo_rtc_playback_sink_start(playback);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "playback sink start failed: %d\n", (int)status);
        goto cleanup;
    }
    playback_started = 1;

    turbo_rtc_subscriber_config_init(&subscriber_config);
    subscriber_config.whep_base_url = options->base_url;
    subscriber_config.whep_path = options->path;
    subscriber_config.bearer_token = token;
    subscriber_config.ca_file = options->ca_file;
    subscriber_config.server_name = options->server_name;
    subscriber_config.allow_plaintext_loopback =
        options->allow_plaintext_loopback;
    subscriber_config.allow_loopback = options->allow_loopback;
    subscriber_config.stun_servers = options->stun_servers;
    subscriber_config.stun_server_count = options->stun_server_count;
    subscriber_config.turn_servers = options->turn_servers;
    subscriber_config.turn_server_count = options->turn_server_count;
    subscriber_config.sample_rate = playback_config.sample_rate;
    subscriber_config.channels = playback_config.channels;
    subscriber_config.on_audio = turbo_rtc_playback_sink_on_audio;
    subscriber_config.audio_context = playback;

    status = turbo_rtc_subscriber_create(&subscriber_config, &subscriber);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "subscriber create failed: %d\n", (int)status);
        goto cleanup;
    }
    status = turbo_rtc_subscriber_prepare(subscriber);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "subscriber prepare failed: %d\n", (int)status);
        goto cleanup;
    }
    subscriber_prepared = 1;

    status = turbo_rtc_subscriber_start(subscriber);
    if (status != TURBO_RTC_CLIENT_OK) {
        fprintf(stderr, "WHEP start failed: %d\n", (int)status);
        goto cleanup;
    }

    fprintf(stderr, "subscriber started; playback owner loop is live\n");

    while (g_running) {
        if (live_subscriber_poll_transport(subscriber) != 0) {
            goto cleanup;
        }
        cmeta_sleep_ms(LIVE_POLL_SLEEP_MS);
    }

    result = 0;

cleanup:
    if (subscriber_prepared) {
        status = turbo_rtc_subscriber_stop(subscriber);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "subscriber stop failed: %d\n", (int)status);
            result = 1;
        }
    }
    if (playback_started) {
        status = turbo_rtc_playback_sink_stop(playback);
        if (status != TURBO_RTC_CLIENT_OK &&
            status != TURBO_RTC_CLIENT_ETIMEDOUT) {
            fprintf(stderr, "playback sink stop failed: %d\n", (int)status);
            result = 1;
        } else if (status == TURBO_RTC_CLIENT_ETIMEDOUT) {
            fprintf(stderr, "playback drain timed out after local stop/clear\n");
        }
    }
    if (subscriber) {
        status = turbo_rtc_subscriber_destroy(subscriber);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "subscriber destroy failed: %d\n", (int)status);
            result = 1;
        }
    }
    if (playback) {
        status = turbo_rtc_playback_sink_destroy(playback);
        if (status != TURBO_RTC_CLIENT_OK) {
            fprintf(stderr, "playback sink destroy failed: %d\n", (int)status);
            result = 1;
        }
    }
    return result;
}

int main(int argc, char **argv) {
    live_options_t options;

    if (live_parse_options(argc, argv, &options) != 0) {
        live_print_usage(argv[0]);
        return 2;
    }
    if (options.mode == LIVE_MODE_LIST) {
        return live_list_devices();
    }

    signal(SIGINT, live_signal_handler);
    signal(SIGTERM, live_signal_handler);

    if (options.mode == LIVE_MODE_PUBLISH) {
        return live_run_publish(&options);
    }
    return live_run_subscribe(&options);
}
