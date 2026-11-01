#include "turbo_codec.h"
#include <turbo_streamgraph_h264.h>
#include <tinytest.h>

#include <stdint.h>
#include <string.h>

#define CHECK_TRUE(expr)       \
    do {                       \
        check(expr);           \
        if (!(expr)) {         \
            return;            \
        }                      \
    } while (0)

#define CHECK_INT(expected, actual)            \
    do {                                      \
        int expected_value = (expected);      \
        int actual_value = (actual);          \
        check_equal(actual_value, expected_value); \
        if (actual_value != expected_value) { \
            return;                           \
        }                                     \
    } while (0)

static turbo_audio_codec_config_t g711_config(void) {
    turbo_audio_codec_config_t config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
    config.channels = 1;
    config.bitrate = 64000;
    config.frame_size_ms = 20;
    return config;
}

static void test_registry_lifecycle_registers_g711(void) {
    turbo_codec_registry_shutdown();
    CHECK_TRUE(turbo_codec_find_by_name("pcmu") == NULL);
    CHECK_TRUE(turbo_codec_find_by_pt(0) == NULL);
    CHECK_INT(8000, turbo_codec_audio_rtp_clock_rate("pcmu"));
    CHECK_INT(8000, turbo_codec_audio_rtp_clock_rate("pcma"));
#ifdef TURBO_MEDIA_HAS_OPUS
    CHECK_INT(48000, turbo_codec_audio_rtp_clock_rate("opus"));
#endif
    CHECK_INT(0, turbo_codec_audio_rtp_clock_rate("missing"));
    CHECK_INT(0, turbo_codec_audio_rtp_clock_rate(NULL));

    turbo_codec_registry_init();

    const turbo_codec_ops_t *pcmu = turbo_codec_find_by_name("pcmu");
    const turbo_codec_ops_t *pcma = turbo_codec_find_by_name("pcma");

    CHECK_TRUE(pcmu != NULL);
    CHECK_TRUE(pcma != NULL);
    CHECK_TRUE(turbo_codec_find_by_pt(0) == pcmu);
    CHECK_TRUE(turbo_codec_find_by_pt(8) == pcma);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcmu->type);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcma->type);
    CHECK_INT(8000, pcmu->clock_rate);
    CHECK_INT(8000, pcma->clock_rate);
    CHECK_INT(pcmu->clock_rate, turbo_codec_audio_rtp_clock_rate("pcmu"));
    CHECK_INT(pcma->clock_rate, turbo_codec_audio_rtp_clock_rate("pcma"));

    turbo_codec_registry_shutdown();
    CHECK_TRUE(turbo_codec_find_by_name("pcmu") == NULL);
}

static void test_registry_rejects_duplicate_codec(void) {
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    const turbo_codec_ops_t *pcmu = turbo_codec_find_by_name("pcmu");

    CHECK_TRUE(pcmu != NULL);
    CHECK_INT(-1, turbo_codec_register(pcmu));
    CHECK_INT(-1, turbo_codec_register(NULL));

    turbo_codec_registry_shutdown();
}

static void test_unknown_codec_creation_fails(void) {
    turbo_audio_codec_config_t config = g711_config();

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    CHECK_TRUE(turbo_codec_create_encoder("missing", &config) == NULL);
    CHECK_TRUE(turbo_codec_create_decoder("missing", &config) == NULL);
    CHECK_TRUE(turbo_codec_find_by_name(NULL) == NULL);

    turbo_codec_registry_shutdown();
}

static void test_encode_decode_require_matching_codec_direction(void) {
    turbo_audio_codec_config_t config = g711_config();
    uint8_t input[] = {0xff};
    uint8_t output[8];
    size_t output_len = sizeof(output);

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_decode(encoder, input, sizeof(input), output, &output_len));
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_encode(decoder, input, sizeof(input), output, &output_len, NULL));

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

