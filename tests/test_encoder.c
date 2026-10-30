#include <tinytest.h>
#include <turbo_media_encoder.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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
    if (kind == TM_ENCODER_IMAGE_PNG || kind == TM_ENCODER_IMAGE_JPEG) {
        int width = 0, height = 0, channels = 0;
        int valid = 0;
        uint8_t *pixels = stbi_load_from_memory(
            sink->bytes, (int)sink->count, &width, &height, &channels, 3);
        if (pixels && width == expected_width && height == expected_height) {
            valid = kind == TM_ENCODER_IMAGE_JPEG ||
                    memcmp(pixels, reference_rgb,
                           (size_t)width * height * 3u) == 0;
        }
        stbi_image_free(pixels);
        return valid;
    }
    if (kind == TM_ENCODER_IMAGE_WEBP) {
        const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_WEBP);
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
        accepted = frame->width == expected_width &&
                   frame->height == expected_height;
cleanup:
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return accepted;
    }
    return 0;
}

static int contains_bytes(const uint8_t *source, size_t source_size,
                          const void *fragment, size_t fragment_size) {
    size_t index;
    if (fragment_size > source_size) return 0;
    for (index = 0; index <= source_size - fragment_size; ++index)
        if (memcmp(source + index, fragment, fragment_size) == 0)
            return 1;
    return 0;
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

        it("embeds byte-exact PNG tEXt and JPEG COM parameters and remains decodable") {
            uint8_t pixels[8 * 8 * 3];
            image_sink sink = {0};
            tm_encoder_image_v1 options = source_image(pixels);
            size_t written = 0, i;
            const char *parameters = "seed: 42, prompt: hello";
            static const char png_tag[] = "tEXtparameters\0seed: 42, prompt: hello";
            static const char jpeg_tag[] = "parameters\0seed: 42, prompt: hello\0";
            for (i = 0; i < sizeof(pixels); ++i)
                pixels[i] = (uint8_t)(i * 17u + 3u);
            options.metadata_utf8 = parameters;
            options.metadata_bytes = strlen(parameters);
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_OK);
            check_true(sink.count == written);
            check_true(contains_bytes(sink.bytes, sink.count,
                                      png_tag, sizeof(png_tag) - 1u));
            check_true(decode_actual_dimensions(TM_ENCODER_IMAGE_PNG,
                                                &sink, 8, 8, pixels));
            options.max_output_bytes = written - 1u;
            memset(&sink, 0, sizeof(sink));
            written = 11u;
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_ECAPACITY);
            check_equal(written, 0u);

            options.max_output_bytes = 65536u;
            options.output_format = TM_ENCODER_IMAGE_JPEG;
            memset(&sink, 0, sizeof(sink));
            check_equal(tm_encoder_image_encode(&options, collect_encoded,
                                                &sink, &written), TM_ENCODER_OK);
            check_true(sink.count == written);
            check_true(sink.count > 8);
            check_equal(sink.bytes[2], 0xff);
            check_equal(sink.bytes[3], 0xfe);
            check_true(contains_bytes(sink.bytes, sink.count,
                                      jpeg_tag, sizeof(jpeg_tag) - 1u));
            check_true(decode_actual_dimensions(TM_ENCODER_IMAGE_JPEG,
                                                &sink, 8, 8, pixels));
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
            options.output_format = TM_ENCODER_IMAGE_WEBP;
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
