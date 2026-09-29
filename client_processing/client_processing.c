#include "turbo_client_processing.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <salts/thread.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_CAPACITY = 8
};

#define TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_BYTES (8u * 1024u * 1024u)
#define TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_DURATION_US UINT64_C(500000)

typedef struct turbo_client_processing_video_slot_t {
    size_t offset;
    size_t size;
    int width;
    int height;
    uint64_t timestamp_us;
} turbo_client_processing_video_slot_t;

typedef struct turbo_client_processing_audio_slot_t {
    size_t offset;
    size_t size;
    uint64_t timestamp_us;
} turbo_client_processing_audio_slot_t;

struct turbo_client_processing_s {
    turbo_client_processing_config_t config;
    atomic_int state;
    salts_mutex_t frame_mutex;
    turbo_client_processing_video_slot_t *frame_slots;
    uint8_t *frame_storage;
    size_t frame_head;
    size_t frame_storage_tail;
    size_t queued_frames;
    size_t queued_bytes;
    uint64_t queued_duration_us;
    uint64_t admitted_frames;
    uint64_t rejected_frames;

    salts_mutex_t audio_mutex;
    turbo_client_processing_audio_capture_config_t audio_config;
    turbo_client_processing_audio_slot_t *audio_slots;
    uint8_t *audio_storage;
    size_t audio_head;
    size_t audio_storage_tail;
    size_t queued_audio_frames;
    size_t queued_audio_bytes;
    uint64_t queued_audio_duration_us;
    uint64_t admitted_audio_frames;
    uint64_t rejected_audio_frames;
    int audio_configured;

    char *input_path;
    char *output_path;
    char *output_format;
    char *video_codec;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint64_t bitrate;
    int file_plan_set;
    int file_prepared;

    AVFormatContext *input;
    AVFormatContext *output;
    AVCodecContext *decoder;
    AVCodecContext *encoder;
    AVStream *input_stream;
    AVStream *output_stream;
    AVPacket *input_packet;
    AVPacket *output_packet;
    AVFrame *decoded_frame;
    AVFrame *converted_frame;
    struct SwsContext *sws;
    int video_stream_index;
    int source_width;
    int source_height;
    enum AVPixelFormat source_format;
    int64_t next_pts;
    int output_header_written;
};

static turbo_client_processing_state_t turbo_client_processing_state_get(
    const turbo_client_processing_t *processing) {
    return (turbo_client_processing_state_t)atomic_load_explicit(
        &processing->state, memory_order_acquire);
}

static void turbo_client_processing_state_set(
    turbo_client_processing_t *processing,
    turbo_client_processing_state_t state) {
    atomic_store_explicit(&processing->state, (int)state, memory_order_release);
}

static void turbo_client_processing_queue_recompute_duration_locked(
    turbo_client_processing_t *processing) {
    size_t tail_index;
    uint64_t first_timestamp;
    uint64_t last_timestamp;
    if (processing->queued_frames <= 1u) {
        processing->queued_duration_us = 0u;
        return;
    }
    tail_index = (processing->frame_head + processing->queued_frames - 1u) %
                 processing->config.frame_queue_capacity;
    first_timestamp =
        processing->frame_slots[processing->frame_head].timestamp_us;
    last_timestamp = processing->frame_slots[tail_index].timestamp_us;
    processing->queued_duration_us =
        last_timestamp >= first_timestamp
            ? last_timestamp - first_timestamp
            : 0u;
}

static void turbo_client_processing_queue_clear_locked(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return;
    }
    memset(processing->frame_slots, 0,
           processing->config.frame_queue_capacity *
               sizeof(*processing->frame_slots));
    processing->frame_head = 0u;
    processing->frame_storage_tail = 0u;
    processing->queued_frames = 0u;
    processing->queued_bytes = 0u;
    processing->queued_duration_us = 0u;
}

static void turbo_client_processing_audio_recompute_duration_locked(
    turbo_client_processing_t *processing) {
    size_t tail_index;
    uint64_t first_timestamp;
    uint64_t last_timestamp;
    if (processing->queued_audio_frames <= 1u) {
        processing->queued_audio_duration_us = 0u;
        return;
    }
    tail_index =
        (processing->audio_head + processing->queued_audio_frames - 1u) %
        processing->audio_config.queue_capacity;
    first_timestamp =
        processing->audio_slots[processing->audio_head].timestamp_us;
    last_timestamp = processing->audio_slots[tail_index].timestamp_us;
    processing->queued_audio_duration_us =
        last_timestamp >= first_timestamp
            ? last_timestamp - first_timestamp
            : 0u;
}

static void turbo_client_processing_audio_clear_locked(
    turbo_client_processing_t *processing) {
    if (processing == NULL || !processing->audio_configured) {
        return;
    }
    memset(processing->audio_slots, 0,
           processing->audio_config.queue_capacity *
               sizeof(*processing->audio_slots));
    processing->audio_head = 0u;
    processing->audio_storage_tail = 0u;
    processing->queued_audio_frames = 0u;
    processing->queued_audio_bytes = 0u;
    processing->queued_audio_duration_us = 0u;
}

