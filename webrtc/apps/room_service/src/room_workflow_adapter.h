#ifndef ROOM_SERVICE_ROOM_WORKFLOW_ADAPTER_H
#define ROOM_SERVICE_ROOM_WORKFLOW_ADAPTER_H

#include "room_service/server.h"
#include "turbo_rtc_session_workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct room_workflow_context_s room_workflow_context_t;

int room_workflow_init(room_service_app_server_t *server, const char *room_id,
                       const char *workflow_path,
                       room_workflow_context_t **out_ctx);
void room_workflow_destroy(room_workflow_context_t *ctx);
int room_workflow_receive(room_workflow_context_t *ctx, const char *event, 
                           const turbo_rtc_workflow_param_t *params, int param_count);

#ifdef __cplusplus
}
#endif

#endif /* ROOM_SERVICE_ROOM_WORKFLOW_ADAPTER_H */
