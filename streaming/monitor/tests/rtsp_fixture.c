#include <core/clock.h>
#include <core/fmt.h>
#include <file/fs.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <rtp-payload.h>
#include <threads/threads.h>
#include <turbo_codec.h>
#include <turbo_media_server.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { CAMERAS = 4, WIDTH = 32, HEIGHT = 32, FPS = 10, RTP_CLOCK = 90000 };
typedef struct camera {
    turbo_rtsp_client_t *client;
    void *packetizer;
} camera_t;

static void *alloc_packet(void *context, int size) {
    (void)context;
    return malloc((size_t)size);
}

static void free_packet(void *context, void *packet) {
    (void)context;
    free(packet);
}

static int send_packet(void *context, const void *packet, int size, uint32_t pts, int flags) {
    const camera_t *camera = context;
    (void)pts;
    (void)flags;
    return turbo_rtsp_client_send_interleaved_frame(camera->client, 0, packet, (size_t)size);
}

static bool connect_camera(camera_t *camera, int port, unsigned index) {
    static const char sdp[] =
        "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=monitor-fixture\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:trackID=0\r\n";
    struct rtp_payload_t callbacks = {alloc_packet, free_packet, send_packet};
    const turbo_rtsp_client_config_t config = {
        .host = "127.0.0.1", .port = port, .timeout_ms = 5000,
        .control_transport = TURBO_RTSP_CONTROL_TRANSPORT_TCP
    };
    char url[96], track_url[128];
    fmt(url, sizeof(url), "rtsp://127.0.0.1:{}/live/camera-{}", port, index);
    fmt(track_url, sizeof(track_url), "{}/trackID=0", url);
    const turbo_rtsp_push_announce_t announce = {.uri = url, .sdp = sdp, .sdp_len = sizeof(sdp)-1};
    const turbo_rtsp_push_track_t track = {.control_uri = track_url, .rtp_channel = 0, .rtcp_channel = 1};
    camera->client = turbo_rtsp_client_create(&config);
    if (!camera->client || turbo_rtsp_client_connect(camera->client) ||
        turbo_rtsp_client_announce(camera->client, &announce) ||
        turbo_rtsp_client_setup_interleaved(camera->client, &track) ||
        turbo_rtsp_client_record(camera->client)) return false;
    camera->packetizer = rtp_payload_encode_create(96, "H264", 1, 0x23450000 + index, &callbacks, camera);
    return camera->packetizer != NULL;
}

static int probe(const char *path) {
    AVFormatContext *input = NULL;
    AVCodecContext *decoder = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const AVCodec *codec = NULL;
    int index, frames = 0;
    if (!packet || !frame || avformat_open_input(&input, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(input, NULL) < 0 || input->nb_streams != 1) goto done;
    index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0 || codec->id != AV_CODEC_ID_H264) goto done;
    decoder = avcodec_alloc_context3(codec);
    if (!decoder || avcodec_parameters_to_context(decoder, input->streams[index]->codecpar) < 0 ||
        avcodec_open2(decoder, codec, NULL) < 0) goto done;
    while (av_read_frame(input, packet) >= 0) {
        int result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result < 0) break;
        while (avcodec_receive_frame(decoder, frame) >= 0)
            if (frame->width == WIDTH && frame->height == HEIGHT) ++frames;
    }
done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    return frames >= 3 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--probe") == 0) return probe(argv[2]);
    if (argc != 4) return EXIT_FAILURE;
    int port = atoi(argv[1]);
    const char *control = argv[2];
    camera_t cameras[CAMERAS] = {0};
    uint8_t pixels[WIDTH * HEIGHT * 3 / 2], access_unit[4096];
    size_t bytes = sizeof(access_unit);
    int result = EXIT_FAILURE;
    turbo_encoded_frame_t info = {0};
    const turbo_video_codec_config_t encoding = {
        .width = WIDTH, .height = HEIGHT, .framerate = FPS,
        .bitrate = 200000, .keyframe_interval = 1, .threads = 1
    };
    const turbo_media_server_config_t server = {
        .max_sources = CAMERAS, .source_config = {.max_tracks = 2, .max_subscribers = 4, .gop_capacity = 4}
    };
    const turbo_rtsp_server_config_t rtsp = {
        .bind_host = "127.0.0.1", .port = port, .client_timeout_ms = 30000,
        .connection_capacity = CAMERAS * 3, .send_queue_capacity = 64, .send_queue_bytes = 512 * 1024
    };
    const turbo_media_rtsp_server_adapter_config_t bridge = {
        .vhost = "default", .session_id = "monitor-fixture", .default_rtp_channel_count = 2
    };
    memset(pixels, 128, sizeof(pixels));
    memset(pixels, 80, WIDTH * HEIGHT);
    turbo_codec_registry_init();
    turbo_codec_t *encoder = turbo_codec_create_encoder("h264", &encoding);
    turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&server);
    turbo_media_rtsp_server_adapter_t *adapter = runtime
        ? turbo_media_server_rtsp_adapter_create(runtime, &rtsp, &bridge) : NULL;
    if (!encoder || turbo_codec_encode(encoder, pixels, sizeof(pixels), access_unit, &bytes, &info) ||
        !bytes || !adapter || turbo_media_server_rtsp_adapter_start(adapter)) goto done;
    for (unsigned i = 0; i < CAMERAS; ++i)
        if (!connect_camera(&cameras[i], port, i + 1)) goto done;
    puts("ready");
    fflush(stdout);
    for (uint32_t frame = 1; frame <= 600; ++frame) {
        fs_stat_t ignored;
        if (fs_stat(argv[3], &ignored) == 0) break;
        bool pause = fs_stat(control, &ignored) == 0;
        for (unsigned i = 0; i < CAMERAS; ++i)
            if (!(i == 0 && pause) && rtp_payload_encode_input(cameras[i].packetizer,
                    access_unit, (int)bytes, frame * (RTP_CLOCK / FPS))) goto done;
        sleep_ms(1000 / FPS);
    }
    result = EXIT_SUCCESS;
done:
    for (unsigned i = 0; i < CAMERAS; ++i) {
        if (cameras[i].packetizer) rtp_payload_encode_destroy(cameras[i].packetizer);
        turbo_rtsp_client_destroy(cameras[i].client);
    }
    turbo_media_server_rtsp_adapter_destroy(adapter);
    turbo_media_server_runtime_destroy(runtime);
    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
    return result;
}
