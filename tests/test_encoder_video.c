#include <tinytest.h>
#include <turbo_media_encoder.h>
#include <turbo_demuxer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct video_result {
    uint8_t *bytes;
    size_t capacity, size;
    unsigned writes;
} video_result;

static int collect_video(void *user, const uint8_t *bytes, size_t size) {
    video_result *sink = (video_result *)user;
    if (!sink || !bytes || size > sink->capacity - sink->size)
        return -1;
    memcpy(sink->bytes + sink->size, bytes, size);
    sink->size += size;
    ++sink->writes;
    return 0;
}

static int cancel_now(void *user) {
    (void)user;
    return 1;
}

typedef struct cancel_after {
    unsigned calls;
    unsigned maximum;
} cancel_after;

static int cancel_after_frames(void *user) {
    cancel_after *state = (cancel_after *)user;
    ++state->calls;
    return state->calls > state->maximum;
}

static tm_encoder_video_v1 init_video(const tm_encoder_video_frame_v1 *frames,
                                      size_t frame_count) {
    tm_encoder_video_v1 options = TM_ENCODER_VIDEO_V1_INIT;
    options.width = 16;
    options.height = 16;
    options.fps = 10;
    options.bitrate = 250000;
    options.frames = frames;
    options.frame_count = frame_count;
    options.max_output_bytes = 1024 * 1024;
    return options;
}

static void prepare_frames(uint8_t pixels[][16 * 16 * 3],
                           tm_encoder_video_frame_v1 *frames,
                           size_t frame_count) {
    size_t frame, byte;
    for (frame = 0; frame < frame_count; ++frame) {
        for (byte = 0; byte < 16 * 16 * 3; ++byte)
            pixels[frame][byte] = (uint8_t)((byte + frame * 37u) & 0xffu);
        frames[frame] = (tm_encoder_video_frame_v1)
            TM_ENCODER_VIDEO_FRAME_V1_INIT;
        frames[frame].pixel_format = TM_ENCODER_PIXEL_RGB24;
        frames[frame].pixels = pixels[frame];
        frames[frame].stride_bytes = 16 * 3;
        frames[frame].pixels_capacity = sizeof(pixels[frame]);
    }
}

suite("TurboMedia Encoder VP8 WebM raw-frames contract") {
    group("exact TurboMedia codec and muxer") {
        it("encodes RGB input into native VP8 WebM with real demuxed frames") {
            uint8_t pixels[3][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[3];
            tm_encoder_video_v1 options;
            video_result sink = {0};
            turbo_demuxer_config_t demux_config = {0};
            turbo_demuxer_t *demuxer = NULL;
            turbo_stream_info_t stream = {0};
            turbo_demuxer_packet_t packet = {0};
            size_t written = 0;
            int packet_count = 0;
            int64_t last_pts = -1;
            prepare_frames(pixels, frames, 3);
            options = init_video(frames, 3);
            sink.capacity = options.max_output_bytes;
            sink.bytes = (uint8_t *)malloc(sink.capacity);
            check_not_null(sink.bytes);
            if (!sink.bytes) return;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_OK);
            check_equal(sink.size, written);
            check_equal(sink.writes, 1u);
            check_true(written > 32);
            if (written > 32) {
                static const uint8_t ebml[] = {0x1a, 0x45, 0xdf, 0xa3};
                check_true(memcmp(sink.bytes, ebml, sizeof(ebml)) == 0);
                turbo_demuxer_registry_init();
                demux_config.data = sink.bytes;
                demux_config.data_size = written;
                demuxer = turbo_demuxer_create_by_name("webm", &demux_config);
                check_not_null(demuxer);
                if (demuxer) {
                    check_equal(turbo_demuxer_open(demuxer), 0);
                    check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
                    check_equal(turbo_demuxer_get_stream_info(demuxer, 0, &stream), 0);
                    check_equal(stream.width, 16);
                    check_equal(stream.height, 16);
                    check_equal(stream.codec_name, "vp8");
                    while (turbo_demuxer_read_packet(demuxer, &packet) == 1) {
                        check_true(packet.size > 0);
                        check_true(packet.pts > last_pts);
                        last_pts = packet.pts;
                        ++packet_count;
                        turbo_demuxer_free_packet(&packet);
                        if (packet_count > 3)
                            break;
                    }
                    turbo_demuxer_free_packet(&packet);
                    check_equal(packet_count, 3);
                    turbo_demuxer_destroy(demuxer);
                }
            }
            free(sink.bytes);
        }

        it("rejects audio silently dropping, bad stride, and invalid ABI") {
            uint8_t pixels[1][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[1];
            tm_encoder_video_v1 options;
            video_result sink = {0};
            size_t written = 123;
            prepare_frames(pixels, frames, 1);
            options = init_video(frames, 1);
            options.audio_bytes = 4;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_EUNSUPPORTED);
            check_equal(written, 0u);
            options.audio_bytes = 0;
            options.version = 2;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_EVERSION);
            options.version = TM_ENCODER_VIDEO_V1;
            options.width = 15; /* VP8 I420 requires even dimensions. */
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_EINVAL);
            options.width = 16;
            frames[0].stride_bytes = 1;
            sink.bytes = (uint8_t *)malloc(1024 * 1024);
            sink.capacity = 1024 * 1024;
            check_not_null(sink.bytes);
            if (sink.bytes) {
                check_equal(tm_encoder_webm_encode(&options, collect_video,
                                                   &sink, NULL, NULL, &written),
                            TM_ENCODER_EINVAL);
                check_equal(sink.writes, 0u);
                free(sink.bytes);
            }
        }

        it("refuses undersized output and cancellation without emitting a container") {
            uint8_t pixels[3][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[3];
            tm_encoder_video_v1 options;
            video_result sink = {0};
            size_t written = 3u;
            cancel_after middle = {0u, 1u};
            prepare_frames(pixels, frames, 3);
            options = init_video(frames, 3);
            options.max_output_bytes = 1;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_ECAPACITY);
            check_equal(written, 0u);
            options.max_output_bytes = 1024 * 1024;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, cancel_now, NULL, &written),
                        TM_ENCODER_ECANCELLED);
            check_equal(sink.writes, 0u);
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, cancel_after_frames,
                                               &middle, &written),
                        TM_ENCODER_ECANCELLED);
            check_equal(sink.writes, 0u);
        }
    }
}
