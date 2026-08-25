/* Controlled FlowMQ worker used only by process-boundary tests. */
#include "ivr/ivr_worker.h"
#include "ivr_internal.h"
#include "ivr_dtmf_rtp.h"
#include "ivr_flowmq_gateway.h"
#include "ivr_frame.h"
#include "ivr_media_bot.h"
#include "ivr_thread.h"
#include "turbomedia_ivr_v1.h"
#include "turbo_speech.h"
#include "disruptor.h"

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PROBE_DROP_BEFORE_ACK = 0,
    PROBE_ACK_AND_EXIT,
    PROBE_ACK_AND_STAY,
    PROBE_MEDIA_RUNTIME,
    PROBE_ORPHAN_CLOSE_BEFORE,
    PROBE_ORPHAN_CLOSE_AFTER
} probe_mode_t;

static DataBind *g_codec;
static ivr_flowmq_gateway_t *g_gateway;
static probe_mode_t g_mode;
static ivr_worker_t *g_worker;
static const char *g_worker_id;
static const char *g_orphan_resume_path;
static char g_first_orphan_close_id[128];
static unsigned g_media_destroy_count;
static unsigned g_media_play_count;
static atomic_ullong g_connection_generation;
static atomic_int g_ever_connected;
static atomic_int g_connected;
static atomic_int g_epoch_failed;
static const uint64_t TEST_MEDIA_EVENT_TIMESTAMP_MS = 1234u;
static uint64_t g_event_sequence;

enum { PROBE_EVENT_ID_CAPACITY = 128 };
enum {
    PROBE_REPLY_QUEUE_CAPACITY = 16,
    PROBE_REPLY_FRAME_CAPACITY = IVR_FRAME_HEADER_SIZE + 64 * 1024,
    PROBE_LOOP_SLEEP_MS = 10,
    PROBE_SYNC_INTERVAL_MS = 1000,
    PROBE_SYNC_INTERVAL_LOOPS =
        PROBE_SYNC_INTERVAL_MS / PROBE_LOOP_SLEEP_MS
};

typedef struct probe_reply_entry_s {
    size_t length;
    uint8_t frame[PROBE_REPLY_FRAME_CAPACITY];
} probe_reply_entry_t;

static disruptor_t *g_reply_queue;

typedef struct probe_media_instance_s probe_media_instance_t;

struct probe_media_instance_s {
    ivr_media_bot_t *bot;
    ivr_media_port_ops_t bot_ops;
    ivr_dtmf_ingress_t *dtmf;
    turbo_tts_provider_t tts_provider;
    turbo_asr_provider_t asr_provider;
    const turbo_asr_provider_callbacks_t *asr_callbacks;
    void *asr_callback_context;
    uint32_t asr_writes;
    char input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t input_generation;
};

static ivr_status_t probe_publish_event(void *context,
                                        const ivr_event_view_t *event) {
    ivr_event_view_t outbound;
    char event_id[PROBE_EVENT_ID_CAPACITY];
    int written;
    (void)context;
    if (!event || !event->event_type.data) {
        return IVR_EINVAL;
    }
    written = snprintf(event_id, sizeof(event_id), "media-fixture-%llu",
                       (unsigned long long)++g_event_sequence);
    if (written <= 0 || (size_t)written >= sizeof(event_id)) {
        return IVR_ENOSPC;
    }
    outbound = *event;
    outbound.event_id.data = event_id;
    outbound.event_id.size = (size_t)written;
    outbound.sequence = g_event_sequence;
    return ivr_flowmq_gateway_send_media_event(
        g_gateway, g_worker_id, &outbound,
        TEST_MEDIA_EVENT_TIMESTAMP_MS);
}

static void probe_bot_event(void *context, const ivr_event_view_t *event) {
    (void)context;
    if (g_worker && ivr_worker_publish_event_copy(g_worker, event) != IVR_OK) {
        fprintf(stderr, "media fixture event publish failed\n");
    }
}

static int probe_tts_synthesize(
    void *context, const turbo_tts_request_t *request,
    const turbo_tts_provider_callbacks_t *callbacks,
    void *callback_context) {
    static const int16_t pcm[] = {100, 200, 300, 200, 100, 0, -100, 0};
    turbo_speech_audio_frame_t frame;
    (void)context;
    (void)request;
    memset(&frame, 0, sizeof(frame));
    frame.data = (const uint8_t *)pcm;
    frame.len = sizeof(pcm);
    frame.format.sample_rate = 16000;
    frame.format.channels = 1;
    frame.format.bits_per_sample = 16;
    if (callbacks->on_audio(&frame, callback_context) != TURBO_SPEECH_OK) {
        return TURBO_SPEECH_ERR_PROVIDER;
    }
    callbacks->on_complete(callback_context);
    return TURBO_SPEECH_OK;
}