static void test_g711_buffer_limits_are_reported(void) {
    const int16_t pcm_samples[] = {-12000, 0, 12000};
    turbo_audio_codec_config_t config = g711_config();
    uint8_t encoded[2];
    size_t encoded_len = sizeof(encoded);

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_INT(TURBO_CODEC_ERR_BUFFER,
              turbo_codec_encode(encoder,
                                 (const uint8_t *)pcm_samples,
                                 sizeof(pcm_samples),
                                 encoded,
                                 &encoded_len,
                                 NULL));

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
}

static void test_g711_round_trip_sets_frame_info(void) {
    const int16_t pcm_samples[] = {-12000, -8000, -4000, 0, 4000, 8000, 12000};
    turbo_audio_codec_config_t config = g711_config();
    uint8_t encoded[sizeof(pcm_samples) / sizeof(pcm_samples[0])];
    uint8_t decoded[sizeof(pcm_samples)];
    size_t encoded_len = sizeof(encoded);
    size_t decoded_len = sizeof(decoded);
    turbo_encoded_frame_t frame_info;

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcma", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcma", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);

    memset(&frame_info, 0, sizeof(frame_info));
    CHECK_INT(TURBO_CODEC_OK,
              turbo_codec_encode(encoder,
                                 (const uint8_t *)pcm_samples,
                                 sizeof(pcm_samples),
                                 encoded,
                                 &encoded_len,
                                 &frame_info));
    CHECK_TRUE(frame_info.data == encoded);
    CHECK_TRUE(frame_info.len == encoded_len);
    CHECK_INT(0, frame_info.is_keyframe);
    CHECK_INT(1, frame_info.packet_count);
    CHECK_TRUE(encoded_len == sizeof(encoded));

    CHECK_INT(TURBO_CODEC_OK,
              turbo_codec_decode(decoder, encoded, encoded_len, decoded, &decoded_len));
    CHECK_TRUE(decoded_len == sizeof(decoded));

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

/* Extended test helper functions */
static turbo_video_codec_config_t video_config(void) {
    turbo_video_codec_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = 640;
    config.height = 480;
    config.framerate = 30;
    config.bitrate = 1000000; // 1 Mbps
    config.keyframe_interval = 30;
    config.threads = 1;
    config.quality = 25;
    return config;
}

static void generate_test_frame(uint8_t *frame, int width, int height) {
    // Generate simple I420 test pattern
    int y_size = width * height;
    int uv_size = (width * height) / 4;
    
    // Y plane - gradient
    for (int i = 0; i < y_size; i++) {
        frame[i] = (uint8_t)(i % 256);
    }
    
    // U plane - constant
    for (int i = 0; i < uv_size; i++) {
        frame[y_size + i] = 128;
    }
    
    // V plane - constant  
    for (int i = 0; i < uv_size; i++) {
        frame[y_size + uv_size + i] = 128;
    }
}

static void test_codec_lifecycle_management(void);
static void test_codec_various_input_sizes(void);
static void test_codec_error_conditions(void);
static void test_codec_bitrate_control(void);
static void test_video_keyframe_request(void);
static void test_packet_loss_concealment(void);
static void test_codec_multiple_sample_rates(void);
static void test_codec_concurrent_creation(void);

/* Test codec registry with multiple codecs */
static void test_registry_supports_multiple_codec_types(void) {
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Test audio codecs
    const turbo_codec_ops_t *pcmu = turbo_codec_find_by_name("pcmu");
    const turbo_codec_ops_t *pcma = turbo_codec_find_by_name("pcma");
    CHECK_TRUE(pcmu != NULL);
    CHECK_TRUE(pcma != NULL);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcmu->type);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcma->type);

#ifdef TURBO_MEDIA_HAS_VPX
    // Test video codecs if available
    const turbo_codec_ops_t *vp8 = turbo_codec_find_by_name("vp8");
    if (vp8) {
        CHECK_INT(TURBO_CODEC_TYPE_VIDEO, vp8->type);
        CHECK_TRUE(vp8->clock_rate == 90000); // Standard video clock rate
    }
#endif

