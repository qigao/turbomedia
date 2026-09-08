/**
 * @file http_api.h
 * @brief Room service HTTP JSON API
 */
#ifndef TURBO_ROOM_SERVICE_APP_HTTP_API_H
#define TURBO_ROOM_SERVICE_APP_HTTP_API_H

#include "room_service/server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct room_service_http_api_s room_service_http_api_t;

room_service_http_api_t *room_service_http_api_create(room_service_app_server_t *server);
int room_service_http_api_start(room_service_http_api_t *api, const char *host, int port);
int room_service_http_api_stop(room_service_http_api_t *api);
int room_service_http_api_destroy(room_service_http_api_t *api);
char *room_service_http_api_build_room_diagnostic(room_service_app_server_t *server,
                                                  const char *room_id);
char *room_service_http_api_build_room_sync_diagnostic(room_service_app_server_t *server,
                                                       const char *room_id);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ROOM_SERVICE_APP_HTTP_API_H */