static int probe_tts_cancel(void *context) {
    (void)context;
    return TURBO_SPEECH_OK;
}

static void probe_tts_destroy(void *context) { (void)context; }

static int probe_asr_start(
    void *context, const turbo_asr_config_t *config,
    const turbo_asr_provider_callbacks_t *callbacks,
    void *callback_context) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    (void)config;
    instance->asr_callbacks = callbacks;
    instance->asr_callback_context = callback_context;
    instance->asr_writes = 0;
    return TURBO_SPEECH_OK;
}

static int probe_asr_write(void *context,
                           const turbo_speech_audio_frame_t *frame) {
    static const char transcript[] = "fixture-asr";
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    (void)frame;
    if (++instance->asr_writes == 3u) {
        turbo_asr_result_t result;
        memset(&result, 0, sizeof(result));
        result.text = transcript;
        result.text_len = sizeof(transcript) - 1u;
        result.confidence = 0.95f;
        result.is_final = 1;
        instance->asr_callbacks->on_result(
            &result, instance->asr_callback_context);
    }
    return TURBO_SPEECH_OK;
}

static int probe_asr_finish(void *context) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    instance->asr_callbacks->on_complete(instance->asr_callback_context);
    return TURBO_SPEECH_OK;
}

static int probe_asr_cancel(void *context) {
    (void)context;
    return TURBO_SPEECH_OK;
}

static void probe_asr_destroy(void *context) { (void)context; }

static int probe_play_audio(void *context, const ivr_call_ref_t *call,
                            const uint8_t *pcm, size_t length,
                            uint32_t sample_rate) {
    (void)context;
    (void)call;
    (void)pcm;
    (void)sample_rate;
    return length > 0 ? 0 : -1;
}

static int probe_stop_media(void *context, const ivr_call_ref_t *call) {
    (void)context;
    (void)call;
    return 0;
}

static ivr_status_t probe_start_bot(void *context,
                                    const ivr_call_ref_t *call) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    return instance->bot_ops.start_bot(instance->bot_ops.context, call);
}

static ivr_status_t probe_play(void *context, const ivr_call_ref_t *call,
                               const ivr_bytes_view_t *text) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    ivr_status_t status =
        instance->bot_ops.play_pcm(instance->bot_ops.context, call, text);
    if (status == IVR_OK) {
        ++g_media_play_count;
        printf("probe media play count=%u\n", g_media_play_count);
        fflush(stdout);
    }
    return status;
}

static ivr_status_t probe_emit_dtmf(probe_media_instance_t *instance,
                                    const ivr_call_ref_t *call) {
    static const uint8_t payload[] = {5u, 0x80u | 10u, 0u, 80u};
    static const ivr_bytes_view_t event_type = {"dtmf.final", 10};
    ivr_dtmf_input_t input;
    ivr_event_view_t event;
    char digit[2];
    char payload_json[64];
    int written;
    if (ivr_dtmf_ingress_submit_rtp(instance->dtmf, call, 1u, 9000u,
                                    payload, sizeof(payload),
                                    &input) != IVR_OK) {
        return IVR_ESTATE;
    }
    written = snprintf(payload_json, sizeof(payload_json),
                       "{\"digit\":\"%c\",\"duration\":%u}",
                       input.digit, (unsigned int)input.duration);
    if (written <= 0 || (size_t)written >= sizeof(payload_json)) {
        return IVR_ENOSPC;
    }
    digit[0] = input.digit;
    digit[1] = '\0';
    memset(&event, 0, sizeof(event));
    event.event_type = event_type;
    event.call = *call;
    event.input_id.data = input.input_id;
    event.input_id.size = strlen(input.input_id);
    event.input_value.data = digit;
    event.input_value.size = 1u;
    event.payload_json.data = payload_json;
    event.payload_json.size = (size_t)written;
    return ivr_worker_publish_event_copy(g_worker, &event);
}

