#include "room_workflow_adapter.h"
#include "room_service/server.h"
#include "turbo_rtc_session_workflow.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct room_workflow_context_s {
    room_service_app_server_t *server;
    turbo_rtc_session_workflow_t *workflow;
    char room_id[TURBO_ROOM_ID_MAX];
};

/* Subscribe all tracks owned by `sender_id` to `receiver_id`. */
static void apply_one_direction(room_workflow_context_t *ctx,
                                const turbo_room_summary_t *room_summary,
                                const char *sender_id,
                                const char *receiver_id) {
    int i;
    for (i = 0; i < room_summary->published_track_count; i++) {
        turbo_room_track_summary_t track;
        if (turbo_room_service_get_track_summary_at(
                room_service_app_server_get_service(ctx->server),
                ctx->room_id, i, &track) != 0) {
            continue;
        }
        if (strcmp(track.owner_participant_id, sender_id) != 0) {
            continue;
        }

        turbo_room_subscription_summary_t sub;
        memset(&sub, 0, sizeof(sub));
        strncpy(sub.subscriber_participant_id, receiver_id,
                sizeof(sub.subscriber_participant_id) - 1);
        strncpy(sub.track_id, track.track_id, sizeof(sub.track_id) - 1);
        sub.enabled = 1;
        sub.muted = 0;
        sub.preferred_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        sub.target_layer   = TURBO_ROOM_VIDEO_LAYER_HIGH;
        strncpy(sub.policy_source, "scxml", sizeof(sub.policy_source) - 1);

        room_service_app_server_sync_apply_track_subscription(
            ctx->server, ctx->room_id, &sub);
    }
}

static void on_room_command(void *user_data, const char *target,
                            const char *event,
                            const turbo_rtc_workflow_param_t *params,
                            int param_count) {
    room_workflow_context_t *ctx = (room_workflow_context_t *)user_data;
    if (!ctx || !ctx->server) return;

    if (strcmp(target, "room-service") != 0) return;

    if (strcmp(event, "rtc.command.apply_conference_policy") == 0) {
        (void)room_service_app_server_apply_conference_policy(
            ctx->server, ctx->room_id, NULL);
        return;
    }

    if (strcmp(event, "rtc.command.unsubscribe") == 0) {
        const char *subscriber = NULL;
        const char *tgt        = NULL;
        int i;
        for (i = 0; i < param_count; i++) {
            if (strcmp(params[i].name, "subscriber") == 0) subscriber = params[i].value;
            if (strcmp(params[i].name, "target")     == 0) tgt        = params[i].value;
        }
        if (!subscriber || !tgt) return;

        /* Disable all subscriptions from `subscriber` pointing at tracks owned
         * by `tgt` by setting enabled=0 via the existing SFU control path. */
        turbo_room_summary_t room_summary;
        if (turbo_room_service_get_room_summary(
                room_service_app_server_get_service(ctx->server),
                ctx->room_id, &room_summary) != 0) {
            return;
        }
        int i2;
        for (i2 = 0; i2 < room_summary.published_track_count; i2++) {
            turbo_room_track_summary_t track;
            if (turbo_room_service_get_track_summary_at(
                    room_service_app_server_get_service(ctx->server),
                    ctx->room_id, i2, &track) != 0) {
                continue;
            }
            if (strcmp(track.owner_participant_id, tgt) != 0) continue;

            turbo_room_subscription_summary_t sub;
            memset(&sub, 0, sizeof(sub));
            strncpy(sub.subscriber_participant_id, subscriber,
                    sizeof(sub.subscriber_participant_id) - 1);
            strncpy(sub.track_id, track.track_id, sizeof(sub.track_id) - 1);
            sub.enabled = 0;
            sub.muted   = 1;
            strncpy(sub.policy_source, "scxml", sizeof(sub.policy_source) - 1);
            room_service_app_server_sync_apply_track_subscription(
                ctx->server, ctx->room_id, &sub);
        }
        return;
    }

    if (strcmp(event, "rtc.command.connect_media") == 0) {
        const char *from = NULL;
        const char *to   = NULL;
        int i;
        for (i = 0; i < param_count; i++) {
            if (strcmp(params[i].name, "from") == 0) from = params[i].value;
            if (strcmp(params[i].name, "to")   == 0) to   = params[i].value;
        }
        if (!from || !to) return;

        printf("[RoomWorkflow] Connecting media: %s <-> %s\n", from, to);

        /* Note: on_room_command is invoked from room_workflow_receive, which
         * is called *outside* the server mutex (see server.c). It is safe to
         * call sync_apply_track_subscription here. */
        turbo_room_summary_t room_summary;
        if (turbo_room_service_get_room_summary(
                room_service_app_server_get_service(ctx->server),
                ctx->room_id, &room_summary) != 0) {
            return;
        }

        /* Establish bidirectional media: from->to and to->from. */
        apply_one_direction(ctx, &room_summary, from, to);
        apply_one_direction(ctx, &room_summary, to, from);
    }
}

int room_workflow_init(room_service_app_server_t *server, const char *room_id,
                       const char *workflow_path,
                       room_workflow_context_t **out_ctx) {
    room_workflow_context_t *ctx;
    turbo_rtc_workflow_param_t join_params[1];

    if (!server || !room_id || !workflow_path || !out_ctx) return -1;

    ctx = (room_workflow_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "[RoomWorkflow] calloc failed\n");
        return -1;
    }

    ctx->server = server;
    strncpy(ctx->room_id, room_id, sizeof(ctx->room_id) - 1);

    ctx->workflow = turbo_rtc_session_workflow_create();
    if (!ctx->workflow) {
        fprintf(stderr, "[RoomWorkflow] Failed to create workflow engine\n");
        free(ctx);
        return -1;
    }

    turbo_rtc_session_workflow_set_command_callback(ctx->workflow, on_room_command, ctx);

    if (turbo_rtc_session_workflow_load_file(ctx->workflow, workflow_path) != 0) {
        fprintf(stderr, "[RoomWorkflow] Failed to load workflow: %s\n", workflow_path);
        turbo_rtc_session_workflow_destroy(ctx->workflow);
        free(ctx);
        return -1;
    }

    join_params[0].name  = "room_id";
    join_params[0].value = room_id;
    turbo_rtc_session_workflow_receive(ctx->workflow, "room.join", join_params, 1);
    turbo_rtc_session_workflow_drain(ctx->workflow, 16);

    *out_ctx = ctx;
    return 0;
}

void room_workflow_destroy(room_workflow_context_t *ctx) {
    if (!ctx) return;
    turbo_rtc_session_workflow_receive(ctx->workflow, "room.leave", NULL, 0);
    turbo_rtc_session_workflow_drain(ctx->workflow, 16);
    turbo_rtc_session_workflow_destroy(ctx->workflow);
    free(ctx);
}

int room_workflow_receive(room_workflow_context_t *ctx, const char *event,
                          const turbo_rtc_workflow_param_t *params, int param_count) {
    int res;
    if (!ctx) return -1;
    res = turbo_rtc_session_workflow_receive(ctx->workflow, event, params, param_count);
    turbo_rtc_session_workflow_drain(ctx->workflow, 16);
    return res;
}
