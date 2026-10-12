/* Exercise an independently installed TurboMedia::RTSP runtime, not a
 * build-tree target. Real localhost TCP, Salts CNet NativeIO, repeated
 * stop/start and callback quiescence are required on each desktop host.
 *
 * This is an RTSP SDK lifecycle preflight. It does NOT validate the
 * separate FFmpeg Provider DSO N -> N+1 Generation unload barrier.
 */
#include <turbo_rtsp.h>

#include <stdio.h>
#include <string.h>

enum {
    TM_INSTALLED_RTSP_PORT = 21564,
    TM_INSTALLED_RTSP_TIMEOUT_MS = 5000,
    TM_INSTALLED_RTSP_ROUNDS = 3
};

typedef struct installed_rtsp_fixture {
    /* Worker-only writes; main reads only after server_stop joins Worker. */
    unsigned options;
    unsigned closes;
} installed_rtsp_fixture;

static int installed_options(turbo_rtsp_session_t *session,
                             const turbo_rtsp_request_t *request,
                             turbo_rtsp_response_t *response, void *user) {
    installed_rtsp_fixture *fixture = (installed_rtsp_fixture *)user;
    (void)session;
    (void)request;
    ++fixture->options;
    return turbo_rtsp_response_options(response, NULL);
}

static void installed_close(turbo_rtsp_session_t *session, void *user) {
    installed_rtsp_fixture *fixture = (installed_rtsp_fixture *)user;
    (void)session;
    ++fixture->closes;
}

int main(void) {
    static const char uri[] = "rtsp://127.0.0.1:21564/installed";
    turbo_rtsp_server_config_t server_config;
    turbo_rtsp_server_handlers_t handlers;
    turbo_rtsp_client_config_t client_config;
    installed_rtsp_fixture fixture = {0};
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client = NULL;
    unsigned round;
    int server_started = 0;
    int status = 0;

    memset(&server_config, 0, sizeof(server_config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&client_config, 0, sizeof(client_config));
    server_config.bind_host = "127.0.0.1";
    server_config.port = TM_INSTALLED_RTSP_PORT;
    server_config.client_timeout_ms = TM_INSTALLED_RTSP_TIMEOUT_MS;
    server_config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    server_config.connection_capacity = 4u;
    server_config.send_queue_capacity = 16u;
    server_config.send_queue_bytes = 512u * 1024u;
    handlers.on_options = installed_options;
    handlers.on_session_close = installed_close;

    client_config.host = "127.0.0.1";
    client_config.port = TM_INSTALLED_RTSP_PORT;
    client_config.timeout_ms = TM_INSTALLED_RTSP_TIMEOUT_MS;
    client_config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_TCP;

    server = turbo_rtsp_server_create(&server_config, &handlers, &fixture);
    if (!server) {
        fprintf(stderr, "Installed RTSP SDK: server creation failed\n");
        return 1;
    }

    for (round = 0u; round < TM_INSTALLED_RTSP_ROUNDS; ++round) {
        const turbo_rtsp_client_response_t *response;
        if (turbo_rtsp_server_start(server) != 0) {
            status = 2;
            break;
        }
        server_started = 1;
        client = turbo_rtsp_client_create(&client_config);
        if (!client || turbo_rtsp_client_connect(client) != 0) {
            status = 3;
            break;
        }
        if (turbo_rtsp_client_options(client, uri) != 0) {
            status = 4;
            break;
        }
        response = turbo_rtsp_client_get_last_response(client);
        if (!response || response->status_code != 200) {
            status = 5;
            break;
        }

        /* Stop while one actual accepted CNet TCP session still exists.
         * The stop barrier must join the original server Worker and its
         * callbacks before the test reads fixture state or restarts it. */
        turbo_rtsp_server_stop(server);
        server_started = 0;
        if (fixture.options != round + 1u) {
            status = 6;
            break;
        }
        turbo_rtsp_client_close(client);
        turbo_rtsp_client_destroy(client);
        client = NULL;
    }
    if (client) {
        turbo_rtsp_client_close(client);
        turbo_rtsp_client_destroy(client);
    }
    if (server_started) turbo_rtsp_server_stop(server);
    turbo_rtsp_server_destroy(server);

    if (status) {
        fprintf(stderr, "Installed RTSP SDK: lifecycle failed at stage %d, round %u\n",
                status, round);
    }
    return status;
}
