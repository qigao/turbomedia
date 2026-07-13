#include "rtc_workflow_adapter.h"
#include "sfu_node/server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct rtc_workflow_context_s {
    sfu_node_app_server_t *server;
    void *session;
    turbo_rtc_session_workflow_t *workflow;
};

void on_rtc_command(void *user_data, const char *target,
                    const char *event,
                    const turbo_rtc_workflow_param_t *params,
                    int param_count) {
    rtc_workflow_context_t *ctx = (rtc_workflow_context_t *)user_data;
    if (!ctx || !ctx->session) return;
    // sfu_node_webrtc_session_t is opaque here if we don't have its full definition,
    // but we can pass it as void* if needed. For now just print.
    // sfu_node_webrtc_session_t *session = (sfu_node_webrtc_session_t *)ctx->session;

    if (strcmp(target, "sfu-node") == 0) {
        if (strcmp(event, "rtc.command.create_answer") == 0) {
            printf("[SFUWorkflow] Creating answer\n");
            // turbo_sfu_node_create_answer(session);
        } else if (strcmp(event, "rtc.command.add_ice_candidate") == 0) {
            for (int i = 0; i < param_count; i++) {
                if (strcmp(params[i].name, "candidate") == 0) {
                    printf("[SFUWorkflow] Adding ICE candidate\n");
                    break;
                }
            }
        }
    }
}

int sfu_node_workflow_init(sfu_node_app_server_t *server, void *session_user_data, rtc_workflow_context_t **out_ctx) {
    rtc_workflow_context_t *ctx;
    
    if (!server || !session_user_data || !out_ctx) return -1;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    
    ctx->server = server;
    ctx->session = session_user_data;
    ctx->workflow = turbo_rtc_session_workflow_create();
    
    if (!ctx->workflow) {
        free(ctx);
        return -1;
    }
    
    turbo_rtc_session_workflow_set_command_callback(ctx->workflow, on_rtc_command, ctx);
    
    const char *wf_path = "media/workflows/rtc_session.scxml";
    if (turbo_rtc_session_workflow_load_file(ctx->workflow, wf_path) != 0) {
        turbo_rtc_session_workflow_destroy(ctx->workflow);
        free(ctx);
        return -1;
    }
    
    *out_ctx = ctx;
    return 0;
}

void sfu_node_workflow_destroy(rtc_workflow_context_t *ctx) {
    if (!ctx) return;
    if (ctx->workflow) {
        turbo_rtc_session_workflow_destroy(ctx->workflow);
    }
    free(ctx);
}

int sfu_node_workflow_receive(rtc_workflow_context_t *ctx, const char *event, 
                               const turbo_rtc_workflow_param_t *params, int param_count) {
    int res;
    if (!ctx || !ctx->workflow) return -1;
    
    res = turbo_rtc_session_workflow_receive(ctx->workflow, event, params, param_count);
    turbo_rtc_session_workflow_drain(ctx->workflow, 16);
    return res;
}