static ivr_status_t probe_begin_input(void *context,
                                      const ivr_call_ref_t *call,
                                      const ivr_bytes_view_t *input_id,
                                      uint64_t input_generation) {
    static const uint8_t pcm[32] = {0};
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    char input_id_copy[IVR_MEDIA_ID_CAPACITY];
    if (!input_id || !input_id->data || input_id->size == 0 ||
        input_id->size >= sizeof(input_id_copy)) {
        return IVR_EINVAL;
    }
    memcpy(input_id_copy, input_id->data, input_id->size);
    input_id_copy[input_id->size] = '\0';
    if (ivr_dtmf_ingress_begin_input(instance->dtmf, call, input_id_copy,
                                     input_generation) != IVR_OK) {
        return IVR_ESTATE;
    }
    ivr_status_t status = instance->bot_ops.begin_input(
        instance->bot_ops.context, call, input_id, input_generation);
    if (status != IVR_OK) {
        (void)ivr_dtmf_ingress_end_input(instance->dtmf, call,
                                         input_generation);
        return status;
    }
    memcpy(instance->input_id, input_id_copy, input_id->size + 1u);
    instance->input_generation = input_generation;
    for (int i = 0; i < 3; ++i) {
        if (ivr_media_bot_feed_caller_audio(instance->bot, call, pcm,
                                            sizeof(pcm)) != IVR_OK) {
            return IVR_ESTATE;
        }
    }
    return probe_emit_dtmf(instance, call);
}

static ivr_status_t probe_end_input(void *context,
                                    const ivr_call_ref_t *call,
                                    const ivr_bytes_view_t *input_id,
                                    uint64_t input_generation) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    ivr_status_t bot_status = instance->bot_ops.end_input(
        instance->bot_ops.context, call, input_id, input_generation);
    ivr_status_t dtmf_status = ivr_dtmf_ingress_end_input(
        instance->dtmf, call, input_generation);
    instance->input_id[0] = '\0';
    instance->input_generation = 0;
    return bot_status != IVR_OK ? bot_status : dtmf_status;
}

static ivr_status_t probe_cancel_input(void *context,
                                       const ivr_call_ref_t *call) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    ivr_status_t status = instance->bot_ops.cancel_input(
        instance->bot_ops.context, call);
    if (instance->input_generation != 0) {
        (void)ivr_dtmf_ingress_end_input(instance->dtmf, call,
                                         instance->input_generation);
    }
    instance->input_id[0] = '\0';
    instance->input_generation = 0;
    return status;
}

static ivr_status_t probe_stop_bot(void *context,
                                   const ivr_call_ref_t *call) {
    probe_media_instance_t *instance = (probe_media_instance_t *)context;
    return instance->bot_ops.stop_bot(instance->bot_ops.context, call);
}

static ivr_status_t probe_media_create(void *context,
                                       const ivr_call_ref_t *call,
                                       ivr_media_port_ops_t *out_media,
                                       void **out_instance) {
    probe_media_instance_t *instance;
    ivr_media_bot_config_t bot_config;
    ivr_dtmf_ingress_config_t dtmf_config;
    (void)context;
    (void)call;
    if (!out_media || !out_instance) {
        return IVR_EINVAL;
    }
    instance = (probe_media_instance_t *)calloc(1, sizeof(*instance));
    if (!instance) {
        return IVR_ENOSPC;
    }
    instance->tts_provider.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION;
    instance->tts_provider.context = instance;
    instance->tts_provider.synthesize = probe_tts_synthesize;
    instance->tts_provider.cancel = probe_tts_cancel;
    instance->tts_provider.destroy = probe_tts_destroy;
    instance->asr_provider.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION;
    instance->asr_provider.context = instance;
    instance->asr_provider.start = probe_asr_start;
    instance->asr_provider.write = probe_asr_write;
    instance->asr_provider.finish = probe_asr_finish;
    instance->asr_provider.cancel = probe_asr_cancel;
    instance->asr_provider.destroy = probe_asr_destroy;
    memset(&bot_config, 0, sizeof(bot_config));
    bot_config.tts_provider = &instance->tts_provider;
    bot_config.asr_provider = &instance->asr_provider;
    bot_config.transport.context = instance;
    bot_config.transport.play_audio = probe_play_audio;
    bot_config.transport.stop = probe_stop_media;
    bot_config.sample_rate = 16000u;
    bot_config.on_event = probe_bot_event;
    bot_config.event_ctx = instance;
    memset(&dtmf_config, 0, sizeof(dtmf_config));
    dtmf_config.window_capacity = 1u;
    if (ivr_media_bot_create(&bot_config, &instance->bot) != IVR_OK ||
        ivr_dtmf_ingress_create(&dtmf_config, &instance->dtmf) != IVR_OK) {
        ivr_media_bot_destroy(instance->bot);
        ivr_dtmf_ingress_destroy(instance->dtmf);
        free(instance);
        return IVR_ENOSPC;
    }
    ivr_media_bot_get_ops(instance->bot, &instance->bot_ops);
    memset(out_media, 0, sizeof(*out_media));
    out_media->abi_version = IVR_WORKER_ABI_VERSION;
    out_media->context = instance;
    out_media->start_bot = probe_start_bot;
    out_media->play_pcm = probe_play;
    out_media->cancel_input = probe_cancel_input;
    out_media->stop_bot = probe_stop_bot;
    out_media->begin_input = probe_begin_input;
    out_media->end_input = probe_end_input;
    *out_instance = instance;
    return IVR_OK;
}

