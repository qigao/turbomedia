#include "internal/client_processing_internal.h"

#include <libavutil/avutil.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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

void turbo_client_processing_file_runtime_clear(
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

void turbo_client_processing_file_plan_clear(
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

turbo_client_processing_status_t turbo_client_processing_file_prepare(
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
    cmeta_mutex_lock(&processing->frame_mutex);
    processing->admitted_frames++;
    cmeta_mutex_unlock(&processing->frame_mutex);

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
