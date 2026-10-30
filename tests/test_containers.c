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

static int matroska_find_uint(const uint8_t *bytes, size_t count,
                              uint16_t element_id, uint64_t *value) {
    size_t i;
    for (i = 0; i + 4u <= count; ++i) {
        unsigned n, j;
        uint64_t parsed = 0;
        if (bytes[i] != (uint8_t)(element_id >> 8) ||
            bytes[i + 1] != (uint8_t)element_id)
            continue;
        if ((bytes[i + 2] & 0xf8u) != 0x80u)
            continue;
        n = bytes[i + 2] & 0x7fu;
        if (n == 0 || n > 8 || n > count - i - 3u)
            continue;
        for (j = 0; j < n; ++j)
            parsed = (parsed << 8) | bytes[i + 3u + j];
        *value = parsed;
        return 1;
    }
    return 0;
}

/* Minimal EBML field inspection for the writer's Cue structural
 * contract. This is a test assertion, not a second media parser. */
static int matroska_find_single_id_uint(
    const uint8_t *data, size_t count, uint8_t id, uint64_t *out)
{
    size_t i, j;
    if (!data || !out) return 0;
    for (i = 0; i + 3u <= count; ++i) {
        uint64_t value = 0;
        size_t n;
        if (data[i] != id || (data[i + 1u] & 0x80u) == 0u)
            continue;
        n = (size_t)(data[i + 1u] & 0x7fu);
        if (!n || n > 8u || n > count - i - 2u)
            continue;
        for (j = 0; j < n; ++j)
            value = (value << 8) | data[i + 2u + j];
        *out = value;
        return 1;
    }
    return 0;
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

    group("Opus Matroska timing") {
        it("publishes actual OpusHead pre-skip as CodecDelay alongside SeekPreRoll") {
            /* An OpusHead with 312 samples pre-skip at 48kHz (6.5ms).
             * InputSampleRate is an informational 48kHz value in OpusHead;
             * CodecDelay must use the fixed 48k Opus decode timebase. */
            static const uint8_t opus_head[19] = {
                'O','p','u','s','H','e','a','d',
                1, 1, 0x38, 0x01, 0x80, 0xbb, 0, 0, 0, 0, 0
            };
            const uint8_t encoded_packet[4] = {0xf8u, 0xffu, 0xfeu, 0u};
            turbo_muxer_config_t config = {0};
            turbo_muxer_t *muxer;
            turbo_demuxer_t *demuxer = NULL;
            turbo_demuxer_config_t demux_config = {0};
            turbo_demuxer_track_timing_v1 timing =
                TURBO_DEMUXER_TRACK_TIMING_V1_INIT;
            turbo_stream_info_t audio = {0};
            turbo_muxer_packet_t packet = {0};
            uint8_t *output = NULL;
            size_t size = 0;
            uint64_t delay = 0, preroll = 0;
            int stream_id = -1;
            config.format = TURBO_MUXER_WEBM;
            muxer = turbo_muxer_create(&config);
            check_not_null(muxer);
            if (!muxer) return;
            audio.type = TURBO_CODEC_TYPE_AUDIO;
            audio.codec_name = "opus";
            audio.extradata = opus_head;
            audio.extradata_size = sizeof(opus_head);
            audio.sample_rate = 48000;
            audio.channels = 1;
            check_equal(turbo_muxer_add_stream(muxer, &audio, &stream_id), 0);
            packet.stream_id = stream_id;
            packet.data = encoded_packet;
            packet.size = sizeof(encoded_packet);
            packet.duration = 20000;
            packet.is_keyframe = 1;
            check_equal(turbo_muxer_write_packet(muxer, &packet), 0);
            check_equal(turbo_muxer_write_trailer(muxer), 0);
            check_equal(turbo_muxer_get_data(muxer, &output, &size), 0);
            check_true(matroska_find_uint(output, size, 0x56AA, &delay));
            check_true(matroska_find_uint(output, size, 0x56BB, &preroll));
            check_equal(delay, UINT64_C(6500000));
            check_equal(preroll, UINT64_C(80000000));

            /* Round trip the real TrackEntry through the public Demuxer.
             * This must fail if the reader silently discards 0x56AA/0x56BB. */
            demux_config.data = output;
            demux_config.data_size = size;
            demuxer = turbo_demuxer_create_by_name("webm", &demux_config);
            check_not_null(demuxer);
            if (demuxer) {
                check_equal(turbo_demuxer_open(demuxer), 0);
                check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
                check_equal(turbo_demuxer_get_track_timing(
                    demuxer, 0, &timing), 0);
                check_equal(timing.codec_delay_ns, delay);
                check_equal(timing.seek_preroll_ns, preroll);
                /* Audio-only WebM without Cues is not enough to prove a
                 * safe preroll seek. Fail closed; don't silently jump. */
                {
                    int64_t selected = INT64_C(-123);
                    check_true(turbo_demuxer_seek_opus_preroll(
                        demuxer, 0, 100, &selected) != 0);
                    check_equal(selected, INT64_C(-123));
                }
                {
                    turbo_demuxer_track_timing_v1 bad = timing;
                    bad.version++;
                    check_true(turbo_demuxer_get_track_timing(
                        demuxer, 0, &bad) != 0);
                    bad = timing;
                    bad.size--;
                    check_true(turbo_demuxer_get_track_timing(
                        demuxer, 0, &bad) != 0);
                }
                check_true(turbo_demuxer_get_track_timing(
                    demuxer, -1, &timing) != 0);
                check_true(turbo_demuxer_get_track_timing(
                    demuxer, 1, &timing) != 0);
            }
            turbo_demuxer_destroy(demuxer);
            turbo_muxer_destroy(muxer);
        }
        it("round trips a terminal Opus BlockGroup and positive DiscardPadding") {
            static const uint8_t opus_head[19] = {
                'O','p','u','s','H','e','a','d',
                1, 1, 0x38, 0x01, 0x80, 0xbb, 0, 0, 0, 0, 0
            };
            const uint8_t encoded_packet[4] = {0xf8u, 0xffu, 0xfeu, 0u};
            turbo_muxer_config_t config = {0};
            turbo_stream_info_t info = {0};
            turbo_muxer_packet_t packet = {0};
            turbo_demuxer_config_t dconfig = {0};
            turbo_demuxer_packet_t decoded = {0};
            turbo_demuxer_packet_timing_v1 timing =
                TURBO_DEMUXER_PACKET_TIMING_V1_INIT;
            turbo_muxer_t *muxer = NULL;
            turbo_demuxer_t *demuxer = NULL;
            uint8_t *container = NULL;
            size_t bytes = 0;
            uint64_t padding = 0;
            int track = -1, received = 0;
            config.format = TURBO_MUXER_WEBM;
            muxer = turbo_muxer_create(&config);
            check_not_null(muxer);
            if (!muxer) return;
            info.type = TURBO_CODEC_TYPE_AUDIO;
            info.codec_name = "opus";
            info.extradata = opus_head;
            info.extradata_size = sizeof(opus_head);
            info.sample_rate = 48000;
            info.channels = 1;
            check_equal(turbo_muxer_add_stream(muxer, &info, &track), 0);
            if (track < 0) goto opus_trim_cleanup;
            packet.stream_id = track;
            packet.data = encoded_packet;
            packet.size = sizeof(encoded_packet);
            packet.duration = 20000;
            packet.is_keyframe = 1;
            check_equal(turbo_muxer_write_packet(muxer, &packet), 0);
            packet.pts = packet.dts = 20000;
            check_equal(turbo_muxer_write_packet_with_discard_padding(
                muxer, &packet, UINT64_C(13500000)), 0);
            check_true(turbo_muxer_write_packet(muxer, &packet) != 0);
            check_equal(turbo_muxer_write_trailer(muxer), 0);
            check_equal(turbo_muxer_get_data(muxer, &container, &bytes), 0);
            check_true(matroska_find_uint(container, bytes, 0x75A2, &padding));
            check_equal(padding, UINT64_C(13500000));
            dconfig.data = container;
            dconfig.data_size = bytes;
            demuxer = turbo_demuxer_create_by_name("webm", &dconfig);
            check_not_null(demuxer);
            if (demuxer) {
                int read_status;
                check_equal(turbo_demuxer_open(demuxer), 0);
                {
                    turbo_demuxer_packet_timing_v1 bad = timing;
                    bad.version++;
                    /* Bad version must not consume the first packet. */
                    check_true(turbo_demuxer_read_packet_with_timing(
                        demuxer, &decoded, &bad) < 0);
                }
                while ((read_status = turbo_demuxer_read_packet_with_timing(
                            demuxer, &decoded, &timing)) == 1) {
                    check_equal(decoded.size, sizeof(encoded_packet));
                    check_equal(decoded.data, encoded_packet, sizeof(encoded_packet));
                    check_equal(timing.discard_padding_ns,
                                received == 0 ? INT64_C(0) :
                                INT64_C(13500000));
                    ++received;
                    turbo_demuxer_free_packet(&decoded);
                    if (received > 2) break;
                }
                check_equal(read_status, 0); /* real EOF, not parser error */
                check_equal(timing.discard_padding_ns, INT64_C(0));
                check_equal(received, 2);
            }
            turbo_demuxer_destroy(demuxer);
            demuxer = NULL;
            {
                /* Negative DiscardPadding is valid Matroska and trims from
                 * the BEGINNING; re-use the generated real container by
                 * changing only the signed integer payload to -1 ns. */
                size_t at;
                int changed = 0, read_status = 0;
                for (at = 0; at + 7u <= bytes; ++at) {
                    if (container[at] == 0x75u &&
                        container[at + 1u] == 0xA2u &&
                        container[at + 2u] == 0x84u) {
                        memset(container + at + 3u, 0xff, 4u);
                        changed = 1;
                        break;
                    }
                }
                check_true(changed);
                if (changed) {
                    dconfig.data = container;
                    demuxer = turbo_demuxer_create_by_name("webm", &dconfig);
                    check_not_null(demuxer);
                    if (demuxer) {
                        check_equal(turbo_demuxer_open(demuxer), 0);
                        received = 0;
                        while ((read_status =
                                turbo_demuxer_read_packet_with_timing(
                                    demuxer, &decoded, &timing)) == 1) {
                            check_equal(timing.discard_padding_ns,
                                received == 0 ? INT64_C(0) : INT64_C(-1));
                            ++received;
                            turbo_demuxer_free_packet(&decoded);
                            if (received > 2) break;
                        }
                        check_equal(read_status, 0);
                        check_equal(received, 2);
                    }
                }
            }
        opus_trim_cleanup:
            turbo_demuxer_free_packet(&decoded);
            turbo_demuxer_destroy(demuxer);
            turbo_muxer_destroy(muxer);
        }
        it("refuses an Opus track without a matching OpusHead and sample rate") {
            static const uint8_t opus_head[19] = {
                'O','p','u','s','H','e','a','d',
                1, 1, 0x38, 0x01, 0x80, 0xbb, 0, 0, 0, 0, 0
            };
            turbo_muxer_config_t config = {0};
            turbo_muxer_t *muxer;
            turbo_stream_info_t audio = {0};
            int stream_id = -1;
            config.format = TURBO_MUXER_WEBM;
            muxer = turbo_muxer_create(&config);
            check_not_null(muxer);
            if (!muxer) return;
            audio.type = TURBO_CODEC_TYPE_AUDIO;
            audio.codec_name = "opus";
            audio.sample_rate = 48000;
            audio.channels = 1;
            check_true(turbo_muxer_add_stream(muxer, &audio, &stream_id) != 0);
            audio.extradata = opus_head;
            audio.extradata_size = sizeof(opus_head);
            audio.sample_rate = 44100;
            check_true(turbo_muxer_add_stream(muxer, &audio, &stream_id) != 0);
            turbo_muxer_destroy(muxer);
        }
    }

    group("Opus seek/preroll cue admission") {
        it("never chooses a future cue across real WebM cluster boundaries") {
            static const uint8_t opus_head[19] = {
                'O','p','u','s','H','e','a','d',
                1, 1, 0x38, 0x01, 0x80, 0xbb, 0, 0, 0, 0, 0
            };
            const uint8_t vp8[5] = {0x10, 0, 0, 0, 0};
            const uint8_t opus[4] = {0xf8, 0xff, 0xfe, 0};
            turbo_muxer_config_t config = {0};
            turbo_stream_info_t stream = {0};
            turbo_muxer_packet_t input = {0};
            turbo_demuxer_config_t dc = {0};
            turbo_demuxer_packet_t output = {0};
            turbo_demuxer_packet_timing_v1 timing =
                TURBO_DEMUXER_PACKET_TIMING_V1_INIT;
            turbo_demuxer_track_timing_v1 track =
                TURBO_DEMUXER_TRACK_TIMING_V1_INIT;
            turbo_muxer_t *muxer = NULL;
            turbo_demuxer_t *demuxer = NULL;
            uint8_t *container = NULL;
            size_t size = 0;
            int video = -1, audio = -1, i;
            int64_t anchor = -77;

            config.format = TURBO_MUXER_WEBM;
            muxer = turbo_muxer_create(&config);
            check_not_null(muxer);
            if (!muxer) return;
            stream.type = TURBO_CODEC_TYPE_VIDEO;
            stream.codec_name = "vp8";
            stream.width = stream.height = 16;
            stream.framerate = 10;
            check_equal(turbo_muxer_add_stream(muxer, &stream, &video), 0);
            stream = (turbo_stream_info_t){0};
            stream.type = TURBO_CODEC_TYPE_AUDIO;
            stream.codec_name = "opus";
            stream.sample_rate = 48000;
            stream.channels = 1;
            stream.extradata = opus_head;
            stream.extradata_size = sizeof(opus_head);
            check_equal(turbo_muxer_add_stream(muxer, &stream, &audio), 0);
            if (video < 0 || audio < 0) goto cleanup;
            for (i = 0; i < 3; ++i) {
                input.stream_id = video;
                input.data = vp8;
                input.size = sizeof(vp8);
                input.pts = input.dts = (int64_t)i * INT64_C(6000000);
                input.duration = 6000000;
                input.is_keyframe = 1;
                check_equal(turbo_muxer_write_packet(muxer, &input), 0);
                input.stream_id = audio;
                input.data = opus;
                input.size = sizeof(opus);
                input.pts = input.dts =
                    (int64_t)i * INT64_C(6000000) + INT64_C(20000);
                input.duration = 20000;
                check_equal(turbo_muxer_write_packet(muxer, &input), 0);
            }
            check_equal(turbo_muxer_write_trailer(muxer), 0);
            check_equal(turbo_muxer_get_data(muxer, &container, &size), 0);
            if (!container || !size) goto cleanup;
            {
                /* Verify RFC 9559 byte origins in emitted EBML, not only
                 * that TurboMedia's Reader happens to find its own Cues. */
                static const uint8_t segment_id[4] = {
                    0x18, 0x53, 0x80, 0x67
                };
                static const uint8_t cues_id[4] = {
                    0x1c, 0x53, 0xbb, 0x6b
                };
                static const uint8_t cluster_id[4] = {
                    0x1f, 0x43, 0xb6, 0x75
                };
                size_t i, segment_start = SIZE_MAX, cues_at = SIZE_MAX;
                uint64_t cue_cluster = UINT64_MAX, cue_relative = UINT64_MAX;
                for (i = 0; i + 12u <= size; ++i) {
                    if (memcmp(container + i, segment_id, 4u) == 0 &&
                        container[i + 4u] == 0x01u) {
                        segment_start = i + 12u; /* ID4 + VINT8 */
                        break;
                    }
                }
                for (i = 0; i + 4u <= size; ++i)
                    if (memcmp(container + i, cues_id, 4u) == 0)
                        cues_at = i; /* last match is the real Cues */
                check_true(segment_start != SIZE_MAX);
                check_true(cues_at != SIZE_MAX);
                if (segment_start != SIZE_MAX && cues_at != SIZE_MAX) {
                    size_t cluster_at;
                    check_true(matroska_find_single_id_uint(
                        container + cues_at, size - cues_at,
                        0xF1, &cue_cluster));
                    check_true(matroska_find_single_id_uint(
                        container + cues_at, size - cues_at,
                        0xF0, &cue_relative));
                    check_true(cue_cluster <= size - segment_start - 4u);
                    if (cue_cluster <= size - segment_start - 4u) {
                        cluster_at = segment_start + (size_t)cue_cluster;
                        check_equal(container + cluster_at, cluster_id, 4u);
                        check_true(cue_relative <=
                                   size - cluster_at - 8u - 1u);
                        if (cue_relative <= size - cluster_at - 8u - 1u)
                            check_equal(container[cluster_at + 8u +
                                (size_t)cue_relative], 0xA3u);
                    }
                }
            }
            dc.data = container;
            dc.data_size = size;
            demuxer = turbo_demuxer_create_by_name("webm", &dc);
            check_not_null(demuxer);
            if (!demuxer) goto cleanup;
            check_equal(turbo_demuxer_open(demuxer), 0);
            check_equal(turbo_demuxer_get_stream_count(demuxer), 2);
            check_equal(turbo_demuxer_get_track_timing(
                demuxer, audio, &track), 0);
            check_equal(track.codec_delay_ns, UINT64_C(6500000));
            check_equal(track.seek_preroll_ns, UINT64_C(80000000));

            /* The legacy nearest-cue choice could select 12000ms here,
             * which is later than the requested 11980ms presentation time
             * and misses the mandatory 80ms Opus decoder warmup. */
            check_true(turbo_demuxer_seek_opus_preroll(
                demuxer, video, 11980, &anchor) != 0);
            check_true(turbo_demuxer_seek_opus_preroll(
                demuxer, audio, -1, &anchor) != 0);
            check_true(turbo_demuxer_seek_opus_preroll(
                demuxer, audio, 11980, NULL) != 0);
            check_equal(anchor, INT64_C(-77));
            check_equal(turbo_demuxer_seek_opus_preroll(
                demuxer, audio, 11980, &anchor), 0);
            check_equal(anchor, INT64_C(6000));
            check_equal(turbo_demuxer_read_packet_with_timing(
                demuxer, &output, &timing), 1);
            check_equal(output.stream_index, video);
            check_equal(output.pts, INT64_C(6000));
            check_equal(timing.discard_padding_ns, INT64_C(0));
            turbo_demuxer_free_packet(&output);

            check_equal(turbo_demuxer_seek_opus_preroll(
                demuxer, audio, 12120, &anchor), 0);
            check_equal(anchor, INT64_C(12000));
            check_equal(turbo_demuxer_read_packet(
                demuxer, &output), 1);
            check_equal(output.pts, INT64_C(12000));
            turbo_demuxer_free_packet(&output);

            /* The existing nearest-Cue API must remain usable after
             * switching the stored offsets to RFC segment-relative form. */
            check_equal(turbo_demuxer_seek(
                demuxer, INT64_C(12120), 0), 0);
            check_equal(turbo_demuxer_read_packet(
                demuxer, &output), 1);
            check_equal(output.pts, INT64_C(12000));
            turbo_demuxer_free_packet(&output);

            check_equal(turbo_demuxer_seek_opus_preroll(
                demuxer, audio, 50, &anchor), 0);
            check_equal(anchor, INT64_C(0));
            check_equal(turbo_demuxer_read_packet(
                demuxer, &output), 1);
            check_equal(output.pts, INT64_C(0));
        cleanup:
            turbo_demuxer_free_packet(&output);
            turbo_demuxer_destroy(demuxer);
            turbo_muxer_destroy(muxer);
        }
    }

    group("bounded WebM writer") {
        it("rejects an oversized VP8 packet before extending memory past its limit") {
            turbo_muxer_config_t config = {0};
            turbo_stream_info_t stream = {0};
            turbo_muxer_packet_t packet = {0};
            turbo_muxer_t *muxer;
            uint8_t frame[32 * 1024];
            uint8_t *result = NULL;
            size_t size = 0;
            int stream_id = -1;
            memset(frame, 0x5a, sizeof(frame));
            config.format = TURBO_MUXER_WEBM;
            muxer = turbo_muxer_create(&config);
            check_not_null(muxer);
            if (!muxer)
                return;
            check_true(turbo_muxer_set_memory_limit(muxer, 0) != 0);
            check_equal(turbo_muxer_set_memory_limit(muxer, 4096), 0);
            stream.type = TURBO_CODEC_TYPE_VIDEO;
            stream.codec_name = "vp8";
            stream.width = 16;
            stream.height = 16;
            stream.framerate = 10;
            check_equal(turbo_muxer_add_stream(muxer, &stream, &stream_id), 0);
            packet.stream_id = stream_id;
            packet.data = frame;
            packet.size = sizeof(frame);
            packet.is_keyframe = 1;
            packet.duration = 100000;
            check_true(turbo_muxer_write_packet(muxer, &packet) != 0);
            if (turbo_muxer_get_data(muxer, &result, &size) == 0)
                check_true(size <= 4096u);
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