static void probe_media_destroy(void *context, void *opaque_instance) {
    probe_media_instance_t *instance =
        (probe_media_instance_t *)opaque_instance;
    (void)context;
    if (!instance) {
        return;
    }
    ivr_dtmf_ingress_destroy(instance->dtmf);
    ivr_media_bot_destroy(instance->bot);
    free(instance);
    ++g_media_destroy_count;
    printf("probe media destroy count=%u\n", g_media_destroy_count);
    fflush(stdout);
}

static int probe_mode_uses_media_runtime(void) {
    return g_mode == PROBE_MEDIA_RUNTIME ||
           g_mode == PROBE_ORPHAN_CLOSE_BEFORE ||
           g_mode == PROBE_ORPHAN_CLOSE_AFTER;
}

static int orphan_resume_armed(void) {
    FILE *marker;
    if (!g_orphan_resume_path || !g_orphan_resume_path[0]) return 0;
    marker = fopen(g_orphan_resume_path, "rb");
    if (!marker) return 0;
    fclose(marker);
    return 1;
}

static ivr_status_t probe_execute_media(const ivr_media_command_t *command) {
    ivr_call_ref_t call;
    ivr_media_operation_t operation;
    ivr_bytes_view_t input;
    memset(&call, 0, sizeof(call));
    call.tenant_id.data = command->tenant_id;
    call.tenant_id.size = strlen(command->tenant_id);
    call.provider_session_id.data = command->provider_session_id;
    call.provider_session_id.size = strlen(command->provider_session_id);
    call.dialog_id.data = command->dialog_id;
    call.dialog_id.size = strlen(command->dialog_id);
    call.room_id.data = command->room_id;
    call.room_id.size = strlen(command->room_id);
    call.call_id.data = command->call_id;
    call.call_id.size = strlen(command->call_id);
    call.call_generation = command->call_generation;
    memset(&operation, 0, sizeof(operation));
    operation.call = call;
    operation.operation_generation = command->operation_generation;
    switch (command->kind) {
        case IVR_MEDIA_COMMAND_SESSION_OPEN:
            return ivr_worker_open_media_operation(g_worker, &operation);
        case IVR_MEDIA_COMMAND_PLAY:
            input.data = command->text;
            input.size = strlen(command->text);
            return ivr_worker_play(g_worker, &operation, &input);
        case IVR_MEDIA_COMMAND_INPUT_START:
            input.data = command->input_id;
            input.size = strlen(command->input_id);
            return ivr_worker_begin_input(g_worker, &operation, &input,
                                          command->input_generation);
        case IVR_MEDIA_COMMAND_INPUT_STOP:
            input.data = command->input_id;
            input.size = strlen(command->input_id);
            return ivr_worker_end_input(g_worker, &operation, &input,
                                        command->input_generation);
        case IVR_MEDIA_COMMAND_CANCEL:
            return ivr_worker_cancel_input(g_worker, &operation, &input,
                                           command->input_generation);
        case IVR_MEDIA_COMMAND_SESSION_CLOSE:
            return ivr_worker_close_media_call(g_worker, &call);
        default:
            return IVR_EINVAL;
    }
}

static void probe_exit(void) {
    fflush(stdout);
    fflush(stderr);
    _Exit(0);
}