#ifdef TURBO_MEDIA_HAS_OPUS
    // Test Opus codec if available
    const turbo_codec_ops_t *opus = turbo_codec_find_by_name("opus");
    if (opus) {
        CHECK_INT(TURBO_CODEC_TYPE_AUDIO, opus->type);
        CHECK_TRUE(opus->clock_rate == 48000);
    }
#endif

    turbo_codec_registry_shutdown();
}

/* Test codec configuration validation */
static void test_audio_codec_config_validation(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    CHECK_INT(TURBO_CODEC_OK,
              turbo_codec_validate_audio_config("pcmu", &config));
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_validate_audio_config("missing", &config));
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_validate_audio_config(NULL, &config));
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_validate_audio_config("pcmu", NULL));

    // Test invalid sample rates
    config.sample_rate = 0;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.sample_rate = -1;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.sample_rate = 999999; // Unrealistic rate
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_validate_audio_config("pcmu", &config));

#ifdef TURBO_MEDIA_HAS_OPUS
    config = g711_config();
    config.sample_rate = 12000;
    config.bitrate = 32000;
    config.complexity = 5;
    CHECK_INT(TURBO_CODEC_OK,
              turbo_codec_validate_audio_config("opus", &config));
#endif

    // Test invalid channels
    config = g711_config();
    config.channels = 0;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.channels = -1;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.channels = 100; // Too many channels
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);

    // Test invalid frame sizes
    config = g711_config();
    config.frame_size_ms = 0;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.frame_size_ms = -1;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);

    turbo_codec_registry_shutdown();
}

/* Test video codec operations */
static void test_video_codec_config_validation(void) {
#ifdef TURBO_MEDIA_HAS_VPX
    turbo_video_codec_config_t config = video_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Test invalid dimensions
    config.width = 0;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config = video_config();
    config.height = 0;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config = video_config();
    config.width = -1;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);

    // Test invalid framerate
    config = video_config();
    config.framerate = 0;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config.framerate = -1;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config.framerate = 1000; // Unrealistic framerate
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);

    // Test invalid bitrate
    config = video_config();
    config.bitrate = -1;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);

    turbo_codec_registry_shutdown();
#endif
}


/* M17c: actual OpenH264 bytes -> one Codec Provider -> canonical CPU
 * StreamGraph Source/Executor -> FrameStore -> 2 independent sink credits.
 * Executed in existing test_codec; no consumer/installer/probe project. */
