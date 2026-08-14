#ifndef TURBO_MEDIA_IVR_WORKER_HTTP_H
#define TURBO_MEDIA_IVR_WORKER_HTTP_H

#include "ivr_worker_health.h"
#include "ivr_worker_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_worker_http_s ivr_worker_http_t;
typedef int (*ivr_worker_http_drain_fn)(void *context);

/* The management listener is intentionally loopback-only. Health is borrowed
   and must outlive the server. */
int ivr_worker_http_create(ivr_worker_health_t *health,
                           ivr_worker_http_t **out_server);
int ivr_worker_http_set_metrics(ivr_worker_http_t *server,
                                ivr_worker_metrics_t *metrics);
/* Installs the loopback management drain request callback before start. The
   callback must only signal the process owner; it must not stop this HTTP
   server reentrantly from its request thread. */
int ivr_worker_http_set_drain_handler(ivr_worker_http_t *server,
                                      ivr_worker_http_drain_fn callback,
                                      void *context);
int ivr_worker_http_start(ivr_worker_http_t *server, const char *host,
                          int port);
void ivr_worker_http_stop(ivr_worker_http_t *server);
void ivr_worker_http_destroy(ivr_worker_http_t *server);

#ifdef __cplusplus
}
#endif

#endif
