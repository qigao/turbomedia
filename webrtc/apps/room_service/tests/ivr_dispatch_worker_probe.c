/* Controlled FlowMQ worker used only by the three-process failure tests. */
#include "ivr_flowmq_gateway.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "turbomedia_ivr_v1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PROBE_DROP_BEFORE_ACK = 0,
    PROBE_ACK_AND_EXIT,
    PROBE_ACK_AND_STAY
} probe_mode_t;

static DataBind *g_codec;
static ivr_flowmq_gateway_t *g_gateway;
static probe_mode_t g_mode;

static void probe_exit(void) {
    fflush(stdout);
    fflush(stderr);
    _Exit(0);
}

static void on_reply(void *context, const uint8_t *frame, size_t length) {
    ivr_frame_info_t info;
    (void)context;
    if (ivr_frame_decode(frame, length, &info) != IVR_OK) {
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_WORKER_SYNC_RESULT_V1) {
        DataBindError error = DATA_BIND_ERROR_INIT;
        DataBindObject *object = NULL;
        int status_code = IVR_ESTATE;
        if (length >= IVR_FRAME_HEADER_SIZE &&
            data_bind_object_from_bin(
                g_codec, "WorkerSyncResultV1",
                frame + IVR_FRAME_HEADER_SIZE,
                length - IVR_FRAME_HEADER_SIZE, &object,
                &error) == DATA_BIND_OK) {
            const DataBindValue *root = data_bind_object_value(object);
            const DataBindValue *status =
                data_bind_value_get(root, "status_code");
            if (status) {
                status_code = data_bind_value_as_int(status);
            }
            data_bind_object_free(object);
        }
        if (status_code == IVR_OK) {
            printf("probe sync acknowledged\n");
        } else {
            printf("probe sync rejected status=%d\n", status_code);
        }
        fflush(stdout);
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V2) {
        ivr_call_dispatch_t dispatch;
        if (ivr_flowmq_gateway_decode_dispatch(g_codec, frame, length,
                                               &dispatch) != IVR_OK) {
            return;
        }
        printf("probe dispatch received %s/%s\n", dispatch.room_id,
               dispatch.call_id);
        fflush(stdout);
        if (g_mode == PROBE_DROP_BEFORE_ACK) {
            probe_exit();
        }
        if (ivr_flowmq_gateway_send_dispatch_result_v2(
                g_gateway, &dispatch, IVR_OK, 1u, 1u, "", "") != IVR_OK) {
            fprintf(stderr, "probe dispatch ACK send failed\n");
            probe_exit();
        }
        printf("probe dispatch accepted %s/%s\n", dispatch.room_id,
               dispatch.call_id);
        fflush(stdout);
        if (g_mode == PROBE_ACK_AND_EXIT) {
            ivr_thread_sleep_ms(250);
            probe_exit();
        }
    }
}

int main(int argc, char **argv) {
    const char *worker_id;
    const char *mode;
    ivr_flowmq_gateway_config_t config;
    ivr_command_gateway_ops_t ops;
    ivr_worker_status_view_t status;
    char sync_message_id[128];
    DataBindError error = DATA_BIND_ERROR_INIT;

    if (argc != 3) {
        fprintf(stderr, "usage: %s WORKER_ID MODE\n", argv[0]);
        return 2;
    }
    worker_id = argv[1];
    mode = argv[2];
    if (strcmp(mode, "drop-before-ack") == 0) {
        g_mode = PROBE_DROP_BEFORE_ACK;
    } else if (strcmp(mode, "ack-exit") == 0) {
        g_mode = PROBE_ACK_AND_EXIT;
    } else if (strcmp(mode, "ack-stay") == 0) {
        g_mode = PROBE_ACK_AND_STAY;
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }

    if (TurboMediaIvrV1_codec_create(&g_codec, &error) != DATA_BIND_OK) {
        return 1;
    }
    memset(&config, 0, sizeof(config));
    config.worker_id = worker_id;
    config.host = "127.0.0.1";
    config.port = 17823;
    config.timeout_ms = 5000;
    config.on_reply = on_reply;
    if (ivr_flowmq_gateway_create(&config, &ops, &g_gateway) != IVR_OK ||
        ivr_flowmq_gateway_start(g_gateway) != IVR_OK) {
        return 1;
    }
    ivr_thread_sleep_ms(800);
    memset(&status, 0, sizeof(status));
    status.instance_id = mode;
    status.connection_generation = 1;
    status.max_sessions = 1;
    status.lease_duration_ms = 15000;
    status.capabilities = "turboxml,flowmq,dispatch-v2,health.ready";
    int sync_id_length = snprintf(sync_message_id, sizeof(sync_message_id),
                                  "probe-sync-%s-%s", worker_id, mode);
    if (sync_id_length <= 0 ||
        (size_t)sync_id_length >= sizeof(sync_message_id)) {
        return 1;
    }
    if (ivr_flowmq_gateway_send_worker_sync_v2(g_gateway, sync_message_id,
                                               &status) != IVR_OK) {
        return 1;
    }
    printf("probe sync sent worker=%s mode=%s\n", worker_id, mode);
    fflush(stdout);
    for (;;) {
        ivr_thread_sleep_ms(100);
    }
}
