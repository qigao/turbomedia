/**
 * @file http_api.h
 * @brief SFU node HTTP JSON API
 */
#ifndef TURBO_SFU_NODE_APP_HTTP_API_H
#define TURBO_SFU_NODE_APP_HTTP_API_H

#include "sfu_node/server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sfu_node_http_api_s sfu_node_http_api_t;

sfu_node_http_api_t *sfu_node_http_api_create(sfu_node_app_server_t *server);
int sfu_node_http_api_start(sfu_node_http_api_t *api, const char *host, int port);
void sfu_node_http_api_stop(sfu_node_http_api_t *api);
void sfu_node_http_api_destroy(sfu_node_http_api_t *api);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SFU_NODE_APP_HTTP_API_H */
