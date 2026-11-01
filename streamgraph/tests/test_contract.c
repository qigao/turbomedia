#include <turbo_streamgraph.h>
#include <turbo_streamgraph_screen_packet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PORT(id,dir,kind) {sizeof(tm_sg_port_v1), TM_SG_ABI_VERSION, id, dir, kind, 0u, TM_SG_MEMORY_CPU, 0u, 0u, 0u}
#define NODE(id,kind,ports) {sizeof(tm_sg_node_v1), TM_SG_ABI_VERSION, id, kind, (uint32_t)(sizeof(ports)/sizeof(ports[0])), ports}
#define EDGE(a,b,c,d) {sizeof(tm_sg_edge_v1), TM_SG_ABI_VERSION, a,b,c,d}

static void test_immutable_and_negative_contracts(void) {
    tm_sg_port_v1 source[] = {PORT("out", TM_SG_OUTPUT, TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 process[] = {PORT("in", TM_SG_INPUT, TM_SG_VIDEO_FRAME),
                               PORT("out", TM_SG_OUTPUT, TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 sink[] = {PORT("in", TM_SG_INPUT, TM_SG_VIDEO_FRAME)};
    tm_sg_node_v1 n[] = {NODE("capture",TM_SG_SOURCE,source),
                         NODE("decode",TM_SG_PROCESSOR,process),
                         NODE("viewer",TM_SG_SINK,sink)};
    tm_sg_edge_v1 e[] = {EDGE("capture","out","decode","in"),
                         EDGE("decode","out","viewer","in")};
    tm_sg_program *p = NULL;
    const tm_sg_node_v1 *view = NULL;
    const tm_sg_edge_v1 *edge = NULL;
    tm_sg_diagnostic_v1 d = TM_SG_DIAGNOSTIC_V1_INIT;
    tm_sg_limits_v1 limits = TM_SG_LIMITS_V1_INIT;

    assert(tm_sg_compile(n,3,e,2,NULL,&p,&d) == TM_SG_OK);
    assert(p != NULL && tm_sg_program_node_count(p) == 3u);
    assert(tm_sg_program_node_at(p,0,&view) == TM_SG_OK && strcmp(view->id,"capture")==0);
    assert(tm_sg_program_node_at(p,1,&view) == TM_SG_OK && strcmp(view->id,"decode")==0);
    assert(tm_sg_program_edge_at(p,0,&edge) == TM_SG_OK && strcmp(edge->target_node,"decode")==0);
    source[0].name = "elsewhere";
    n[0].id = "rewritten";
    e[0].source_node = "rewritten";
    assert(tm_sg_program_node_at(p,0,&view) == TM_SG_OK && strcmp(view->id,"capture")==0);
    assert(strcmp(view->ports[0].name,"out")==0);
    assert(tm_sg_program_edge_at(p,0,&edge) == TM_SG_OK && strcmp(edge->source_node,"capture")==0);
    tm_sg_program_destroy(p); p = NULL;

    source[0].name = "out";
    n[0].id = "capture";
    e[0].source_node = "capture";
    sink[0].kind = TM_SG_AUDIO_FRAME;
    assert(tm_sg_compile(n,3,e,2,NULL,&p,&d) == TM_SG_ETYPE && p == NULL && d.edge_index==1u);
    sink[0].kind = TM_SG_VIDEO_FRAME;
    process[1].format_id = 3u; sink[0].format_id = 5u;
    assert(tm_sg_compile(n,3,e,2,NULL,&p,&d) == TM_SG_EFORMAT && p == NULL);
    process[1].format_id = sink[0].format_id = 0u;
    n[1].id = "capture";
    assert(tm_sg_compile(n,3,e,2,NULL,&p,&d) == TM_SG_EDUPLICATE);
    n[1].id = "decode";
    e[1].target_node = "missing";
    assert(tm_sg_compile(n,3,e,2,NULL,&p,&d) == TM_SG_ENOTFOUND);
    e[1].target_node = "viewer";
    limits.max_nodes = 2u;
    assert(tm_sg_compile(n,3,e,2,&limits,&p,&d) == TM_SG_ELIMIT);
    limits.max_nodes = 3u;
    source[0].version = 99u;
    assert(tm_sg_compile(n,3,e,2,&limits,&p,&d) == TM_SG_EVERSION);
    source[0].version = TM_SG_ABI_VERSION;
    sink[0].timebase_num = 1u;
    assert(tm_sg_compile(n,3,e,2,&limits,&p,&d) == TM_SG_EINVAL);
}

static void test_cycle_and_bounded_fanout(void) {
    tm_sg_port_v1 source[] = {PORT("out",TM_SG_OUTPUT,TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 middle[] = {PORT("in",TM_SG_INPUT,TM_SG_VIDEO_FRAME),
                              PORT("loop",TM_SG_INPUT,TM_SG_VIDEO_FRAME),
                              PORT("out",TM_SG_OUTPUT,TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 second[] = {PORT("in",TM_SG_INPUT,TM_SG_VIDEO_FRAME),
                              PORT("out",TM_SG_OUTPUT,TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 sink[] = {PORT("in",TM_SG_INPUT,TM_SG_VIDEO_FRAME)};
    tm_sg_node_v1 n[] = {NODE("source",TM_SG_SOURCE,source),
                         NODE("p1",TM_SG_PROCESSOR,middle),
                         NODE("p2",TM_SG_PROCESSOR,second),
                         NODE("sink",TM_SG_SINK,sink)};
    tm_sg_edge_v1 e[] = {EDGE("source","out","p1","in"),
                         EDGE("p1","out","p2","in"),
                         EDGE("p2","out","p1","loop"),
                         EDGE("p2","out","sink","in")};
    tm_sg_diagnostic_v1 d = TM_SG_DIAGNOSTIC_V1_INIT;
    tm_sg_program *p = NULL;
    assert(tm_sg_compile(n,4,e,4,NULL,&p,&d) == TM_SG_ECYCLE);

    tm_sg_port_v1 obs[] = {PORT("out",TM_SG_OUTPUT,TM_SG_OBSERVATION)};
    tm_sg_port_v1 obs_in1[] = {PORT("in",TM_SG_INPUT,TM_SG_OBSERVATION)};
    tm_sg_port_v1 obs_in2[] = {PORT("in",TM_SG_INPUT,TM_SG_OBSERVATION)};
    tm_sg_node_v1 fan[] = {NODE("recognizer",TM_SG_SOURCE,obs),
                          NODE("flow",TM_SG_SINK,obs_in1),NODE("trace",TM_SG_SINK,obs_in2)};
    tm_sg_edge_v1 links[] = {EDGE("recognizer","out","flow","in"),
                             EDGE("recognizer","out","trace","in")};
    tm_sg_limits_v1 limit = TM_SG_LIMITS_V1_INIT;
    obs[0].schema_id = obs_in1[0].schema_id = obs_in2[0].schema_id = 55u;
    limit.max_fanout = 1u;
    assert(tm_sg_compile(fan,3,links,2,&limit,&p,&d) == TM_SG_ELIMIT);
    limit.max_fanout = 2u;
    assert(tm_sg_compile(fan,3,links,2,&limit,&p,&d) == TM_SG_OK);
    tm_sg_program_destroy(p); p = NULL;
    obs_in2[0].schema_id = 66u;
    assert(tm_sg_compile(fan,3,links,2,&limit,&p,&d) == TM_SG_ETYPE);
}


/* M17a: a realistic typed Android-H264 plan, COMPILE-ONLY.
 * Current tm_ex_create handles only CPU VideoFrame -> read-only Processors
 * -> Sinks. This test MUST NOT claim a live H264/Observation executor. */
static void test_android_h264_graph_preflight_only(void) {
    /* Fixture-local format IDs; NOT a shipped codec/pixel-format registry. */
    enum { FMT_H264 = 264u, FMT_I420 = 420u };
    tm_sg_port_v1 packet[] = {PORT("out", TM_SG_OUTPUT, TM_SG_ENCODED_PACKET)};
    tm_sg_port_v1 decode[] = {
        PORT("in", TM_SG_INPUT, TM_SG_ENCODED_PACKET),
        PORT("out", TM_SG_OUTPUT, TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 sample[] = {
        PORT("in", TM_SG_INPUT, TM_SG_VIDEO_FRAME),
        PORT("out", TM_SG_OUTPUT, TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 vision[] = {
        PORT("in", TM_SG_INPUT, TM_SG_VIDEO_FRAME),
        PORT("out", TM_SG_OUTPUT, TM_SG_OBSERVATION)};
    tm_sg_port_v1 preview[] = {PORT("in", TM_SG_INPUT, TM_SG_VIDEO_FRAME)};
    tm_sg_port_v1 observation[] = {PORT("in", TM_SG_INPUT, TM_SG_OBSERVATION)};
    tm_sg_node_v1 nodes[] = {
        NODE("android_packet", TM_SG_SOURCE, packet),
        NODE("h264_decoder", TM_SG_PROCESSOR, decode),
        NODE("sampler", TM_SG_PROCESSOR, sample),
        NODE("vision", TM_SG_PROCESSOR, vision),
        NODE("preview", TM_SG_SINK, preview),
        NODE("observation", TM_SG_SINK, observation)};
    tm_sg_edge_v1 edges[] = {
        EDGE("android_packet", "out", "h264_decoder", "in"),
        EDGE("h264_decoder", "out", "sampler", "in"),
        EDGE("h264_decoder", "out", "preview", "in"),
        EDGE("sampler", "out", "vision", "in"),
        EDGE("vision", "out", "observation", "in")};
    tm_sg_program *compiled = NULL;
    tm_sg_diagnostic_v1 diag = TM_SG_DIAGNOSTIC_V1_INIT;
    size_t i;

    packet[0].format_id = decode[0].format_id = FMT_H264;
    packet[0].timebase_num = decode[0].timebase_num = 1u;
    packet[0].timebase_den = decode[0].timebase_den = 1000000u;
    decode[1].format_id = sample[0].format_id = sample[1].format_id =
        vision[0].format_id = preview[0].format_id = FMT_I420;
    decode[1].timebase_num = 1u;
    decode[1].timebase_den = 1000000u;
    for (i = 0u; i < 2u; i++) {
        sample[i].timebase_num = 1u;
        sample[i].timebase_den = 1000000u;
    }
    vision[0].timebase_num = preview[0].timebase_num = 1u;
    vision[0].timebase_den = preview[0].timebase_den = 1000000u;
    vision[1].schema_id = observation[0].schema_id = 0xA17u;

    assert(tm_sg_compile(nodes, 6u, edges, 5u, NULL,
                         &compiled, &diag) == TM_SG_OK);
    assert(compiled != NULL && tm_sg_program_node_count(compiled) == 6u);
    tm_sg_program_destroy(compiled);
    compiled = NULL;

    /* No compressed EncodedPacket -> decoded VideoFrame implicit edge. */
    edges[1].source_node = "android_packet";
    assert(tm_sg_compile(nodes, 6u, edges, 5u, NULL,
                         &compiled, &diag) == TM_SG_ETYPE && compiled == NULL);
    edges[1].source_node = "h264_decoder";

    decode[0].format_id = FMT_H264 + 1u;
    assert(tm_sg_compile(nodes, 6u, edges, 5u, NULL,
                         &compiled, &diag) == TM_SG_EFORMAT && compiled == NULL);
    decode[0].format_id = FMT_H264;

    packet[0].timebase_den = 90000u;
    assert(tm_sg_compile(nodes, 6u, edges, 5u, NULL,
                         &compiled, &diag) == TM_SG_EFORMAT && compiled == NULL);
    packet[0].timebase_den = 1000000u;

    decode[1].memory_domain = TM_SG_MEMORY_GPU;
    assert(tm_sg_compile(nodes, 6u, edges, 5u, NULL,
                         &compiled, &diag) == TM_SG_EFORMAT && compiled == NULL);
    decode[1].memory_domain = TM_SG_MEMORY_CPU;

    observation[0].schema_id++;
    assert(tm_sg_compile(nodes, 6u, edges, 5u, NULL,
                         &compiled, &diag) == TM_SG_ETYPE && compiled == NULL);
}


/* M17b actual H264 Annex-B copy/identity/order/credit ingress, not a decoder. */
static tm_si_stream_v1 screen_stream(void) {
    tm_si_stream_v1 s=TM_SI_STREAM_V1_INIT;
    s.device_session_id=1001u;s.session_generation=5u;s.display_id=7u;
    s.display_generation=3u;s.geometry_generation=8u;
    s.capture_hi=0xaaa5u;s.capture_lo=0xbbb8u;s.stream_generation=11u;
    s.width=640u;s.height=480u;return s;
}
static tm_si_packet_v1 packet(tm_si_stream_v1 stream, uint32_t kind,
                               uint64_t sequence, const uint8_t *bytes,size_t n) {
    tm_si_packet_v1 p=TM_SI_PACKET_V1_INIT;
    p.stream=stream;p.kind=kind;p.sequence=sequence;p.bytes=bytes;p.byte_count=n;
    p.pts_us=p.dts_us=(int64_t)(sequence*33000u);
    return p;
}
static void test_screen_packet_ownership_and_provenance(void) {
    tm_si_config_v1 cfg=TM_SI_CONFIG_V1_INIT;
    tm_si_owner *o=NULL;
    tm_si_packet_v1 out=TM_SI_PACKET_V1_INIT,p;
    const uint8_t config[]={0,0,0,1,0x67,0x42,0x00,0x1f,
                            0,0,0,1,0x68,0xce,0x06,0xe2};
    uint8_t key[]={0,0,0,1,0x65,0x80};
    const uint8_t delta[]={0,0,1,0x41,0x80};
    cfg.stream=screen_stream();cfg.max_packet_bytes=128u;
    assert(tm_si_create(&cfg,&o)==TM_SI_OK);

    p=packet(cfg.stream,TM_SI_IDR,1u,key,sizeof(key));
    assert(tm_si_offer(o,&p)==TM_SI_EFORMAT); /* SPS/PPS required */
    p=packet(cfg.stream,TM_SI_CONFIG,1u,config,sizeof(config));
    assert(tm_si_offer(o,&p)==TM_SI_OK);
    assert(tm_si_offer(o,&p)==TM_SI_EFULL);
    assert(tm_si_take(o,&out)==TM_SI_OK);
    assert(out.byte_count==sizeof(config) &&
           memcmp(out.bytes,config,sizeof(config))==0);
    assert(tm_si_take(o,&out)==TM_SI_EBUSY);
    assert(tm_si_destroy(o)==TM_SI_EBUSY);
    assert(tm_si_settle(o)==TM_SI_OK);
    assert(tm_si_settle(o)==TM_SI_EBUSY);

    p=packet(cfg.stream,TM_SI_IDR,2u,key,sizeof(key));
    assert(tm_si_offer(o,&p)==TM_SI_OK);
    key[5]=0xff; /* input mutation cannot alter owner-copied bytes */
    assert(tm_si_take(o,&out)==TM_SI_OK && out.bytes[5]==0x80);
    assert(tm_si_settle(o)==TM_SI_OK);

    p=packet(cfg.stream,TM_SI_DELTA,4u,delta,sizeof(delta));
    assert(tm_si_offer(o,&p)==TM_SI_ESTALE); /* no silent packet loss */
    p.sequence=3u;p.pts_us=p.dts_us=1;
    assert(tm_si_offer(o,&p)==TM_SI_EREORDER);
    p.pts_us=99000;p.dts_us=98000;
    assert(tm_si_offer(o,&p)==TM_SI_EREORDER); /* B frame unsupported */
    p.dts_us=p.pts_us;
    p.stream.geometry_generation++;
    assert(tm_si_offer(o,&p)==TM_SI_ESTALE);
    p.stream=cfg.stream;
    assert(tm_si_offer(o,&p)==TM_SI_OK);
    assert(tm_si_take(o,&out)==TM_SI_OK);
    assert(tm_si_settle(o)==TM_SI_OK);

    p=packet(cfg.stream,TM_SI_EOS,4u,NULL,0u);
    assert(tm_si_offer(o,&p)==TM_SI_OK);
    assert(tm_si_take(o,&out)==TM_SI_OK && out.bytes==NULL);
    assert(tm_si_settle(o)==TM_SI_OK);
    assert(tm_si_offer(o,&p)==TM_SI_ESHUTDOWN);
    assert(tm_si_destroy(o)==TM_SI_OK);
}
static void test_screen_packet_cancel_preserves_taken_credit(void) {
    tm_si_config_v1 cfg=TM_SI_CONFIG_V1_INIT;
    tm_si_owner *o=NULL;
    tm_si_packet_v1 out=TM_SI_PACKET_V1_INIT,p;
    uint8_t invalid[]={0,0,1,0xe5,0x80};
    const uint8_t sps[]={0,0,1,0x67,0x42,0,0x1f};
    cfg.stream=screen_stream();cfg.max_packet_bytes=64u;
    assert(tm_si_create(&cfg,&o)==TM_SI_OK);
    p=packet(cfg.stream,TM_SI_CONFIG,1u,invalid,sizeof(invalid));
    assert(tm_si_offer(o,&p)==TM_SI_EFORMAT);
    p.bytes=sps;p.byte_count=sizeof(sps);
    assert(tm_si_offer(o,&p)==TM_SI_OK);
    assert(tm_si_take(o,&out)==TM_SI_OK);
    assert(tm_si_cancel(o)==TM_SI_EBUSY);
    assert(tm_si_destroy(o)==TM_SI_EBUSY);
    assert(tm_si_settle(o)==TM_SI_OK);
    assert(tm_si_offer(o,&p)==TM_SI_ESHUTDOWN);
    assert(tm_si_destroy(o)==TM_SI_OK);
}

int main(void) {
    test_immutable_and_negative_contracts();
    test_cycle_and_bounded_fanout();
    test_android_h264_graph_preflight_only();
    test_screen_packet_ownership_and_provenance();
    test_screen_packet_cancel_preserves_taken_credit();
    puts("StreamGraph typed contract tests: PASS");
    return 0;
}