static int turbo_client_processing_audio_config_valid(
    const turbo_client_processing_audio_capture_config_t *config) {
    return config != NULL &&
           config->size == sizeof(*config) &&
           config->queue_capacity > 0u &&
           config->queue_capacity <=
               SIZE_MAX / sizeof(turbo_client_processing_audio_slot_t) &&
           config->queue_max_bytes > 0u &&
           config->queue_max_duration_us > 0u &&
           (config->sample_rate == 8000u ||
            config->sample_rate == 16000u ||
            config->sample_rate == 24000u ||
            config->sample_rate == 48000u) &&
           (config->channels == 1u || config->channels == 2u) &&
           (config->bits_per_sample == 16u ||
            config->bits_per_sample == 32u);
}

static size_t turbo_client_processing_audio_frame_bytes(
    const turbo_client_processing_audio_capture_config_t *config) {
    return (size_t)config->channels * (size_t)(config->bits_per_sample / 8u);
}

static int turbo_client_processing_config_valid(
    const turbo_client_processing_config_t *config) {
    return config != NULL &&
           config->size == sizeof(*config) &&
           config->frame_queue_capacity > 0u &&
           config->frame_queue_capacity <=
               SIZE_MAX / sizeof(turbo_client_processing_video_slot_t) &&
           config->frame_queue_max_bytes > 0u &&
           config->frame_queue_max_duration_us > 0u;
}

static int turbo_client_processing_string_present(const char *value) {
    return value != NULL && value[0] != '\0';
}

static char *turbo_client_processing_copy_string(const char *value) {
    size_t size;
    char *copy;
    if (!turbo_client_processing_string_present(value)) {
        return NULL;
    }
    size = strlen(value) + 1u;
    copy = (char *)malloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

static int turbo_client_processing_file_plan_valid(
    const turbo_client_processing_file_plan_t *plan) {
    return plan != NULL &&
           plan->size == sizeof(*plan) &&
           turbo_client_processing_string_present(plan->input_path) &&
           turbo_client_processing_string_present(plan->output_path) &&
           turbo_client_processing_string_present(plan->output_format) &&
           turbo_client_processing_string_present(plan->video_codec) &&
           plan->output_width > 0u && plan->output_width <= (uint32_t)INT_MAX &&
           plan->output_height > 0u && plan->output_height <= (uint32_t)INT_MAX &&
           plan->frame_rate_num > 0u && plan->frame_rate_num <= (uint32_t)INT_MAX &&
           plan->frame_rate_den > 0u && plan->frame_rate_den <= (uint32_t)INT_MAX &&
           plan->bitrate > 0u && plan->bitrate <= (uint64_t)INT64_MAX;
}

static void turbo_client_processing_file_runtime_clear(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return;
    }
    sws_freeContext(processing->sws);
    processing->sws = NULL;
    av_frame_free(&processing->decoded_frame);
    av_frame_free(&processing->converted_frame);
    av_packet_free(&processing->input_packet);
    av_packet_free(&processing->output_packet);
    avcodec_free_context(&processing->decoder);
    avcodec_free_context(&processing->encoder);
    if (processing->input != NULL) {
        avformat_close_input(&processing->input);
    }
    if (processing->output != NULL) {
        if (processing->output->pb != NULL &&
            !(processing->output->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&processing->output->pb);
        }
        avformat_free_context(processing->output);
        processing->output = NULL;
    }
    processing->input_stream = NULL;
    processing->output_stream = NULL;
    processing->video_stream_index = -1;
    processing->source_width = 0;
    processing->source_height = 0;
    processing->source_format = AV_PIX_FMT_NONE;
    processing->next_pts = 0;
    processing->output_header_written = 0;
    processing->file_prepared = 0;
}

static void turbo_client_processing_file_plan_clear(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return;
    }
    free(processing->input_path);
    free(processing->output_path);
    free(processing->output_format);
    free(processing->video_codec);
    processing->input_path = NULL;
    processing->output_path = NULL;
    processing->output_format = NULL;
    processing->video_codec = NULL;
    processing->output_width = 0u;
    processing->output_height = 0u;
    processing->frame_rate_num = 0u;
    processing->frame_rate_den = 0u;
    processing->bitrate = 0u;
    processing->file_plan_set = 0;
}

static int turbo_client_processing_encoder_supports_yuv420p(
    const AVCodec *encoder) {
    const enum AVPixelFormat *format;
    if (encoder == NULL || encoder->pix_fmts == NULL) {
        return 0;
    }
    for (format = encoder->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_YUV420P) {
            return 1;
        }
    }
    return 0;
}