static int is_media_command_type(uint32_t type_id) {
    switch (type_id) {
        case IVR_TYPE_MEDIA_SESSION_OPEN_COMMAND_V1:
        case IVR_TYPE_MEDIA_PLAY_COMMAND_V1:
        case IVR_TYPE_MEDIA_INPUT_START_COMMAND_V1:
        case IVR_TYPE_MEDIA_INPUT_STOP_COMMAND_V1:
        case IVR_TYPE_MEDIA_CANCEL_COMMAND_V2:
        case IVR_TYPE_MEDIA_SESSION_CLOSE_COMMAND_V1:
            return 1;
        default:
            return 0;
    }
}

static void handle_media_command(const uint8_t *frame, size_t length) {
    static const char empty[] = "";
    static const char event_type[] = "playback.finished";
    static const char payload[] = "{\"probe\":true}";
    ivr_media_command_t command;
    ivr_event_view_t event;
    char event_id[PROBE_EVENT_ID_CAPACITY];
    int event_id_length;

    if (ivr_flowmq_gateway_decode_media_command(g_codec, frame, length,
                                                &command) != IVR_OK) {
        return;
    }
    printf("probe media received %s kind=%d\n", command.message_id,
           (int)command.kind);
    fflush(stdout);
    if (g_mode == PROBE_DROP_BEFORE_ACK) {
        probe_exit();
    }
    if (probe_mode_uses_media_runtime()) {
        ivr_status_t status;
        if (command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE &&
            g_mode == PROBE_ORPHAN_CLOSE_BEFORE &&
            !orphan_resume_armed()) {
            if (!g_first_orphan_close_id[0]) {
                snprintf(g_first_orphan_close_id,
                         sizeof(g_first_orphan_close_id), "%s",
                         command.message_id);
                printf("probe orphan close before id=%s\n",
                       command.message_id);
            } else {
                printf("probe orphan close waiting stable=%d id=%s\n",
                       strcmp(g_first_orphan_close_id,
                              command.message_id) == 0,
                       command.message_id);
            }
            fflush(stdout);
            return;
        }
        status = probe_execute_media(&command);
        if (command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE &&
            g_mode == PROBE_ORPHAN_CLOSE_BEFORE) {
            printf("probe orphan close retry stable=%d id=%s\n",
                   g_first_orphan_close_id[0] &&
                       strcmp(g_first_orphan_close_id,
                              command.message_id) == 0,
                   command.message_id);
            fflush(stdout);
        }
        if (command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE &&
            g_mode == PROBE_ORPHAN_CLOSE_AFTER &&
            !orphan_resume_armed()) {
            printf("probe orphan close after applied id=%s status=%d\n",
                   command.message_id, (int)status);
            fflush(stdout);
            while (!orphan_resume_armed()) {
                ivr_thread_sleep_ms(PROBE_LOOP_SLEEP_MS);
            }
            return;
        }
        if (ivr_flowmq_gateway_send_media_result(
                g_gateway, &command, status,
                status == IVR_OK ? "" : "media.fixture.rejected",
                status == IVR_OK ? "" : "media fixture rejected command") !=
            IVR_OK) {
            fprintf(stderr, "media fixture result send failed\n");
            probe_exit();
        }
        printf("media runtime completed %s kind=%d status=%d\n",
               command.message_id, (int)command.kind, (int)status);
        fflush(stdout);
        return;
    }
    if (ivr_flowmq_gateway_send_media_result(g_gateway, &command, IVR_OK, "",
                                             "") != IVR_OK) {
        fprintf(stderr, "probe media result send failed\n");
        probe_exit();
    }
    printf("probe media completed %s\n", command.message_id);
    fflush(stdout);
    if (g_mode == PROBE_ACK_AND_EXIT) {
        ivr_thread_sleep_ms(250);
        probe_exit();
    }
    if (command.kind != IVR_MEDIA_COMMAND_PLAY) return;

    event_id_length = snprintf(event_id, sizeof(event_id), "event-%s",
                               command.message_id);
    if (event_id_length <= 0 || (size_t)event_id_length >= sizeof(event_id)) {
        fprintf(stderr, "probe media event id overflow\n");
        probe_exit();
    }
    memset(&event, 0, sizeof(event));
    event.event_id.data = event_id;
    event.event_id.size = (size_t)event_id_length;
    event.call.tenant_id.data = command.tenant_id;
    event.call.tenant_id.size = strlen(command.tenant_id);
    event.call.provider_session_id.data = command.provider_session_id;
    event.call.provider_session_id.size = strlen(command.provider_session_id);
    event.call.dialog_id.data = command.dialog_id;
    event.call.dialog_id.size = strlen(command.dialog_id);
    event.call.room_id.data = command.room_id;
    event.call.room_id.size = strlen(command.room_id);
    event.call.call_id.data = command.call_id;
    event.call.call_id.size = strlen(command.call_id);
    event.call.call_generation = command.call_generation;
    event.event_type.data = event_type;
    event.event_type.size = sizeof(event_type) - 1u;
    event.input_id.data = empty;
    event.input_value.data = empty;
    event.payload_json.data = payload;
    event.payload_json.size = sizeof(payload) - 1u;
    if (ivr_flowmq_gateway_send_media_event(
            g_gateway, command.worker_id, &event,
            TEST_MEDIA_EVENT_TIMESTAMP_MS) != IVR_OK) {
        fprintf(stderr, "probe media event send failed\n");
        probe_exit();
    }
}

