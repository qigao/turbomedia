#include <turbo_streamgraph.h>
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

int main(void) {
    test_immutable_and_negative_contracts();
    test_cycle_and_bounded_fanout();
    puts("StreamGraph typed contract tests: PASS");
    return 0;
}