static turbo_client_processing_status_t turbo_client_processing_file_prepare(
    turbo_client_processing_t *processing) {
    const AVCodec *decoder = NULL;
    const AVCodec *encoder;
    int result;

    result = avformat_open_input(
        &processing->input, processing->input_path, NULL, NULL);
    if (result < 0) {
        return TURBO_CLIENT_PROCESSING_EOPEN;
    }
    result = avformat_find_stream_info(processing->input, NULL);
    if (result < 0) {
        return TURBO_CLIENT_PROCESSING_EOPEN;
    }

    processing->video_stream_index = av_find_best_stream(
        processing->input, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (processing->video_stream_index < 0 || decoder == NULL) {
        return TURBO_CLIENT_PROCESSING_EUNSUPPORTED;
    }
    processing->input_stream =
        processing->input->streams[processing->video_stream_index];

    processing->decoder = avcodec_alloc_context3(decoder);
    if (processing->decoder == NULL) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    result = avcodec_parameters_to_context(
        processing->decoder, processing->input_stream->codecpar);
    if (result < 0 || avcodec_open2(processing->decoder, decoder, NULL) < 0) {
        return TURBO_CLIENT_PROCESSING_ECODEC;
    }

    result = avformat_alloc_output_context2(
        &processing->output, NULL, processing->output_format,
        processing->output_path);
    if (result < 0 || processing->output == NULL) {
        return TURBO_CLIENT_PROCESSING_EUNSUPPORTED;
    }

    encoder = avcodec_find_encoder_by_name(processing->video_codec);
    if (encoder == NULL ||
        !turbo_client_processing_encoder_supports_yuv420p(encoder)) {
        return TURBO_CLIENT_PROCESSING_EUNSUPPORTED;
    }
    processing->encoder = avcodec_alloc_context3(encoder);
    if (processing->encoder == NULL) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->encoder->codec_type = AVMEDIA_TYPE_VIDEO;
    processing->encoder->width = (int)processing->output_width;
    processing->encoder->height = (int)processing->output_height;
    processing->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    processing->encoder->time_base =
        (AVRational){(int)processing->frame_rate_den,
                     (int)processing->frame_rate_num};
    processing->encoder->framerate =
        (AVRational){(int)processing->frame_rate_num,
                     (int)processing->frame_rate_den};
    processing->encoder->bit_rate = (int64_t)processing->bitrate;
    processing->encoder->gop_size = 12;
    processing->encoder->max_b_frames = 0;
    if (processing->output->oformat->flags & AVFMT_GLOBALHEADER) {
        processing->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(processing->encoder, encoder, NULL) < 0) {
        return TURBO_CLIENT_PROCESSING_ECODEC;
    }

    processing->output_stream =
        avformat_new_stream(processing->output, NULL);
    if (processing->output_stream == NULL) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->output_stream->time_base = processing->encoder->time_base;
    if (avcodec_parameters_from_context(
            processing->output_stream->codecpar, processing->encoder) < 0) {
        return TURBO_CLIENT_PROCESSING_ECODEC;
    }

    if (!(processing->output->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&processing->output->pb, processing->output_path,
                      AVIO_FLAG_WRITE) < 0) {
            return TURBO_CLIENT_PROCESSING_EOPEN;
        }
    }
    if (avformat_write_header(processing->output, NULL) < 0) {
        return TURBO_CLIENT_PROCESSING_EIO;
    }
    processing->output_header_written = 1;

    processing->input_packet = av_packet_alloc();
    processing->output_packet = av_packet_alloc();
    processing->decoded_frame = av_frame_alloc();
    processing->converted_frame = av_frame_alloc();
    if (processing->input_packet == NULL ||
        processing->output_packet == NULL ||
        processing->decoded_frame == NULL ||
        processing->converted_frame == NULL) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }

    processing->converted_frame->format = AV_PIX_FMT_YUV420P;
    processing->converted_frame->width = processing->encoder->width;
    processing->converted_frame->height = processing->encoder->height;
    if (av_frame_get_buffer(processing->converted_frame, 32) < 0) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->file_prepared = 1;
    return TURBO_CLIENT_PROCESSING_OK;
}

static turbo_client_processing_status_t
turbo_client_processing_write_encoded_packets(
    turbo_client_processing_t *processing) {
    int result;
    for (;;) {
        result = avcodec_receive_packet(
            processing->encoder, processing->output_packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return TURBO_CLIENT_PROCESSING_OK;
        }
        if (result < 0) {
            return TURBO_CLIENT_PROCESSING_ECODEC;
        }
        av_packet_rescale_ts(
            processing->output_packet,
            processing->encoder->time_base,
            processing->output_stream->time_base);
        processing->output_packet->stream_index =
            processing->output_stream->index;
        result = av_interleaved_write_frame(
            processing->output, processing->output_packet);
        av_packet_unref(processing->output_packet);
        if (result < 0) {
            return TURBO_CLIENT_PROCESSING_EIO;
        }
    }
}

static turbo_client_processing_status_t
turbo_client_processing_encode_frame(
    turbo_client_processing_t *processing, AVFrame *frame) {
    turbo_client_processing_status_t status;
    int result;
    for (;;) {
        result = avcodec_send_frame(processing->encoder, frame);
        if (result != AVERROR(EAGAIN)) {
            break;
        }
        status = turbo_client_processing_write_encoded_packets(processing);
        if (status != TURBO_CLIENT_PROCESSING_OK) {
            return status;
        }
    }
    if (result < 0 && result != AVERROR_EOF) {
        return TURBO_CLIENT_PROCESSING_ECODEC;
    }
    return turbo_client_processing_write_encoded_packets(processing);
}