static void handle_inventory_query(const uint8_t *frame, size_t length) {
    ivr_worker_inventory_request_t request;
    ivr_worker_inventory_envelope_t result;
    ivr_status_t status = ivr_flowmq_gateway_decode_inventory_query(
        g_codec, frame, length, &request);
    if (!request.message_id[0] || !request.worker_id[0]) return;
    memset(&result, 0, sizeof(result));
    snprintf(result.message_id, sizeof(result.message_id), "%s",
             request.message_id);
    snprintf(result.worker_id, sizeof(result.worker_id), "%s",
             request.worker_id);
    if (status == IVR_OK) {
        if (g_worker) {
            status = ivr_worker_query_inventory(g_worker, &request.query,
                                                &result.page);
        } else if (request.query.inventory_version !=
                       IVR_WORKER_INVENTORY_VERSION ||
                   request.query.cursor != 0u ||
                   (request.query.expected_revision != 0u &&
                    request.query.expected_revision != 1u)) {
            status = IVR_EVERSION;
        } else {
            result.page.inventory_version = IVR_WORKER_INVENTORY_VERSION;
            result.page.revision = 1u;
            result.page.cursor = 0u;
            result.page.total_active = 0u;
        }
    }
    result.status_code = status;
    if (status != IVR_OK) {
        result.page.inventory_version = request.query.inventory_version;
        snprintf(result.error_code, sizeof(result.error_code), "%s",
                 status == IVR_EVERSION ? "inventory.version_unsupported"
                                        : "inventory.query_failed");
        snprintf(result.error_message, sizeof(result.error_message),
                 "inventory query rejected");
    }
    status = ivr_flowmq_gateway_send_inventory_page(g_gateway, &result);
    if (status != IVR_OK) {
        fprintf(stderr,
                "probe inventory page send failed status=%d envelope=%s/%s "
                "page=%u/%llu/%u/%u more=%d next=%u\n",
                status, result.message_id, result.worker_id,
                result.page.inventory_version,
                (unsigned long long)result.page.revision,
                result.page.count, result.page.total_active,
                result.page.has_more, result.page.next_cursor);
        for (uint32_t index = 0u; index < result.page.count; ++index) {
            const ivr_worker_inventory_record_t *record =
                &result.page.records[index];
            fprintf(stderr,
                    "probe inventory record[%u]=worker:%s instance:%s "
                    "epoch:%llu tenant:%s session:%s dialog:%s room:%s "
                    "call:%s/%llu operation:%llu input:%s/%llu/%d "
                    "state:%d rebindable:%d\n",
                    index, record->worker_id, record->worker_instance_id,
                    (unsigned long long)record->worker_epoch,
                    record->tenant_id, record->provider_session_id,
                    record->dialog_id, record->room_id, record->call_id,
                    (unsigned long long)record->call_generation,
                    (unsigned long long)record->operation_generation,
                    record->input_id,
                    (unsigned long long)record->input_generation,
                    record->input_active, (int)record->state,
                    record->rebindable);
        }
        probe_exit();
    }
}

static void handle_reply(const uint8_t *frame, size_t length) {
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, length, &info) != IVR_OK) {
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        info.schema_type_id == IVR_TYPE_WORKER_MEDIA_INVENTORY_QUERY_V1) {
        handle_inventory_query(frame, length);
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
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        is_media_command_type(info.schema_type_id)) {
        handle_media_command(frame, length);
    }
}