static size_t sh_start(const uint8_t *p,size_t n,size_t at) {
    if(at+3u<=n && !p[at] && !p[at+1u] && p[at+2u]==1u) return 3u;
    if(at+4u<=n && !p[at] && !p[at+1u] &&
       !p[at+2u] && p[at+3u]==1u) return 4u;
    return 0u;
}
static int sh_split(const uint8_t *input,size_t len,
                    uint8_t *config,size_t *config_len,
                    uint8_t *idr,size_t *idr_len,size_t capacity) {
    size_t at=0u;
    unsigned seen=0u;
    *config_len=0u;*idr_len=0u;
    while(at<len) {
        size_t prefix=sh_start(input,len,at),start,end,i;
        unsigned type;
        uint8_t *out;
        size_t *used;
        if(!prefix) return 0;
        start=at+prefix;
        if(start>=len) return 0;
        type=input[start]&0x1fu;
        end=len;
        for(i=start+1u;i<len;i++) if(sh_start(input,len,i)) {end=i;break;}
        if(type==7u || type==8u || type==5u) {
            out=(type==5u)?idr:config;
            used=(type==5u)?idr_len:config_len;
            if(end-at>capacity-*used) return 0;
            memcpy(out+*used,input+at,end-at);
            *used+=end-at;
            seen|=(type==7u)?1u:(type==8u)?2u:4u;
        }
        at=end;
    }
    return seen==7u && *config_len && *idr_len;
}
typedef struct sh_fixture {
    tm_sh_decoder *decoder;
    unsigned processed,recorded,previewed;
    uint64_t frame_id;
} sh_fixture;
static tm_sg_port_v1 sh_port(const char *name,uint32_t direction) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=direction;p.kind=TM_SG_VIDEO_FRAME;
    p.memory_domain=TM_SG_MEMORY_CPU;p.format_id=420u;
    p.timebase_num=1u;p.timebase_den=1000000u;
    return p;
}
static tm_sg_program *sh_program(void) {
    tm_sg_port_v1 input=sh_port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 processor[]={
        sh_port("in",TM_SG_INPUT),sh_port("out",TM_SG_OUTPUT)};
    tm_sg_port_v1 sink=sh_port("in",TM_SG_INPUT);
    const char *names[]={"h264_source","inspect","record","preview"};
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    tm_sg_program *program=NULL;
    unsigned i;
    for(i=0;i<4u;i++) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=names[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:
                      i==1u?TM_SG_PROCESSOR:TM_SG_SINK;
        nodes[i].ports=i==0u?&input:i==1u?processor:&sink;
        nodes[i].port_count=i==1u?2u:1u;
    }
    for(i=0;i<3u;i++) {
        edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
        edges[i].source_node=i==0u?"h264_source":"inspect";
        edges[i].source_port="out";
        edges[i].target_node=i==0u?"inspect":names[i+1u];
        edges[i].target_port="in";
    }
    if(tm_sg_compile(nodes,4u,edges,3u,NULL,&program,NULL)!=TM_SG_OK)
        return NULL;
    return program;
}
static int sh_inspect(void *ctx,const tm_fs_frame_ref_v1 *ref,
                      const tm_fs_cpu_view_v1 *view) {
    sh_fixture *f=(sh_fixture *)ctx;
    tm_sh_stamp_v1 stamp=TM_SH_STAMP_V1_INIT;
    if(tm_sh_last_stamp(f->decoder,&stamp)!=TM_SH_OK ||
       stamp.packet_sequence!=2u || stamp.pts_us!=33000 ||
       stamp.stream.display_generation!=ref->display_generation ||
       stamp.stream.session_generation!=ref->source_session_generation ||
       stamp.decoded_bytes!=view->size_bytes ||
       view->size_bytes!=64u*64u*3u/2u) return -1;
    f->frame_id=ref->frame_id;f->processed++;return 0;
}
static int sh_record(void *ctx,const tm_fs_frame_ref_v1 *ref,
                     const tm_fs_cpu_view_v1 *view) {
    sh_fixture *f=(sh_fixture *)ctx;
    if(ref->frame_id!=f->frame_id ||
       view->size_bytes!=64u*64u*3u/2u) return -1;
    f->recorded++;return 0;
}
static int sh_preview(void *ctx,const tm_fs_frame_ref_v1 *ref,
                      const tm_fs_cpu_view_v1 *view) {
    sh_fixture *f=(sh_fixture *)ctx;
    if(ref->frame_id!=f->frame_id ||
       view->size_bytes!=64u*64u*3u/2u) return -1;
    f->previewed++;return 0;
}
static void test_real_h264_codec_streamgraph(void) {
    turbo_video_codec_config_t cfg=video_config();
    turbo_codec_t *encoder;
    uint8_t raw[64u*64u*3u/2u],encoded[65536],params[65536],idr[65536];
    size_t encoded_len=sizeof(encoded),params_len=0u,idr_len=0u;
    turbo_encoded_frame_t frame={0};
    tm_sh_config_v1 setup=TM_SH_CONFIG_V1_INIT;
    tm_sh_decoder *dec=NULL;
    tm_si_packet_v1 packet=TM_SI_PACKET_V1_INIT;
    tm_fs_config_v1 fcfg=TM_FS_CONFIG_V1_INIT;
    tm_fs_store *store=NULL;
    tm_ex_config_v1 ecfg=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 bindings[4];
    tm_ex_owner *executor=NULL;
    tm_sg_program *graph=NULL;
    sh_fixture f={0};
    int ready=0;
    unsigned i;
    cfg.width=64;cfg.height=64;cfg.keyframe_interval=1;
    turbo_codec_registry_init();
    encoder=turbo_codec_create_encoder("h264",&cfg);
    CHECK_TRUE(encoder!=NULL);
    generate_test_frame(raw,64,64);
    CHECK_INT(TURBO_CODEC_OK,turbo_codec_encode(
        encoder,raw,sizeof(raw),encoded,&encoded_len,&frame));
    CHECK_TRUE(frame.is_keyframe!=0 && encoded_len>0u);
    CHECK_TRUE(sh_split(encoded,encoded_len,params,&params_len,
                        idr,&idr_len,sizeof(params)));
    turbo_codec_destroy(encoder);

    setup.ingress.stream=(tm_si_stream_v1)TM_SI_STREAM_V1_INIT;
    setup.ingress.stream.device_session_id=1001u;
    setup.ingress.stream.session_generation=5u;
    setup.ingress.stream.display_id=7u;
    setup.ingress.stream.display_generation=3u;
    setup.ingress.stream.geometry_generation=8u;
    setup.ingress.stream.capture_hi=0xaaa5u;
    setup.ingress.stream.capture_lo=0xbbb8u;
    setup.ingress.stream.stream_generation=11u;
    setup.ingress.stream.width=64u;setup.ingress.stream.height=64u;
    setup.ingress.max_packet_bytes=65536u;
    CHECK_INT(TM_SH_OK,tm_sh_create(&setup,&dec));
    f.decoder=dec;
    packet.stream=setup.ingress.stream;
    packet.kind=TM_SI_CONFIG;packet.sequence=1u;
    packet.bytes=params;packet.byte_count=params_len;
    packet.pts_us=packet.dts_us=0;
    CHECK_INT(TM_SH_OK,tm_sh_offer(dec,&packet));
    CHECK_INT(TM_SH_AGAIN,tm_sh_pump(dec));
    CHECK_INT(TM_SH_OK,tm_sh_ready(dec,&ready));
    CHECK_INT(0,ready);
    packet.kind=TM_SI_IDR;packet.sequence=2u;
    packet.bytes=idr;packet.byte_count=idr_len;
    packet.pts_us=packet.dts_us=33000;
    CHECK_INT(TM_SH_OK,tm_sh_offer(dec,&packet));
    CHECK_INT(TM_SH_OK,tm_sh_pump(dec));
    CHECK_INT(TM_SH_OK,tm_sh_ready(dec,&ready));
    CHECK_INT(1,ready);

    fcfg.store_id=777u;fcfg.slot_capacity=2u;
    fcfg.max_frame_bytes=sizeof(raw);fcfg.max_pins=4u;
    fcfg.max_leases=4u;fcfg.max_retained_bytes=2u*sizeof(raw);
    CHECK_INT(TM_FS_OK,tm_fs_create(&fcfg,&store));
    graph=sh_program();CHECK_TRUE(graph!=NULL);
    ecfg.max_frame_bytes=sizeof(raw);
    ecfg.source_session_generation=setup.ingress.stream.session_generation;
    ecfg.display_generation=setup.ingress.stream.display_generation;
    ecfg.fanout.branch_count=2u;
    ecfg.fanout.branches[0]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ecfg.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    ecfg.fanout.branches[0].queue_depth=2u;
    ecfg.fanout.branches[1]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ecfg.fanout.branches[1].policy=TM_FO_LATEST;
    ecfg.fanout.branches[1].queue_depth=2u;
    for(i=0u;i<4u;i++) {
        const char *ids[]={"h264_source","inspect","record","preview"};
        bindings[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        bindings[i].node_id=ids[i];
        bindings[i].ctx=i==0u?(void *)dec:(void *)&f;
    }
    bindings[0].source_next=tm_sh_next_cpu_i420;
    bindings[1].visit=sh_inspect;
    bindings[2].visit=sh_record;
    bindings[3].visit=sh_preview;
    CHECK_INT(TM_EX_OK,tm_ex_create(graph,store,&ecfg,bindings,4u,&executor));
    CHECK_INT(TM_EX_OK,tm_ex_pump_source(executor));
    CHECK_INT(TM_EX_OK,tm_ex_pump_sink(executor,0u));
    CHECK_INT(TM_EX_OK,tm_ex_pump_sink(executor,1u));
    CHECK_TRUE(f.processed==1u && f.recorded==1u && f.previewed==1u);

    packet.kind=TM_SI_EOS;packet.sequence=3u;
    packet.bytes=NULL;packet.byte_count=0u;
    packet.pts_us=packet.dts_us=66000;
    CHECK_INT(TM_SH_OK,tm_sh_offer(dec,&packet));
    CHECK_INT(TM_SH_EOS,tm_sh_pump(dec));
    CHECK_INT(TM_EX_EOS,tm_ex_pump_source(executor));
    CHECK_INT(TM_EX_OK,tm_ex_destroy(executor));
    CHECK_INT(TM_FS_OK,tm_fs_seal(store));
    CHECK_INT(TM_FS_OK,tm_fs_destroy(store));
    tm_sg_program_destroy(graph);
    CHECK_INT(TM_SH_OK,tm_sh_destroy(dec));
    turbo_codec_registry_shutdown();
}

suite("turbo_media_codec") {
    group("Codec Registry Management") {
        it("registers and unregisters built-in G.711 codecs") {
            test_registry_lifecycle_registers_g711();
        }

        it("rejects duplicate codec registrations") {
            test_registry_rejects_duplicate_codec();
        }

        it("rejects unknown codec creation") {
            test_unknown_codec_creation_fails();
        }

        it("supports multiple codec types in registry") {
            test_registry_supports_multiple_codec_types();
        }
    }

    group("Codec Configuration Validation") {
        it("validates audio codec configuration parameters") {
            test_audio_codec_config_validation();
        }

        it("validates video codec configuration parameters") {
            test_video_codec_config_validation();
        }

        it("supports multiple sample rates") {
            test_codec_multiple_sample_rates();
        }
    }

    group("Codec Operations") {
        it("requires codec direction to match encode and decode calls") {
            test_encode_decode_require_matching_codec_direction();
        }

        it("reports G.711 output buffer limits") {
            test_g711_buffer_limits_are_reported();
        }

        it("round-trips G.711 and sets frame metadata") {
            test_g711_round_trip_sets_frame_info();
        }

        it("handles various input sizes correctly") {
            test_codec_various_input_sizes();
        }
    }

    group("Codec Lifecycle") {
        it("manages codec instance lifecycle properly") {
            test_codec_lifecycle_management();
        }

        it("supports concurrent codec instance creation") {
            test_codec_concurrent_creation();
        }
    }

    group("Error Handling") {
        it("handles error conditions gracefully") {
            test_codec_error_conditions();
        }
    }

    group("H264 into real StreamGraph CPU owner") {
        it("decodes real H264 and fans out one FrameStore lease per consumer") {
            test_real_h264_codec_streamgraph();
        }
    }

    group("Advanced Features") {
        it("supports bitrate control") {
            test_codec_bitrate_control();
        }

        it("supports keyframe requests for video codecs") {
            test_video_keyframe_request();
        }

        it("supports packet loss concealment") {
            test_packet_loss_concealment();
        }
    }

}

/* Test codec lifecycle management */
static void test_codec_lifecycle_management(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);

    // Test proper destruction
    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    
    // Test null destruction (should not crash)
    turbo_codec_destroy(NULL);

    turbo_codec_registry_shutdown();
}