static turbo_client_processing_status_t
turbo_client_processing_transform_frame(
    turbo_client_processing_t *processing, AVFrame *decoded) {
    int scaled;

    if (processing->sws == NULL) {
        processing->source_width = decoded->width;
        processing->source_height = decoded->height;
        processing->source_format = (enum AVPixelFormat)decoded->format;
        processing->sws = sws_getContext(
            decoded->width, decoded->height,
            (enum AVPixelFormat)decoded->format,
            processing->encoder->width, processing->encoder->height,
            processing->encoder->pix_fmt,
            SWS_BILINEAR, NULL, NULL, NULL);
        if (processing->sws == NULL) {
            return TURBO_CLIENT_PROCESSING_EUNSUPPORTED;
        }
    } else if (processing->source_width != decoded->width ||
               processing->source_height != decoded->height ||
               processing->source_format !=
                   (enum AVPixelFormat)decoded->format) {
        return TURBO_CLIENT_PROCESSING_EUNSUPPORTED;
    }

    if (av_frame_make_writable(processing->converted_frame) < 0) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    scaled = sws_scale(
        processing->sws,
        (const uint8_t *const *)decoded->data, decoded->linesize,
        0, decoded->height,
        processing->converted_frame->data,
        processing->converted_frame->linesize);
    if (scaled != processing->encoder->height) {
        return TURBO_CLIENT_PROCESSING_EIO;
    }

    if (decoded->best_effort_timestamp != AV_NOPTS_VALUE) {
        processing->converted_frame->pts = av_rescale_q(
            decoded->best_effort_timestamp,
            processing->input_stream->time_base,
            processing->encoder->time_base);
        if (processing->converted_frame->pts < processing->next_pts) {
            processing->converted_frame->pts = processing->next_pts;
        }
    } else {
        processing->converted_frame->pts = processing->next_pts;
    }
    processing->next_pts = processing->converted_frame->pts + 1;
    salts_mutex_lock(&processing->frame_mutex);
    processing->admitted_frames++;
    salts_mutex_unlock(&processing->frame_mutex);

    return turbo_client_processing_encode_frame(
        processing, processing->converted_frame);
}

static turbo_client_processing_status_t
turbo_client_processing_receive_decoded(
    turbo_client_processing_t *processing) {
    turbo_client_processing_status_t status;
    int result;
    for (;;) {
        result = avcodec_receive_frame(
            processing->decoder, processing->decoded_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return TURBO_CLIENT_PROCESSING_OK;
        }
        if (result < 0) {
            return TURBO_CLIENT_PROCESSING_ECODEC;
        }
        status = turbo_client_processing_transform_frame(
            processing, processing->decoded_frame);
        av_frame_unref(processing->decoded_frame);
        if (status != TURBO_CLIENT_PROCESSING_OK) {
            return status;
        }
    }
}

static turbo_client_processing_status_t
turbo_client_processing_send_input_packet(
    turbo_client_processing_t *processing, const AVPacket *packet) {
    turbo_client_processing_status_t status;
    int result;
    for (;;) {
        result = avcodec_send_packet(processing->decoder, packet);
        if (result != AVERROR(EAGAIN)) {
            break;
        }
        status = turbo_client_processing_receive_decoded(processing);
        if (status != TURBO_CLIENT_PROCESSING_OK) {
            return status;
        }
    }
    if (result < 0 && result != AVERROR_EOF) {
        return TURBO_CLIENT_PROCESSING_ECODEC;
    }
    return turbo_client_processing_receive_decoded(processing);
}

void turbo_client_processing_config_init(
    turbo_client_processing_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->frame_queue_capacity =
        TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_CAPACITY;
    config->frame_queue_max_bytes =
        TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_BYTES;
    config->frame_queue_max_duration_us =
        TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_DURATION_US;
}

void turbo_client_processing_snapshot_init(
    turbo_client_processing_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
}

void turbo_client_processing_video_frame_info_init(
    turbo_client_processing_video_frame_info_t *info) {
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
}

void turbo_client_processing_audio_capture_config_init(
    turbo_client_processing_audio_capture_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->queue_capacity = 16u;
    config->queue_max_bytes = 1024u * 1024u;
    config->queue_max_duration_us = UINT64_C(500000);
}

void turbo_client_processing_audio_frame_info_init(
    turbo_client_processing_audio_frame_info_t *info) {
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
}

void turbo_client_processing_audio_snapshot_init(
    turbo_client_processing_audio_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
}

void turbo_client_processing_file_plan_init(
    turbo_client_processing_file_plan_t *plan) {
    if (plan == NULL) {
        return;
    }
    memset(plan, 0, sizeof(*plan));
    plan->size = sizeof(*plan);
}

