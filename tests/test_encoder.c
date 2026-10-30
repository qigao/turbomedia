#include <tinytest.h>
#include <turbo_media_encoder.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct image_sink {
    uint8_t bytes[65536];
    size_t count;
    int writes;
    int refuse;
} image_sink;

static int collect_encoded(void *opaque, const uint8_t *data, size_t size) {
    image_sink *sink = (image_sink *)opaque;
    if (!sink || !data || sink->refuse ||
        size > sizeof(sink->bytes) - sink->count)
        return -1;
    memcpy(sink->bytes + sink->count, data, size);
    sink->count += size;
    sink->writes++;
    return 0;
}

static tm_encoder_image_v1 source_image(uint8_t *bytes) {
    tm_encoder_image_v1 options = TM_ENCODER_IMAGE_V1_INIT;
    options.width = 8;
    options.height = 8;
    options.stride_bytes = 8 * 3;
    options.pixels_capacity = 8 * 8 * 3;
    options.pixels = bytes;
    options.max_output_bytes = 65536;
    return options;
}

static int decode_actual_dimensions(uint32_t kind, const image_sink *sink,
                                     int expected_width, int expected_height,
                                     const uint8_t *reference_rgb) {
    enum AVCodecID id = kind == TM_ENCODER_IMAGE_PNG ? AV_CODEC_ID_PNG :
                         kind == TM_ENCODER_IMAGE_JPEG ? AV_CODEC_ID_MJPEG :
                         AV_CODEC_ID_WEBP;
    const AVCodec *decoder = avcodec_find_decoder(id);
    AVCodecContext *context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int accepted = 0;
    if (!decoder) return 0;
    context = avcodec_alloc_context3(decoder);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!context || !packet || !frame ||
        avcodec_open2(context, decoder, NULL) < 0 ||
        av_new_packet(packet, (int)sink->count) < 0)
        goto cleanup;
    memcpy(packet->data, sink->bytes, sink->count);
    if (avcodec_send_packet(context, packet) < 0 ||
        avcodec_receive_frame(context, frame) < 0)
        goto cleanup;
    if (frame->width != expected_width || frame->height != expected_height)
        goto cleanup;
    if (kind == TM_ENCODER_IMAGE_PNG) {
        int row;
        if (frame->format != AV_PIX_FMT_RGB24 || !frame->data[0])
            goto cleanup;
        for (row = 0; row < expected_height; ++row) {
            if (memcmp(frame->data[0] + (size_t)row * frame->linesize[0],
                       reference_rgb + (size_t)row * expected_width * 3,
                       (size_t)expected_width * 3) != 0)
                goto cleanup;
        }
    }
    accepted = 1;
cleanup:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return accepted;
}

suite("TurboMedia C11 still-image encoder output contract") {
    group("raw RGB image to actual encoded PNG/JPEG") {
        it("produces decoded exact-size and PNG pixels with the selected codec") {
            uint8_t pixels[8 * 8 * 3];
            image_sink sink = {0};
            tm_encoder_image_v1 options = source_image(pixels);
            size_t written = 0;
            size_t i;
            for (i = 0; i < sizeof(pixels); ++i)
                pixels[i] = (uint8_t)(i * 17u + 3u);
            check_true(tm_encoder_image_available(TM_ENCODER_IMAGE_PNG));
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_OK);
            check_true(sink.writes > 0);
            check_equal(sink.count, written);
            check_true(sink.count >= 8);
            check_true(memcmp(sink.bytes, "\x89PNG\r\n\x1a\n", 8) == 0);
            check_true(decode_actual_dimensions(options.output_format, &sink,
                                                8, 8, pixels));

            options.output_format = TM_ENCODER_IMAGE_JPEG;
            memset(&sink, 0, sizeof(sink));
            written = 0;
            check_true(tm_encoder_image_available(TM_ENCODER_IMAGE_JPEG));
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_OK);
            check_equal(sink.count, written);
            check_true(sink.count > 4);
            check_equal(sink.bytes[0], 0xff);
            check_equal(sink.bytes[1], 0xd8);
            check_true(decode_actual_dimensions(options.output_format, &sink,
                                                8, 8, pixels));
        }

        it("fails over-capacity and failed sinks without publishing success") {
            uint8_t pixels[8 * 8 * 3] = {0};
            image_sink sink = {0};
            tm_encoder_image_v1 options = source_image(pixels);
            size_t written = 0;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_OK);
            check_true(written > 1);
            options.max_output_bytes = written - 1;
            memset(&sink, 0, sizeof(sink));
            written = 12;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written),
                        TM_ENCODER_ECAPACITY);
            check_equal(written, 0);
            options.max_output_bytes = sizeof(sink.bytes);
            sink.refuse = 1;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written),
                        TM_ENCODER_ESINK);
            check_equal(written, 0);
        }

        it("rejects invalid frame layout, version and unimplemented metadata") {
            uint8_t pixels[8 * 8 * 3] = {0};
            image_sink sink = {0};
            tm_encoder_image_v1 options = source_image(pixels);
            size_t written = 0;
            options.version = 2;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written),
                        TM_ENCODER_EVERSION);
            options.version = TM_ENCODER_IMAGE_V1;
            options.stride_bytes = 8;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_EINVAL);
            options.stride_bytes = 8 * 3;
            options.pixels_capacity = 2;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_EINVAL);
            options.pixels_capacity = sizeof(pixels);
            options.metadata_utf8 = "{}";
            options.metadata_bytes = 2;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written),
                        TM_ENCODER_EUNSUPPORTED);
            check_equal(sink.writes, 0);
        }

        it("exposes WebP only if the installed FFmpeg SDK has its real codec") {
            uint8_t pixels[8 * 8 * 3] = {0};
            image_sink sink = {0};
            tm_encoder_image_v1 options = source_image(pixels);
            size_t written = 0;
            tm_encoder_status outcome;
            options.output_format = TM_ENCODER_IMAGE_WEBP;
            outcome = tm_encoder_image_encode(&options, collect_encoded,
                                              &sink, &written);
            if (tm_encoder_image_available(TM_ENCODER_IMAGE_WEBP)) {
                check_equal(outcome, TM_ENCODER_OK);
                if (outcome == TM_ENCODER_OK) {
                    check_true(sink.count > 12);
                    check_true(memcmp(sink.bytes, "RIFF", 4) == 0);
                    check_true(memcmp(sink.bytes + 8, "WEBP", 4) == 0);
                    check_true(decode_actual_dimensions(options.output_format,
                                                        &sink, 8, 8, pixels));
                }
            } else {
                check_equal(outcome, TM_ENCODER_EUNSUPPORTED);
                check_equal(sink.count, 0);
            }
        }
    }
}