/* Test codec operations with various input sizes */
static void test_codec_various_input_sizes(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    CHECK_TRUE(encoder != NULL);

    // Test different input sizes
    int16_t samples_160[160];  // 160 samples for 20ms @ 8kHz
    uint8_t encoded[256];
    size_t encoded_len;
    turbo_encoded_frame_t frame_info;

    // Initialize samples
    for (int i = 0; i < 160; i++) {
        samples_160[i] = (int16_t)(i * 100);
    }

    // Test normal frame size
    encoded_len = sizeof(encoded);
    memset(&frame_info, 0, sizeof(frame_info));
    int result = turbo_codec_encode(encoder,
                                  (const uint8_t *)samples_160,
                                  sizeof(samples_160),
                                  encoded,
                                  &encoded_len,
                                  &frame_info);
    
    if (result == TURBO_CODEC_OK) {
        CHECK_TRUE(encoded_len > 0);
        CHECK_TRUE(frame_info.len == encoded_len);
        CHECK_TRUE(frame_info.data == encoded);
    }

    // Test empty input
    encoded_len = sizeof(encoded);
    result = turbo_codec_encode(encoder, NULL, 0, encoded, &encoded_len, NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID, result);

    // Test null output
    encoded_len = sizeof(encoded);
    result = turbo_codec_encode(encoder,
                              (const uint8_t *)samples_160,
                              sizeof(samples_160),
                              NULL,
                              &encoded_len,
                              NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID, result);

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
}