static void on_reply(void *context, const uint8_t *frame, size_t length) {
    disruptor_cursor_t cursor;
    probe_reply_entry_t *entry;
    (void)context;
    if (!g_reply_queue || !frame || length == 0u ||
        length > PROBE_REPLY_FRAME_CAPACITY ||
        !disruptor_publisher_try_claim(g_reply_queue, &cursor)) {
        fprintf(stderr, "probe reply queue rejected FlowMQ frame\n");
        probe_exit();
    }
    entry = (probe_reply_entry_t *)disruptor_acquire_entry(g_reply_queue,
                                                            &cursor);
    entry->length = length;
    memcpy(entry->frame, frame, length);
    if (!disruptor_publisher_publish(g_reply_queue, &cursor)) {
        fprintf(stderr, "probe reply queue publish failed\n");
        probe_exit();
    }
}

static int probe_reply_queue_create(void) {
    disruptor_config_t config;
    memset(&config, 0, sizeof(config));
    config.entry_size = sizeof(probe_reply_entry_t);
    config.capacity = PROBE_REPLY_QUEUE_CAPACITY;
    config.consumer_capacity = 1u;
    config.mode = DISRUPTOR_MODE_WORKER_POOL;
    g_reply_queue = disruptor_create(&config);
    return g_reply_queue ? 0 : -1;
}

static void probe_process_replies(void) {
    disruptor_cursor_t cursor;
    while (g_reply_queue &&
           disruptor_worker_try_claim(g_reply_queue, &cursor)) {
        const probe_reply_entry_t *entry =
            (const probe_reply_entry_t *)disruptor_show_entry(g_reply_queue,
                                                               &cursor);
        handle_reply(entry->frame, entry->length);
        disruptor_worker_release_entry(g_reply_queue, &cursor);
    }
}

static void probe_connection_changed(void *context, int connected) {
    int previous;
    (void)context;
    previous = atomic_load_explicit(&g_connected, memory_order_acquire);
    if (connected && !previous &&
        atomic_exchange_explicit(&g_ever_connected, 1,
                                 memory_order_acq_rel)) {
        uint64_t current = atomic_load_explicit(
            &g_connection_generation, memory_order_acquire);
        uint64_t next = current == UINT64_MAX ? 0 : current + 1u;
        if (next == 0 || !g_worker ||
            ivr_worker_advance_epoch(g_worker, next) != IVR_OK) {
            atomic_store_explicit(&g_epoch_failed, 1,
                                  memory_order_release);
            connected = 0;
        } else {
            atomic_store_explicit(&g_connection_generation, next,
                                  memory_order_release);
            printf("probe connection generation=%llu\n",
                   (unsigned long long)next);
            fflush(stdout);
        }
    }
    atomic_store_explicit(&g_connected, connected, memory_order_release);
}

