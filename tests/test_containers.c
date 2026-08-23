#include <tinytest.h>
#include <turbo_demuxer.h>
#include <turbo_muxer.h>

#include <stdint.h>
#include <string.h>

enum {
    CONTAINER_TEST_SAMPLE_RATE = 8000,
    CONTAINER_TEST_CHANNELS = 1,
    CONTAINER_TEST_FRAME_BYTES = 160
};

static void fill_audio_frame(uint8_t *frame, size_t size) {
    for (size_t i = 0; i < size; ++i) frame[i] = (uint8_t)(i ^ 0x5aU);
}

static void run_mpeg2_round_trip(turbo_muxer_format_t format,
                                 const char *demuxer_name) {
    static const uint8_t mpeg2_sequence_and_picture[] = {
        0x00, 0x00, 0x01, 0xb3, 0x16, 0x01, 0x20, 0x13,
        0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0xb5,
        0x14, 0x8a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0xb8,
        0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x0f, 0xff, 0xf8, 0x00, 0x00, 0x01, 0x01,
        0x13, 0x94, 0x20, 0x00,
    };
    turbo_muxer_config_t mux_config = {0};
    turbo_stream_info_t input_info = {0};
    turbo_muxer_packet_t input_packet = {0};
    turbo_demuxer_config_t demux_config = {0};
    turbo_stream_info_t output_info = {0};
    turbo_demuxer_packet_t output_packet = {0};
    turbo_muxer_t *muxer = NULL;
    turbo_demuxer_t *demuxer = NULL;
    const turbo_demuxer_ops_t *probed_ops;
    uint8_t *container_data = NULL;
    size_t container_size = 0;
    int stream_id = -1;
    int result;
    int i;

    mux_config.format = format;
    mux_config.write_duration = 1;
    muxer = turbo_muxer_create(&mux_config);
    check_not_null(muxer);
    if (!muxer) goto cleanup;

    input_info.type = TURBO_CODEC_TYPE_VIDEO;
    input_info.codec_name = "mpeg2video";
    input_info.width = 352;
    input_info.height = 288;
    input_info.framerate = 25;
    result = turbo_muxer_add_stream(muxer, &input_info, &stream_id);
    check_equal(result, 0);
    if (result != 0) goto cleanup;

    input_packet.stream_id = stream_id;
    input_packet.data = mpeg2_sequence_and_picture;
    input_packet.size = sizeof(mpeg2_sequence_and_picture);
    input_packet.duration = 40000;
    input_packet.is_keyframe = 1;
    for (i = 0; i < 3; ++i) {
        input_packet.pts = (int64_t)i * input_packet.duration;
        input_packet.dts = input_packet.pts;
        result = turbo_muxer_write_packet(muxer, &input_packet);
        check_equal(result, 0);
        if (result != 0) goto cleanup;
    }
    result = turbo_muxer_write_trailer(muxer);
    check_equal(result, 0);
    if (result != 0) goto cleanup;
    result = turbo_muxer_get_data(muxer, &container_data, &container_size);
    check_equal(result, 0);
    check_not_null(container_data);
    check_greater(container_size, sizeof(mpeg2_sequence_and_picture));
    if (result != 0 || !container_data) goto cleanup;

    probed_ops = turbo_demuxer_probe(container_data, container_size);
    check_not_null(probed_ops);
    if (probed_ops) check_equal(probed_ops->name, demuxer_name);

    demux_config.data = container_data;
    demux_config.data_size = container_size;
    demuxer = turbo_demuxer_create_by_name(demuxer_name, &demux_config);
    check_not_null(demuxer);
    if (!demuxer) goto cleanup;
    result = turbo_demuxer_open(demuxer);
    check_equal(result, 0);
    if (result != 0) goto cleanup;
    check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
    result = turbo_demuxer_get_stream_info(demuxer, 0, &output_info);
    check_equal(result, 0);
    check_equal(output_info.codec_name, "mpeg2video");
    check_equal(output_info.width, 352);
    check_equal(output_info.height, 288);
    result = turbo_demuxer_read_packet(demuxer, &output_packet);
    check_equal(result, 1);
    check_not_null(output_packet.data);
    check_greater(output_packet.size, 0);
    check_equal(output_packet.stream_index, 0);

cleanup:
    turbo_demuxer_free_packet(&output_packet);
    turbo_demuxer_destroy(demuxer);
    turbo_muxer_destroy(muxer);
}

