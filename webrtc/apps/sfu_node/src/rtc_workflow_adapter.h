#ifndef SFU_NODE_RTC_WORKFLOW_ADAPTER_H
#define SFU_NODE_RTC_WORKFLOW_ADAPTER_H

#include "sfu_node/server.h"
#include "turbo_rtc_session_workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtc_workflow_context_s rtc_workflow_context_t;

int sfu_node_workflow_init(sfu_node_app_server_t *server, void *session_user_data, rtc_workflow_context_t **out_ctx);
void sfu_node_workflow_destroy(rtc_workflow_context_t *ctx);
int sfu_node_workflow_receive(rtc_workflow_context_t *ctx, const char *event, 
                               const turbo_rtc_workflow_param_t *params, int param_count);

#ifdef __cplusplus
}
#endif

#endif /* SFU_NODE_RTC_WORKFLOW_ADAPTER_H */