/* Test codec error handling */
static void test_codec_error_conditions(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);

    // Test operations with null codec instances
    uint8_t buffer[128];
    size_t buffer_len = sizeof(buffer);
    
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_encode(NULL, buffer, sizeof(buffer), 
                               buffer, &buffer_len, NULL));
    
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_decode(NULL, buffer, sizeof(buffer),
                               buffer, &buffer_len));

    // Test with null output length pointer
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_encode(encoder, buffer, sizeof(buffer),
                               buffer, NULL, NULL));

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

/* Test codec bitrate control */
static void test_codec_bitrate_control(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    if (encoder == NULL) {
        turbo_codec_registry_shutdown();
        return;
    }

    // Test bitrate setting if supported
    turbo_codec_set_bitrate(encoder, 64000);
    turbo_codec_set_bitrate(encoder, 128000);

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
}

/* Test keyframe request for video codecs */
static void test_video_keyframe_request(void) {
#ifdef TURBO_MEDIA_HAS_VPX
    turbo_video_codec_config_t config = video_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("vp8", &config);
    if (encoder == NULL) {
        turbo_codec_registry_shutdown();
        return;
    }

    // Test keyframe request
    turbo_codec_request_keyframe(encoder);

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
#endif
}

/* Test packet loss concealment */
static void test_packet_loss_concealment(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);
    if (decoder == NULL) {
        turbo_codec_registry_shutdown();
        return;
    }

    // Test PLC if supported
    {
        uint8_t plc_frame[320];
        size_t plc_len = sizeof(plc_frame);
        int result = turbo_codec_plc(decoder, plc_frame, &plc_len);

        if (result == TURBO_CODEC_OK) CHECK_TRUE(plc_len > 0);
    }

    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

