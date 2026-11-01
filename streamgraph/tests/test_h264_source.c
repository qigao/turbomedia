
/*
 * Real TurboMedia::Codec/OpenH264 → existing StreamGraph Executor/FrameStore
 * acceptance IN THE ALREADY EXISTING streamgraph_executor CTest target.
 * No consumer app/project, mock decoder, alternate scheduler or fake pixels.
 */
#include <turbo_streamgraph_h264.h>
#include <turbo_codec.h>
#include <turbo_streamgraph_executor.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { W=64,H=64,I420_BYTES=W*H*3/2, ENCODED_CAP=512*1024 };
typedef struct {
    tm_fs_store *store;
    tm_fs_pin_v1 delayed_pin;
    tm_fs_frame_ref_v1 delayed_ref;
    uint64_t expected_packet_sequence, expected_pts;
    unsigned visited, record, preview;
} h264_fixture;

static tm_sg_port_v1 m17_port(const char *name,uint32_t direction){
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=direction;p.kind=TM_SG_VIDEO_FRAME;
    p.format_id=1u;p.memory_domain=TM_SG_MEMORY_CPU;
    p.timebase_num=1u;p.timebase_den=1000000u;
    return p;
}
static tm_sg_program *m17_program(void){
    tm_sg_port_v1 src=m17_port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 proc[2]={m17_port("in",TM_SG_INPUT),
                           m17_port("out",TM_SG_OUTPUT)};
    tm_sg_port_v1 sink=m17_port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    tm_sg_program *program=NULL;
    const char *names[4]={"src","proc","record","preview"};
    unsigned i;
    for(i=0u;i<4u;i++){
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=names[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:i==1u?TM_SG_PROCESSOR:TM_SG_SINK;
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&src:i==1u?proc:&sink;
    }
    for(i=0u;i<3u;i++)edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="src";edges[0].source_port="out";
    edges[0].target_node="proc";edges[0].target_port="in";
    for(i=1u;i<3u;i++){
        edges[i].source_node="proc";edges[i].source_port="out";
        edges[i].target_node=names[i+1u];edges[i].target_port="in";
    }
    assert(tm_sg_compile(nodes,4u,edges,3u,NULL,&program,NULL)==TM_SG_OK);
    return program;
}
static tm_fs_frame_metadata_v1 m17_metadata(uint64_t seq,uint64_t pts,
                                             int key){
    tm_fs_frame_metadata_v1 m=TM_FS_FRAME_METADATA_V1_INIT;
    m.width=W;m.height=H;m.format_id=1u;
    m.device_session_id=41u;m.session_generation=7u;m.display_id=8u;
    m.source_session_generation=77u;m.display_generation=9u;
    m.geometry_generation=3u;m.capture_session_lo=99u;
    m.stream_generation=55u;m.packet_sequence=seq;
    m.pts=pts;m.dts=pts;m.timebase_num=1u;m.timebase_den=1000000u;
    m.flags=key?TM_FS_META_FLAG_KEY_FRAME:0u;
    return m;
}
static int m17_processor(void *ctx,const tm_fs_frame_ref_v1 *ref,
                         const tm_fs_cpu_view_v1 *view){
    h264_fixture *f=(h264_fixture *)ctx;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    assert(ref && view && view->size_bytes==I420_BYTES);
    assert(tm_fs_get_metadata(f->store,ref,&meta)==TM_FS_OK);
    assert(meta.device_session_id==41u && meta.capture_session_lo==99u);
    assert(meta.geometry_generation==3u && meta.stream_generation==55u);
    assert(meta.packet_sequence==f->expected_packet_sequence);
    assert(meta.pts==f->expected_pts && meta.dts==f->expected_pts);
    assert(meta.flags==TM_FS_META_FLAG_KEY_FRAME);
    /* Delayed secondary inference needs its OWN pin before enqueue; FrameRef
     * alone grants no retention. Pin is reserved on Media Owner during visit. */
    assert(tm_fs_reserve_pin(f->store,ref,&f->delayed_pin)==TM_FS_OK);
    f->delayed_ref=*ref;
    f->visited++;
    return 0;
}
static int m17_record(void *ctx,const tm_fs_frame_ref_v1 *ref,
                      const tm_fs_cpu_view_v1 *view){
    h264_fixture *f=(h264_fixture *)ctx;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    assert(view->size_bytes==I420_BYTES);
    assert(tm_fs_get_metadata(f->store,ref,&meta)==TM_FS_OK);
    assert(meta.packet_sequence==f->expected_packet_sequence);
    f->record++;
    return 0;
}
static int m17_preview(void *ctx,const tm_fs_frame_ref_v1 *ref,
                       const tm_fs_cpu_view_v1 *view){
    h264_fixture *f=(h264_fixture *)ctx;
    assert(ref->frame_id>0u && view->size_bytes==I420_BYTES);
    f->preview++;
    return 0;
}
void test_m17_real_h264_codec_into_graph(void){
    turbo_video_codec_config_t encode_config={0};
    tm_sgh264_config_v1 config=TM_SGH264_CONFIG_V1_INIT;
    tm_sgh264_packet_v1 packet=TM_SGH264_PACKET_V1_INIT;
    tm_sgh264_source *source=NULL;
    turbo_codec_t *encoder=NULL;
    tm_sg_program *program=NULL;
    tm_fs_store *store=NULL;
    tm_ex_owner *owner=NULL;
    tm_fs_config_v1 fs=TM_FS_CONFIG_V1_INIT;
    tm_ex_config_v1 ex=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[4];
    h264_fixture f={0};
    uint8_t *encoded=NULL,*input=NULL;
    size_t encoded_size;
    turbo_encoded_frame_t info={0};
    const char *names[4]={"src","proc","record","preview"};
    tm_sgh264_status result=TM_SGH264_NEED_MORE;
    tm_fs_lease_v1 lease={0};
    tm_fs_cpu_view_v1 view=TM_FS_CPU_VIEW_V1_INIT;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    int ready=0;
    unsigned i;

    turbo_codec_registry_init();
    encode_config.width=W;encode_config.height=H;
    encode_config.framerate=30;encode_config.bitrate=400000;
    encode_config.keyframe_interval=1;encode_config.threads=1;
    encoder=turbo_codec_create_encoder("h264",&encode_config);
    assert(encoder!=NULL);
    config.width=W;config.height=H;config.max_packet_bytes=ENCODED_CAP;
    config.decoded_format_id=1u;config.device_session_id=41u;
    config.session_generation=7u;config.display_id=8u;
    config.source_session_generation=77u;config.display_generation=9u;
    config.geometry_generation=3u;config.capture_session_lo=99u;
    config.stream_generation=55u;
    {
        tm_sgh264_config_v1 too_large=config;
        tm_sgh264_source *rejected=NULL;
        too_large.width=4096u;too_large.height=2048u;
        assert(tm_sgh264_create(&too_large,&rejected)==TM_SGH264_EUNSUPPORTED);
        assert(rejected==NULL);
    }
    assert(tm_sgh264_create(&config,&source)==TM_SGH264_OK);

    input=(uint8_t *)malloc(I420_BYTES);
    encoded=(uint8_t *)malloc(ENCODED_CAP);
    assert(input!=NULL && encoded!=NULL);
    memset(input,70,W*H);
    memset(input+W*H,128,I420_BYTES-W*H);

    packet.framing=TM_SGH264_FRAMING_ANNEX_B;
    packet.bytes=encoded;
    /* Negative admission must not mutate decoder/provenance or consume seq. */
    packet.metadata=m17_metadata(1u,0u,1);
    packet.flags=TM_SGH264_PACKET_KEY_FRAME;
    packet.bytes_length=4u;
    encoded[0]=0u;encoded[1]=0u;encoded[2]=0u;encoded[3]=1u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EPROTOCOL);
    packet.metadata.geometry_generation=4u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EPROTOCOL);
    packet.metadata.geometry_generation=3u;
    packet.metadata.packet_sequence=2u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EPROTOCOL);
    packet.metadata.packet_sequence=1u;
    packet.metadata.dts=1u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EPROTOCOL);
    packet.metadata.dts=0u;
    packet.metadata.source_session_generation=1u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EPROTOCOL);
    packet.metadata.source_session_generation=77u;
    encoded[0]=0u;encoded[1]=0u;encoded[2]=0u;encoded[3]=1u;
    encoded[4]=0x67u;encoded[5]=0x64u;encoded[6]=0u;encoded[7]=0u;
    packet.bytes_length=8u;
    packet.flags=TM_SGH264_PACKET_CONFIG;
    packet.metadata.flags=0u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EPROTOCOL);
    packet.flags=TM_SGH264_PACKET_KEY_FRAME;
    packet.metadata.flags=TM_FS_META_FLAG_KEY_FRAME;
    packet.framing=2u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EUNSUPPORTED);
    packet.framing=TM_SGH264_FRAMING_ANNEX_B;

    /* Real TurboMedia H264 encoder, not hardcoded SPS/PPS and not a
     * synthetic frame. OpenH264 may require more than one decoded access
     * unit before it emits a frame; NEED_MORE is not EOF. */
    for(i=0u;i<6u && result!=TM_SGH264_FRAME_READY;i++){
        encoded_size=ENCODED_CAP;
        turbo_codec_request_keyframe(encoder);
        assert(turbo_codec_encode(encoder,input,I420_BYTES,encoded,
                                 &encoded_size,&info)==TURBO_CODEC_OK);
        assert(encoded_size>0u && info.is_keyframe);
        packet.bytes_length=encoded_size;
        packet.flags=TM_SGH264_PACKET_KEY_FRAME;
        packet.metadata=m17_metadata(i+1u,(uint64_t)i*33333u,1);
        result=tm_sgh264_submit(source,&packet);
        assert(result==TM_SGH264_NEED_MORE || result==TM_SGH264_FRAME_READY);
    }
    assert(result==TM_SGH264_FRAME_READY);
    f.expected_packet_sequence=packet.metadata.packet_sequence;
    f.expected_pts=packet.metadata.pts;
    assert(tm_sgh264_ready(source,&ready)==TM_SGH264_OK && ready==1);
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_EFULL);
    {
        turbo_codec_t *raw=turbo_codec_create_decoder("h264",NULL);
        uint32_t w=0u,h=0u;
        uint8_t raw_pixels[I420_BYTES];
        size_t raw_bytes=sizeof(raw_pixels);
        assert(raw!=NULL);
        assert(turbo_codec_decoded_video_geometry(raw,&w,&h)==TURBO_CODEC_ERR_NEED_MORE);
        assert(turbo_codec_decode(raw,encoded,encoded_size,
                                  raw_pixels,&raw_bytes)==TURBO_CODEC_OK);
        assert(turbo_codec_decoded_video_geometry(raw,&w,&h)==TURBO_CODEC_OK);
        assert(w==W && h==H && raw_bytes==I420_BYTES);
        turbo_codec_destroy(raw);
    }
    f.store=NULL;

    fs.store_id=999u;fs.slot_capacity=2u;fs.max_frame_bytes=I420_BYTES;
    fs.max_pins=8u;fs.max_leases=8u;
    fs.max_retained_bytes=2u*I420_BYTES;
    assert(tm_fs_create(&fs,&store)==TM_FS_OK);
    f.store=store;
    program=m17_program();
    ex.max_frame_bytes=I420_BYTES;
    ex.source_session_generation=77u;ex.display_generation=9u;
    ex.fanout.branch_count=2u;
    ex.fanout.branches[0]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ex.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    ex.fanout.branches[0].queue_depth=1u;
    ex.fanout.branches[1]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ex.fanout.branches[1].policy=TM_FO_LATEST;
    ex.fanout.branches[1].queue_depth=1u;
    for(i=0u;i<4u;i++){
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=names[i];
        binds[i].ctx=&f;
    }
    binds[0].ctx=source;
    binds[0].source_next=tm_sgh264_next_cpu_i420;
    binds[1].visit=m17_processor;binds[2].visit=m17_record;
    binds[3].visit=m17_preview;
    assert(tm_ex_create(program,store,&ex,binds,4u,&owner)==TM_EX_OK);
    assert(tm_ex_bind_source_metadata(owner,tm_sgh264_source_metadata,source)==TM_EX_OK);
    assert(tm_ex_pump_source(owner)==TM_EX_OK);
    assert(f.visited==1u);
    assert(tm_ex_pump_sink(owner,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(owner,1u)==TM_EX_OK);
    assert(f.record==1u && f.preview==1u);

    assert(tm_fs_acquire_pinned(store,&f.delayed_pin,&lease)==TM_FS_OK);
    assert(tm_fs_map_cpu(store,&lease,&view)==TM_FS_OK &&
           view.size_bytes==I420_BYTES);
    assert(tm_fs_get_metadata(store,&f.delayed_ref,&meta)==TM_FS_OK);
    assert(meta.packet_sequence==f.expected_packet_sequence &&
           meta.pts==f.expected_pts);
    assert(tm_fs_release_lease(store,&lease)==TM_FS_OK);
    assert(tm_fs_release_pin(store,&f.delayed_pin)==TM_FS_OK);
    assert(tm_fs_get_metadata(store,&f.delayed_ref,&meta)==TM_FS_ESTALE);

    packet.bytes=NULL;packet.bytes_length=0u;
    packet.flags=TM_SGH264_PACKET_EOS;
    packet.metadata=m17_metadata(f.expected_packet_sequence+1u,
                                  f.expected_pts+33333u,0);
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_OK);
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_ESTATE);
    assert(tm_sgh264_ready(source,&ready)==TM_SGH264_OK && ready==1);
    assert(tm_ex_pump_source(owner)==TM_EX_EOS);
    assert(tm_ex_destroy(owner)==TM_EX_OK);
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(program);
    assert(tm_sgh264_destroy(source)==TM_SGH264_OK);
    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
    free(input);
    free(encoded);
}