static int probe_send_worker_sync(ivr_worker_status_view_t *status,
                                  uint64_t *sequence) {
    char message_id[128];
    int message_id_length;
    ivr_status_t send_status;
    if (!status || !sequence || *sequence == UINT64_MAX) {
        return -1;
    }
    ++*sequence;
    status->active_sessions =
        g_worker ? ivr_worker_active_sessions(g_worker) : 0u;
    status->reserved_sessions = 0u;
    status->health_generation = *sequence;
    message_id_length = snprintf(
        message_id, sizeof(message_id), "probe-sync-%s-%llu", g_worker_id,
        (unsigned long long)*sequence);
    if (message_id_length <= 0 ||
        (size_t)message_id_length >= sizeof(message_id)) {
        return -1;
    }
    send_status = ivr_flowmq_gateway_send_worker_sync_v2(
        g_gateway, message_id, status);
    return send_status == IVR_OK ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *worker_id;
    const char *mode;
    ivr_flowmq_gateway_config_t config;
    ivr_command_gateway_ops_t ops;
    ivr_worker_status_view_t status;
    uint64_t sync_sequence = 0u;
    uint32_t sync_loops_remaining = PROBE_SYNC_INTERVAL_LOOPS;
    DataBindError error = DATA_BIND_ERROR_INIT;

    atomic_init(&g_connection_generation, 1u);
    atomic_init(&g_ever_connected, 0);
    atomic_init(&g_connected, 0);
    atomic_init(&g_epoch_failed, 0);

    if (argc != 3) {
        fprintf(stderr, "usage: %s WORKER_ID MODE\n", argv[0]);
        return 2;
    }
    worker_id = argv[1];
    g_worker_id = worker_id;
    mode = argv[2];
    if (strcmp(mode, "drop-before-ack") == 0) {
        g_mode = PROBE_DROP_BEFORE_ACK;
    } else if (strcmp(mode, "ack-exit") == 0) {
        g_mode = PROBE_ACK_AND_EXIT;
    } else if (strcmp(mode, "ack-stay") == 0) {
        g_mode = PROBE_ACK_AND_STAY;
    } else if (strcmp(mode, "media-runtime") == 0) {
        g_mode = PROBE_MEDIA_RUNTIME;
    } else if (strcmp(mode, "orphan-close-before") == 0) {
        g_mode = PROBE_ORPHAN_CLOSE_BEFORE;
    } else if (strcmp(mode, "orphan-close-after") == 0) {
        g_mode = PROBE_ORPHAN_CLOSE_AFTER;
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }

    if (TurboMediaIvrV1_codec_create(&g_codec, &error) != DATA_BIND_OK) {
        return 1;
    }
    if (probe_reply_queue_create() != 0) {
        return 1;
    }
    g_orphan_resume_path = getenv("PROBE_ORPHAN_RESUME_PATH");
    if ((g_mode == PROBE_ORPHAN_CLOSE_BEFORE ||
         g_mode == PROBE_ORPHAN_CLOSE_AFTER) &&
        (!g_orphan_resume_path || !g_orphan_resume_path[0])) {
        fprintf(stderr, "PROBE_ORPHAN_RESUME_PATH is required\n");
        return 2;
    }
    if (probe_mode_uses_media_runtime()) {
        ivr_worker_config_t worker_config;
        ivr_media_event_sink_ops_t event_sink;
        ivr_media_port_factory_ops_t media_factory;
        memset(&worker_config, 0, sizeof(worker_config));
        worker_config.abi_version = IVR_WORKER_ABI_VERSION;
        worker_config.worker_id = worker_id;
        worker_config.worker_instance_id = mode;
        worker_config.worker_epoch = 1u;
        worker_config.max_sessions_per_worker = 1u;
        memset(&event_sink, 0, sizeof(event_sink));
        event_sink.abi_version = IVR_WORKER_ABI_VERSION;
        event_sink.publish_copy = probe_publish_event;
        memset(&media_factory, 0, sizeof(media_factory));
        media_factory.abi_version = IVR_WORKER_ABI_VERSION;
        media_factory.create = probe_media_create;
        media_factory.destroy = probe_media_destroy;
        if (ivr_worker_create(&worker_config, &event_sink, &media_factory,
                              &g_worker) != IVR_OK ||
            ivr_worker_start(g_worker) != IVR_OK) {
            return 1;
        }
    }
    memset(&config, 0, sizeof(config));
    config.worker_id = worker_id;
    config.host = "127.0.0.1";
    config.port = 17823;
    config.timeout_ms = 5000;
    config.on_reply = on_reply;
    config.on_connection = probe_connection_changed;
    if (ivr_flowmq_gateway_create(&config, &ops, &g_gateway) != IVR_OK ||
        ivr_flowmq_gateway_start(g_gateway) != IVR_OK) {
        return 1;
    }
    ivr_thread_sleep_ms(800);
    memset(&status, 0, sizeof(status));
    status.instance_id = mode;
    status.connection_generation = atomic_load_explicit(
        &g_connection_generation, memory_order_acquire);
    status.max_sessions = 1;
    status.lease_duration_ms = 15000;
    status.health_ready = 1;
    status.capabilities =
        probe_mode_uses_media_runtime()
            ? "media-executor,flowmq,dispatch-v2,tts,asr,health.ready"
            : "turboxml,flowmq,dispatch-v2,health.ready";
    if (probe_send_worker_sync(&status, &sync_sequence) != 1) {
        return 1;
    }
    printf("probe sync sent worker=%s mode=%s\n", worker_id, mode);
    fflush(stdout);
    for (;;) {
        if (atomic_load_explicit(&g_epoch_failed, memory_order_acquire)) {
            fprintf(stderr, "probe connection epoch advance failed\n");
            return 1;
        }
        probe_process_replies();
        status.connection_generation = atomic_load_explicit(
            &g_connection_generation, memory_order_acquire);
        if (--sync_loops_remaining == 0u) {
            int sync_result = probe_send_worker_sync(&status, &sync_sequence);
            if (sync_result < 0) {
                fprintf(stderr, "probe periodic sync state invalid\n");
                return 1;
            }
            sync_loops_remaining = PROBE_SYNC_INTERVAL_LOOPS;
        }
        ivr_thread_sleep_ms(PROBE_LOOP_SLEEP_MS);
    }
}