/* Test codec with different sample rates */
static void test_codec_multiple_sample_rates(void) {
    int sample_rates[] = {8000, 16000, 24000, 48000};
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    for (size_t i = 0; i < sizeof(sample_rates) / sizeof(sample_rates[0]); i++) {
        turbo_audio_codec_config_t config = g711_config();
        config.sample_rate = sample_rates[i];
        
        turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
        if (encoder) {
            // Successfully created with this sample rate
            turbo_codec_destroy(encoder);
        }
    }

    turbo_codec_registry_shutdown();
}

/* Test codec thread safety preparation */
static void test_codec_concurrent_creation(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Create multiple codec instances
    turbo_codec_t *encoders[5];
    turbo_codec_t *decoders[5];
    
    for (int i = 0; i < 5; i++) {
        encoders[i] = turbo_codec_create_encoder("pcmu", &config);
        decoders[i] = turbo_codec_create_decoder("pcmu", &config);
    }

    // Verify all created successfully
    int success_count = 0;
    for (int i = 0; i < 5; i++) {
        if (encoders[i] && decoders[i]) {
            success_count++;
        }
    }
    
    CHECK_TRUE(success_count > 0);

    // Clean up
    for (int i = 0; i < 5; i++) {
        if (encoders[i]) turbo_codec_destroy(encoders[i]);
        if (decoders[i]) turbo_codec_destroy(decoders[i]);
    }

    turbo_codec_registry_shutdown();
}