suite("container adapters") {
    before_each() {
        turbo_muxer_registry_init();
        turbo_demuxer_registry_init();
    }

    after_each() {
        turbo_demuxer_registry_shutdown();
        turbo_muxer_registry_shutdown();
    }

    group("registry") {
        it("registers all container adapters unconditionally") {
            check_not_null(turbo_muxer_find_by_format(TURBO_MUXER_FLV));
            check_not_null(turbo_muxer_find_by_format(TURBO_MUXER_MP4));
            check_not_null(turbo_muxer_find_by_format(TURBO_MUXER_MKV));
            check_not_null(turbo_muxer_find_by_format(TURBO_MUXER_WEBM));
            check_not_null(turbo_muxer_find_by_format(TURBO_MUXER_MPEG_TS));
            check_not_null(turbo_muxer_find_by_format(TURBO_MUXER_MPEG_PS));
            check_not_null(turbo_demuxer_find_by_name("flv"));
            check_not_null(turbo_demuxer_find_by_name("mp4"));
            check_not_null(turbo_demuxer_find_by_name("mkv"));
            check_not_null(turbo_demuxer_find_by_name("webm"));
            check_not_null(turbo_demuxer_find_by_name("mpegts"));
            check_not_null(turbo_demuxer_find_by_name("mpegps"));
        }
    }

    group("FLV") {
        it("round trips an in-memory PCMA packet with a complete FLV envelope") {
            turbo_muxer_config_t mux_config = {0};
            turbo_stream_info_t input_info = {0};
            turbo_muxer_packet_t input_packet = {0};
            turbo_demuxer_config_t demux_config = {0};
            turbo_stream_info_t output_info = {0};
            turbo_demuxer_packet_t output_packet = {0};
            turbo_muxer_t *muxer = NULL;
            turbo_demuxer_t *demuxer = NULL;
            uint8_t input[CONTAINER_TEST_FRAME_BYTES];
            uint8_t *container_data = NULL;
            size_t container_size = 0;
            int stream_id = -1;
            int result;

            fill_audio_frame(input, sizeof(input));
            mux_config.format = TURBO_MUXER_FLV;
            muxer = turbo_muxer_create(&mux_config);
            check_not_null(muxer);
            if (!muxer) goto flv_cleanup;
            input_info.type = TURBO_CODEC_TYPE_AUDIO;
            input_info.codec_name = "pcma";
            input_info.sample_rate = CONTAINER_TEST_SAMPLE_RATE;
            input_info.channels = CONTAINER_TEST_CHANNELS;
            result = turbo_muxer_add_stream(muxer, &input_info, &stream_id);
            check_equal(result, 0);
            if (result != 0) goto flv_cleanup;
            input_packet.stream_id = stream_id;
            input_packet.data = input;
            input_packet.size = sizeof(input);
            input_packet.duration = 20000;
            result = turbo_muxer_write_packet(muxer, &input_packet);
            check_equal(result, 0);
            if (result != 0) goto flv_cleanup;
            result = turbo_muxer_write_trailer(muxer);
            check_equal(result, 0);
            if (result != 0) goto flv_cleanup;
            result = turbo_muxer_get_data(muxer, &container_data, &container_size);
            check_equal(result, 0);
            check_greater(container_size, sizeof(input));
            if (result != 0 || !container_data || container_size < 5)
                goto flv_cleanup;
            check_equal(container_data, "FLV", 3);
            check_equal(container_data[4], 4);

            demux_config.data = container_data;
            demux_config.data_size = container_size;
            demuxer = turbo_demuxer_create(&demux_config);
            check_not_null(demuxer);
            if (!demuxer) goto flv_cleanup;
            result = turbo_demuxer_open(demuxer);
            check_equal(result, 0);
            if (result != 0) goto flv_cleanup;
            check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
            result = turbo_demuxer_get_stream_info(demuxer, 0, &output_info);
            check_equal(result, 0);
            check_equal(output_info.codec_name, "pcma");
            result = turbo_demuxer_read_packet(demuxer, &output_packet);
            check_equal(result, 1);
            check_equal(output_packet.size, sizeof(input));
            if (result == 1 && output_packet.data)
                check_equal(output_packet.data, input, sizeof(input));

        flv_cleanup:
            turbo_demuxer_free_packet(&output_packet);
            turbo_demuxer_destroy(demuxer);
            turbo_muxer_destroy(muxer);
        }
    }

    group("MP4") {
        it("round trips an in-memory PCMU packet") {
            turbo_muxer_config_t mux_config = {0};
            turbo_stream_info_t input_info = {0};
            turbo_muxer_packet_t input_packet = {0};
            turbo_demuxer_config_t demux_config = {0};
            turbo_stream_info_t output_info = {0};
            turbo_demuxer_packet_t output_packet = {0};
            turbo_muxer_t *muxer = NULL;
            turbo_demuxer_t *demuxer = NULL;
            uint8_t input[CONTAINER_TEST_FRAME_BYTES];
            uint8_t *container_data = NULL;
            size_t container_size = 0;
            int stream_id = -1;
            int result;

            fill_audio_frame(input, sizeof(input));
            mux_config.format = TURBO_MUXER_MP4;
            mux_config.faststart = 1;
            mux_config.write_duration = 1;
            muxer = turbo_muxer_create(&mux_config);
            check_not_null(muxer);
            if (!muxer) goto mp4_cleanup;

            input_info.type = TURBO_CODEC_TYPE_AUDIO;
            input_info.codec_name = "pcmu";
            input_info.sample_rate = CONTAINER_TEST_SAMPLE_RATE;
            input_info.channels = CONTAINER_TEST_CHANNELS;
            result = turbo_muxer_add_stream(muxer, &input_info, &stream_id);
            check_equal(result, 0);
            if (result != 0) goto mp4_cleanup;

            input_packet.stream_id = stream_id;
            input_packet.data = input;
            input_packet.size = sizeof(input);
            input_packet.duration = 20000;
            input_packet.is_keyframe = 1;
            result = turbo_muxer_write_packet(muxer, &input_packet);
            check_equal(result, 0);
            if (result != 0) goto mp4_cleanup;
            result = turbo_muxer_write_trailer(muxer);
            check_equal(result, 0);
            if (result != 0) goto mp4_cleanup;
            result = turbo_muxer_get_data(muxer, &container_data, &container_size);
            check_equal(result, 0);
            check_not_null(container_data);
            check_greater(container_size, sizeof(input));
            if (result != 0 || !container_data) goto mp4_cleanup;

            demux_config.data = container_data;
            demux_config.data_size = container_size;
            demuxer = turbo_demuxer_create(&demux_config);
            check_not_null(demuxer);
            if (!demuxer) goto mp4_cleanup;
            result = turbo_demuxer_open(demuxer);
            check_equal(result, 0);
            if (result != 0) goto mp4_cleanup;
            check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
            result = turbo_demuxer_get_stream_info(demuxer, 0, &output_info);
            check_equal(result, 0);
            check_equal(output_info.codec_name, "pcmu");
            check_equal(output_info.sample_rate, CONTAINER_TEST_SAMPLE_RATE);
            check_equal(output_info.channels, CONTAINER_TEST_CHANNELS);

            result = turbo_demuxer_read_packet(demuxer, &output_packet);
            check_equal(result, 1);
            check_equal(output_packet.size, sizeof(input));
            if (result == 1 && output_packet.data)
                check_equal(output_packet.data, input, sizeof(input));

        mp4_cleanup:
            turbo_demuxer_free_packet(&output_packet);
            turbo_demuxer_destroy(demuxer);
            turbo_muxer_destroy(muxer);
        }
    }

    group("MKV") {
        it("round trips an in-memory MP3 packet") {
            turbo_muxer_config_t mux_config = {0};
            turbo_stream_info_t input_info = {0};
            turbo_muxer_packet_t input_packet = {0};
            turbo_demuxer_config_t demux_config = {0};
            turbo_stream_info_t output_info = {0};
            turbo_demuxer_packet_t output_packet = {0};
            turbo_muxer_t *muxer = NULL;
            turbo_demuxer_t *demuxer = NULL;
            uint8_t input[CONTAINER_TEST_FRAME_BYTES];
            uint8_t *container_data = NULL;
            size_t container_size = 0;
            int stream_id = -1;
            int result;

            fill_audio_frame(input, sizeof(input));
            mux_config.format = TURBO_MUXER_MKV;
            mux_config.write_duration = 1;
            muxer = turbo_muxer_create(&mux_config);
            check_not_null(muxer);
            if (!muxer) goto mkv_cleanup;

            input_info.type = TURBO_CODEC_TYPE_AUDIO;
            input_info.codec_name = "mp3";
            input_info.sample_rate = 44100;
            input_info.channels = 2;
            result = turbo_muxer_add_stream(muxer, &input_info, &stream_id);
            check_equal(result, 0);
            if (result != 0) goto mkv_cleanup;
            input_packet.stream_id = stream_id;
            input_packet.data = input;
            input_packet.size = sizeof(input);
            input_packet.duration = 26000;
            input_packet.is_keyframe = 1;
            result = turbo_muxer_write_packet(muxer, &input_packet);
            check_equal(result, 0);
            if (result != 0) goto mkv_cleanup;
            result = turbo_muxer_write_trailer(muxer);
            check_equal(result, 0);
            if (result != 0) goto mkv_cleanup;
            result = turbo_muxer_get_data(muxer, &container_data, &container_size);
            check_equal(result, 0);
            check_greater(container_size, sizeof(input));
            if (result != 0 || !container_data) goto mkv_cleanup;

            demux_config.data = container_data;
            demux_config.data_size = container_size;
            demuxer = turbo_demuxer_create(&demux_config);
            check_not_null(demuxer);
            if (!demuxer) goto mkv_cleanup;
            result = turbo_demuxer_open(demuxer);
            check_equal(result, 0);
            if (result != 0) goto mkv_cleanup;
            check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
            result = turbo_demuxer_get_stream_info(demuxer, 0, &output_info);
            check_equal(result, 0);
            check_equal(output_info.codec_name, "mp3");
            check_equal(output_info.sample_rate, 44100);
            check_equal(output_info.channels, 2);
            result = turbo_demuxer_read_packet(demuxer, &output_packet);
            check_equal(result, 1);
            check_equal(output_packet.size, sizeof(input));
            if (result == 1 && output_packet.data)
                check_equal(output_packet.data, input, sizeof(input));

        mkv_cleanup:
            turbo_demuxer_free_packet(&output_packet);
            turbo_demuxer_destroy(demuxer);
            turbo_muxer_destroy(muxer);
        }
    }

    group("MPEG") {
        it("round trips MPEG-2 video through MPEG-TS") {
            run_mpeg2_round_trip(TURBO_MUXER_MPEG_TS, "mpegts");
        }

        it("round trips MPEG-2 video through MPEG-PS") {
            run_mpeg2_round_trip(TURBO_MUXER_MPEG_PS, "mpegps");
        }
    }
}
