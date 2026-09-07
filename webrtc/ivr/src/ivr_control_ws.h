#ifndef TURBO_MEDIA_IVR_CONTROL_WS_H
#define TURBO_MEDIA_IVR_CONTROL_WS_H

#include "ivr/ivr_worker.h"

#include <chttp/chttp.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_CONTROL_WS_SUBPROTOCOL "turbomedia.control.v1.bin"
#define IVR_CONTROL_WS_IDENTITY_HEADER "X-TurboMedia-Identity"

typedef struct ivr_control_ws_client_s ivr_control_ws_client_t;
typedef struct ivr_control_ws_server_s ivr_control_ws_server_t;

typedef struct ivr_control_ws_route_s {
    chttp_server_websocket_session session;
} ivr_control_ws_route_t;

typedef void (*ivr_control_ws_client_message_fn)(void *context,
                                                  const uint8_t *data,
                                                  size_t size);
typedef void (*ivr_control_ws_client_connection_fn)(void *context,
                                                     int connected);

typedef int (*ivr_control_ws_verify_identity_fn)(
    void *context, const char *certificate_sha256,
    const char *claimed_identity);
typedef void (*ivr_control_ws_peer_fn)(void *context,
                                       const ivr_control_ws_route_t *route,
                                       const char *identity, int connected);
typedef int (*ivr_control_ws_server_message_fn)(
    void *context, const ivr_control_ws_route_t *route, const char *identity,
    const uint8_t *data, size_t size);

typedef struct ivr_control_ws_client_config_s {
    const char *uri;
    const char *identity;
    const cnet_tls_client_config *tls;
    size_t maximum_queue_messages;
    size_t maximum_queue_bytes;
    size_t maximum_message_bytes;
    uint32_t start_timeout_ms;
    uint32_t io_timeout_ms;
    uint32_t reconnect_initial_ms;
    uint32_t reconnect_max_ms;
    ivr_control_ws_client_message_fn on_message;
    ivr_control_ws_client_connection_fn on_connection;
    void *callback_context;
} ivr_control_ws_client_config_t;

typedef struct ivr_control_ws_server_config_s {
    const char *host;
    uint16_t port;
    const char *path;
    const cnet_tls_server_config *tls;
    size_t maximum_connections;
    size_t maximum_message_bytes;
    uint32_t shutdown_timeout_ms;
    ivr_control_ws_verify_identity_fn verify_identity;
    void *verify_identity_context;
    ivr_control_ws_peer_fn on_peer;
    ivr_control_ws_server_message_fn on_message;
    void *callback_context;
} ivr_control_ws_server_config_t;

void ivr_control_ws_client_config_init(
    ivr_control_ws_client_config_t *config);
ivr_status_t ivr_control_ws_client_create(
    const ivr_control_ws_client_config_t *config,
    ivr_control_ws_client_t **out_client);
ivr_status_t ivr_control_ws_client_start(ivr_control_ws_client_t *client);
ivr_status_t ivr_control_ws_client_send_copy(ivr_control_ws_client_t *client,
                                              const uint8_t *data,
                                              size_t size);
void ivr_control_ws_client_stop(ivr_control_ws_client_t *client);
void ivr_control_ws_client_destroy(ivr_control_ws_client_t *client);
int ivr_control_ws_client_running(const ivr_control_ws_client_t *client);

void ivr_control_ws_server_config_init(
    ivr_control_ws_server_config_t *config);
ivr_status_t ivr_control_ws_server_create(
    const ivr_control_ws_server_config_t *config,
    ivr_control_ws_server_t **out_server);
ivr_status_t ivr_control_ws_server_start(ivr_control_ws_server_t *server);
/* Owner-thread maintenance: restarts a terminal CHTTP listener with bounded
   backoff. `out_restarted` is set only after a new listener is accepting. */
ivr_status_t ivr_control_ws_server_maintain(
    ivr_control_ws_server_t *server, int *out_restarted);
void ivr_control_ws_server_stop(ivr_control_ws_server_t *server);
void ivr_control_ws_server_destroy(ivr_control_ws_server_t *server);
ivr_status_t ivr_control_ws_server_port(const ivr_control_ws_server_t *server,
                                         uint16_t *out_port);
ivr_status_t ivr_control_ws_server_send_copy(
    ivr_control_ws_server_t *server, const ivr_control_ws_route_t *route,
    const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_CONTROL_WS_H */