turbo_client_processing_status_t turbo_client_processing_create(
    const turbo_client_processing_config_t *config,
    turbo_client_processing_t **out_processing) {
    turbo_client_processing_t *processing;

    if (out_processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    *out_processing = NULL;
    if (!turbo_client_processing_config_valid(config)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    processing = (turbo_client_processing_t *)calloc(1u, sizeof(*processing));
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->frame_slots = (turbo_client_processing_video_slot_t *)calloc(
        config->frame_queue_capacity, sizeof(*processing->frame_slots));
    processing->frame_storage = (uint8_t *)malloc(config->frame_queue_max_bytes);
    salts_mutex_init(&processing->frame_mutex);
    salts_mutex_init(&processing->audio_mutex);
    if (processing->frame_slots == NULL || processing->frame_storage == NULL ||
        processing->frame_mutex == NULL || processing->audio_mutex == NULL) {
        salts_mutex_destroy(&processing->audio_mutex);
        salts_mutex_destroy(&processing->frame_mutex);
        free(processing->frame_storage);
        free(processing->frame_slots);
        free(processing);
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->config = *config;
    atomic_init(&processing->state, TURBO_CLIENT_PROCESSING_CREATED);
    processing->video_stream_index = -1;
    processing->source_format = AV_PIX_FMT_NONE;
    *out_processing = processing;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_set_file_plan(
    turbo_client_processing_t *processing,
    const turbo_client_processing_file_plan_t *plan) {
    char *input_path;
    char *output_path;
    char *output_format;
    char *video_codec;

    if (processing == NULL || !turbo_client_processing_file_plan_valid(plan)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_CREATED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    input_path = turbo_client_processing_copy_string(plan->input_path);
    output_path = turbo_client_processing_copy_string(plan->output_path);
    output_format = turbo_client_processing_copy_string(plan->output_format);
    video_codec = turbo_client_processing_copy_string(plan->video_codec);
    if (input_path == NULL || output_path == NULL ||
        output_format == NULL || video_codec == NULL) {
        free(input_path);
        free(output_path);
        free(output_format);
        free(video_codec);
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }

    turbo_client_processing_file_plan_clear(processing);
    processing->input_path = input_path;
    processing->output_path = output_path;
    processing->output_format = output_format;
    processing->video_codec = video_codec;
    processing->output_width = plan->output_width;
    processing->output_height = plan->output_height;
    processing->frame_rate_num = plan->frame_rate_num;
    processing->frame_rate_den = plan->frame_rate_den;
    processing->bitrate = plan->bitrate;
    processing->file_plan_set = 1;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t
turbo_client_processing_set_audio_capture_config(
    turbo_client_processing_t *processing,
    const turbo_client_processing_audio_capture_config_t *config) {
    turbo_client_processing_audio_slot_t *slots;
    uint8_t *storage;

    if (processing == NULL || !turbo_client_processing_audio_config_valid(config)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_CREATED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    slots = (turbo_client_processing_audio_slot_t *)calloc(
        config->queue_capacity, sizeof(*slots));
    storage = (uint8_t *)malloc(config->queue_max_bytes);
    if (slots == NULL || storage == NULL) {
        free(storage);
        free(slots);
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }

    salts_mutex_lock(&processing->audio_mutex);
    free(processing->audio_slots);
    free(processing->audio_storage);
    processing->audio_slots = slots;
    processing->audio_storage = storage;
    processing->audio_config = *config;
    processing->audio_head = 0u;
    processing->audio_storage_tail = 0u;
    processing->queued_audio_frames = 0u;
    processing->queued_audio_bytes = 0u;
    processing->queued_audio_duration_us = 0u;
    processing->admitted_audio_frames = 0u;
    processing->rejected_audio_frames = 0u;
    processing->audio_configured = 1;
    salts_mutex_unlock(&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_prepare(
    turbo_client_processing_t *processing) {
    turbo_client_processing_status_t status;
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_CREATED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    if (processing->file_plan_set) {
        status = turbo_client_processing_file_prepare(processing);
        if (status != TURBO_CLIENT_PROCESSING_OK) {
            turbo_client_processing_file_runtime_clear(processing);
            turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_FAILED);
            return status;
        }
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_PREPARED);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_start(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PREPARED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_RUNNING);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_run_file(
    turbo_client_processing_t *processing) {
    turbo_client_processing_status_t status =
        TURBO_CLIENT_PROCESSING_OK;
    int result;

    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PREPARED ||
        !processing->file_plan_set || !processing->file_prepared) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_RUNNING);
    for (;;) {
        result = av_read_frame(
            processing->input, processing->input_packet);
        if (result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            status = TURBO_CLIENT_PROCESSING_EIO;
            goto failed;
        }
        if (processing->input_packet->stream_index ==
            processing->video_stream_index) {
            status = turbo_client_processing_send_input_packet(
                processing, processing->input_packet);
        }
        av_packet_unref(processing->input_packet);
        if (status != TURBO_CLIENT_PROCESSING_OK) {
            goto failed;
        }
    }

    status = turbo_client_processing_send_input_packet(processing, NULL);
    if (status != TURBO_CLIENT_PROCESSING_OK) {
        goto failed;
    }
    status = turbo_client_processing_encode_frame(processing, NULL);
    if (status != TURBO_CLIENT_PROCESSING_OK) {
        goto failed;
    }
    if (av_write_trailer(processing->output) < 0) {
        status = TURBO_CLIENT_PROCESSING_EIO;
        goto failed;
    }
    processing->output_header_written = 0;
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_STOPPED);
    turbo_client_processing_file_runtime_clear(processing);
    return TURBO_CLIENT_PROCESSING_OK;

failed:
    av_packet_unref(processing->input_packet);
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_FAILED);
    turbo_client_processing_file_runtime_clear(processing);
    return status;
}

turbo_client_processing_status_t
turbo_client_processing_admit_video_frame(
    turbo_client_processing_t *processing,
    const uint8_t *frame, size_t len,
    int width, int height, uint64_t timestamp_us) {
    turbo_client_processing_video_slot_t *slot;
    size_t slot_index;
    size_t write_offset;
    size_t storage_capacity;
    size_t storage_head = 0u;
    uint64_t oldest_timestamp = 0u;
    uint64_t newest_timestamp = 0u;
    uint64_t prospective_duration = 0u;

    if (processing == NULL || frame == NULL || len == 0u ||
        width <= 0 || height <= 0) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    salts_mutex_lock(&processing->frame_mutex);
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_RUNNING) {
        processing->rejected_frames++;
        salts_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    if (processing->queued_frames >= processing->config.frame_queue_capacity ||
        len > processing->config.frame_queue_max_bytes -
                  processing->queued_bytes) {
        processing->rejected_frames++;
        salts_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    if (processing->queued_frames != 0u) {
        size_t newest_index =
            (processing->frame_head + processing->queued_frames - 1u) %
            processing->config.frame_queue_capacity;
        storage_head =
            processing->frame_slots[processing->frame_head].offset;
        oldest_timestamp =
            processing->frame_slots[processing->frame_head].timestamp_us;
        newest_timestamp =
            processing->frame_slots[newest_index].timestamp_us;
        if (timestamp_us < newest_timestamp) {
            processing->rejected_frames++;
            salts_mutex_unlock(&processing->frame_mutex);
            return TURBO_CLIENT_PROCESSING_EINVAL;
        }
        prospective_duration = timestamp_us - oldest_timestamp;
        if (prospective_duration >
            processing->config.frame_queue_max_duration_us) {
            processing->rejected_frames++;
            salts_mutex_unlock(&processing->frame_mutex);
            return TURBO_CLIENT_PROCESSING_EFULL;
        }
    }

    storage_capacity = processing->config.frame_queue_max_bytes;
    write_offset = processing->frame_storage_tail;
    if (processing->queued_frames == 0u) {
        write_offset = 0u;
    } else if (processing->frame_storage_tail >= storage_head) {
        size_t tail_space = storage_capacity - processing->frame_storage_tail;
        if (len > tail_space) {
            if (len > storage_head) {
                processing->rejected_frames++;
                salts_mutex_unlock(&processing->frame_mutex);
                return TURBO_CLIENT_PROCESSING_EFULL;
            }
            write_offset = 0u;
        }
    } else if (len > storage_head - processing->frame_storage_tail) {
        processing->rejected_frames++;
        salts_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    memcpy(processing->frame_storage + write_offset, frame, len);
    slot_index =
        (processing->frame_head + processing->queued_frames) %
        processing->config.frame_queue_capacity;
    slot = &processing->frame_slots[slot_index];
    slot->offset = write_offset;
    slot->size = len;
    slot->width = width;
    slot->height = height;
    slot->timestamp_us = timestamp_us;

    processing->frame_storage_tail = write_offset + len;
    if (processing->frame_storage_tail == storage_capacity) {
        processing->frame_storage_tail = 0u;
    }
    processing->queued_frames++;
    processing->queued_bytes += len;
    processing->queued_duration_us = prospective_duration;
    processing->admitted_frames++;
    salts_mutex_unlock(&processing->frame_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

void turbo_client_processing_video_capture_callback(
    salts_capture_t *capture,
    const uint8_t *frame, size_t len,
    int width, int height,
    uint64_t timestamp_us, void *user_data) {
    (void)capture;
    (void)turbo_client_processing_admit_video_frame(
        (turbo_client_processing_t *)user_data,
        frame, len, width, height, timestamp_us);
}

turbo_client_processing_status_t turbo_client_processing_pop_video_frame(
    turbo_client_processing_t *processing,
    void *destination, size_t destination_capacity,
    size_t *out_size,
    turbo_client_processing_video_frame_info_t *info) {
    turbo_client_processing_video_slot_t *slot;

    if (processing == NULL || out_size == NULL || info == NULL ||
        info->size != sizeof(*info)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    salts_mutex_lock(&processing->frame_mutex);
    if (processing->queued_frames == 0u) {
        *out_size = 0u;
        salts_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    slot = &processing->frame_slots[processing->frame_head];
    *out_size = slot->size;
    if (destination == NULL || destination_capacity < slot->size) {
        salts_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    memcpy(destination, processing->frame_storage + slot->offset, slot->size);
    info->data_size = slot->size;
    info->width = slot->width;
    info->height = slot->height;
    info->timestamp_us = slot->timestamp_us;

    processing->queued_bytes -= slot->size;
    memset(slot, 0, sizeof(*slot));
    processing->frame_head =
        (processing->frame_head + 1u) %
        processing->config.frame_queue_capacity;
    processing->queued_frames--;
    if (processing->queued_frames == 0u) {
        processing->frame_head = 0u;
        processing->frame_storage_tail = 0u;
    }
    turbo_client_processing_queue_recompute_duration_locked(processing);
    salts_mutex_unlock(&processing->frame_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t
turbo_client_processing_admit_audio_frame(
    turbo_client_processing_t *processing,
    const uint8_t *samples, size_t len, uint64_t timestamp_us) {
    turbo_client_processing_audio_slot_t *slot;
    size_t slot_index;
    size_t write_offset;
    size_t storage_capacity;
    size_t storage_head = 0u;
    size_t frame_bytes;
    uint64_t oldest_timestamp = 0u;
    uint64_t newest_timestamp = 0u;
    uint64_t prospective_duration = 0u;

    if (processing == NULL || samples == NULL || len == 0u) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    salts_mutex_lock(&processing->audio_mutex);
    if (!processing->audio_configured) {
        processing->rejected_audio_frames++;
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    frame_bytes =
        turbo_client_processing_audio_frame_bytes(&processing->audio_config);
    if (frame_bytes == 0u || len % frame_bytes != 0u) {
        processing->rejected_audio_frames++;
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_RUNNING) {
        processing->rejected_audio_frames++;
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    if (processing->queued_audio_frames >=
            processing->audio_config.queue_capacity ||
        len > processing->audio_config.queue_max_bytes -
                  processing->queued_audio_bytes) {
        processing->rejected_audio_frames++;
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    if (processing->queued_audio_frames != 0u) {
        size_t newest_index =
            (processing->audio_head + processing->queued_audio_frames - 1u) %
            processing->audio_config.queue_capacity;
        storage_head =
            processing->audio_slots[processing->audio_head].offset;
        oldest_timestamp =
            processing->audio_slots[processing->audio_head].timestamp_us;
        newest_timestamp =
            processing->audio_slots[newest_index].timestamp_us;
        if (timestamp_us < newest_timestamp) {
            processing->rejected_audio_frames++;
            salts_mutex_unlock(&processing->audio_mutex);
            return TURBO_CLIENT_PROCESSING_EINVAL;
        }
        prospective_duration = timestamp_us - oldest_timestamp;
        if (prospective_duration >
            processing->audio_config.queue_max_duration_us) {
            processing->rejected_audio_frames++;
            salts_mutex_unlock(&processing->audio_mutex);
            return TURBO_CLIENT_PROCESSING_EFULL;
        }
    }

    storage_capacity = processing->audio_config.queue_max_bytes;
    write_offset = processing->audio_storage_tail;
    if (processing->queued_audio_frames == 0u) {
        write_offset = 0u;
    } else if (processing->audio_storage_tail >= storage_head) {
        size_t tail_space = storage_capacity - processing->audio_storage_tail;
        if (len > tail_space) {
            if (len > storage_head) {
                processing->rejected_audio_frames++;
                salts_mutex_unlock(&processing->audio_mutex);
                return TURBO_CLIENT_PROCESSING_EFULL;
            }
            write_offset = 0u;
        }
    } else if (len > storage_head - processing->audio_storage_tail) {
        processing->rejected_audio_frames++;
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    memcpy(processing->audio_storage + write_offset, samples, len);
    slot_index =
        (processing->audio_head + processing->queued_audio_frames) %
        processing->audio_config.queue_capacity;
    slot = &processing->audio_slots[slot_index];
    slot->offset = write_offset;
    slot->size = len;
    slot->timestamp_us = timestamp_us;

    processing->audio_storage_tail = write_offset + len;
    if (processing->audio_storage_tail == storage_capacity) {
        processing->audio_storage_tail = 0u;
    }
    processing->queued_audio_frames++;
    processing->queued_audio_bytes += len;
    processing->queued_audio_duration_us = prospective_duration;
    processing->admitted_audio_frames++;
    salts_mutex_unlock(&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

void turbo_client_processing_audio_capture_callback(
    salts_capture_t *capture,
    const uint8_t *samples, size_t len,
    uint64_t timestamp_us, void *user_data) {
    (void)capture;
    (void)turbo_client_processing_admit_audio_frame(
        (turbo_client_processing_t *)user_data,
        samples, len, timestamp_us);
}

turbo_client_processing_status_t turbo_client_processing_pop_audio_frame(
    turbo_client_processing_t *processing,
    void *destination, size_t destination_capacity,
    size_t *out_size,
    turbo_client_processing_audio_frame_info_t *info) {
    turbo_client_processing_audio_slot_t *slot;

    if (processing == NULL || out_size == NULL || info == NULL ||
        info->size != sizeof(*info)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    salts_mutex_lock(&processing->audio_mutex);
    if (!processing->audio_configured || processing->queued_audio_frames == 0u) {
        *out_size = 0u;
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    slot = &processing->audio_slots[processing->audio_head];
    *out_size = slot->size;
    if (destination == NULL || destination_capacity < slot->size) {
        salts_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    memcpy(destination, processing->audio_storage + slot->offset, slot->size);
    info->data_size = slot->size;
    info->timestamp_us = slot->timestamp_us;
    info->sample_rate = processing->audio_config.sample_rate;
    info->channels = processing->audio_config.channels;
    info->bits_per_sample = processing->audio_config.bits_per_sample;

    processing->queued_audio_bytes -= slot->size;
    memset(slot, 0, sizeof(*slot));
    processing->audio_head =
        (processing->audio_head + 1u) %
        processing->audio_config.queue_capacity;
    processing->queued_audio_frames--;
    if (processing->queued_audio_frames == 0u) {
        processing->audio_head = 0u;
        processing->audio_storage_tail = 0u;
    }
    turbo_client_processing_audio_recompute_duration_locked(processing);
    salts_mutex_unlock(&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_audio_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_audio_snapshot_t *snapshot) {
    if (processing == NULL || snapshot == NULL ||
        snapshot->size != sizeof(*snapshot)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    salts_mutex_lock((salts_mutex_t *)&processing->audio_mutex);
    if (!processing->audio_configured) {
        salts_mutex_unlock((salts_mutex_t *)&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    snapshot->queue_capacity = processing->audio_config.queue_capacity;
    snapshot->queue_max_bytes = processing->audio_config.queue_max_bytes;
    snapshot->queue_max_duration_us =
        processing->audio_config.queue_max_duration_us;
    snapshot->queued_frames = processing->queued_audio_frames;
    snapshot->queued_bytes = processing->queued_audio_bytes;
    snapshot->queued_duration_us = processing->queued_audio_duration_us;
    snapshot->admitted_frames = processing->admitted_audio_frames;
    snapshot->rejected_frames = processing->rejected_audio_frames;
    salts_mutex_unlock((salts_mutex_t *)&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_write_playback(
    turbo_client_processing_t *processing,
    salts_playback_t *playback,
    const void *samples, size_t len,
    size_t *out_written) {
    int status;

    if (out_written != NULL) {
        *out_written = 0u;
    }
    if (processing == NULL || playback == NULL || samples == NULL ||
        len == 0u || out_written == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_RUNNING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    status = salts_playback_write(playback, samples, len, out_written);
    if (status == SALTS_PLAYBACK_ERR_FORMAT) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (status == SALTS_PLAYBACK_ERR_BUSY) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    if (status == SALTS_PLAYBACK_ERR_DEVICE) {
        return TURBO_CLIENT_PROCESSING_EIO;
    }
    if (status != SALTS_PLAYBACK_OK) {
        return TURBO_CLIENT_PROCESSING_EIO;
    }
    return *out_written == len
               ? TURBO_CLIENT_PROCESSING_OK
               : TURBO_CLIENT_PROCESSING_EFULL;
}

turbo_client_processing_status_t turbo_client_processing_pause(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_RUNNING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_PAUSED);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_resume(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PAUSED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_RUNNING);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_request_stop(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PREPARED &&
        turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_RUNNING &&
        turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PAUSED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_DRAINING);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_drain(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_DRAINING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    salts_mutex_lock(&processing->frame_mutex);
    turbo_client_processing_queue_clear_locked(processing);
    salts_mutex_unlock(&processing->frame_mutex);
    salts_mutex_lock(&processing->audio_mutex);
    turbo_client_processing_audio_clear_locked(processing);
    salts_mutex_unlock(&processing->audio_mutex);
    turbo_client_processing_file_runtime_clear(processing);
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_STOPPED);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_snapshot_t *snapshot) {
    if (processing == NULL || snapshot == NULL ||
        snapshot->size != sizeof(*snapshot)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    snapshot->state = turbo_client_processing_state_get(processing);
    snapshot->frame_queue_capacity = processing->config.frame_queue_capacity;
    snapshot->frame_queue_max_bytes =
        processing->config.frame_queue_max_bytes;
    snapshot->frame_queue_max_duration_us =
        processing->config.frame_queue_max_duration_us;
    salts_mutex_lock((salts_mutex_t *)&processing->frame_mutex);
    snapshot->queued_frames = processing->queued_frames;
    snapshot->queued_bytes = processing->queued_bytes;
    snapshot->queued_duration_us = processing->queued_duration_us;
    snapshot->admitted_frames = processing->admitted_frames;
    snapshot->rejected_frames = processing->rejected_frames;
    salts_mutex_unlock((salts_mutex_t *)&processing->frame_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_destroy(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) == TURBO_CLIENT_PROCESSING_RUNNING ||
        turbo_client_processing_state_get(processing) == TURBO_CLIENT_PROCESSING_PAUSED ||
        turbo_client_processing_state_get(processing) == TURBO_CLIENT_PROCESSING_DRAINING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_file_runtime_clear(processing);
    turbo_client_processing_file_plan_clear(processing);
    salts_mutex_lock(&processing->frame_mutex);
    turbo_client_processing_queue_clear_locked(processing);
    salts_mutex_unlock(&processing->frame_mutex);
    salts_mutex_destroy(&processing->frame_mutex);
    salts_mutex_lock(&processing->audio_mutex);
    turbo_client_processing_audio_clear_locked(processing);
    salts_mutex_unlock(&processing->audio_mutex);
    salts_mutex_destroy(&processing->audio_mutex);
    free(processing->audio_storage);
    free(processing->audio_slots);
    free(processing->frame_storage);
    free(processing->frame_slots);
    free(processing);
    return TURBO_CLIENT_PROCESSING_OK;
}
