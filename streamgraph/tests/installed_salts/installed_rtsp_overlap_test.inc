/* M15d: a real installed TurboMedia::RTSP server/client carries the same
 * MPEG-2 ES bytes used by the installed Salts ComponentPlugin DSO. The
 * callback borrows its original live CNet Session while the host cancels the
 * original Portal/Codec operation. No private RTSP implementation, second
 * registry, fake callback or automatic settlement retry.
 *
 * Include only in the opt-in Linux installed-SDK consumer. This fixture is
 * NOT an installed provider API and does not change production binaries.
 */
#include <turbo_rtsp.h>
#include <turbo_rtsp_rtp.h>
#include <salts/clock.h>

enum {
    TM_INSTALLED_LIVE_RTSP_PORT=21576,
    TM_INSTALLED_LIVE_RTSP_TIMEOUT_MS=5000,
    TM_INSTALLED_LIVE_RTSP_WIRE_CAP=65536
};
typedef struct tm_installed_live_rtsp {
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client;
    unsigned client_closed; /* accessed only by the RTSP Client Owner */
    _Atomic unsigned entered,release,exited;
    unsigned channel;
    size_t wire_len;
    uint8_t wire[TM_INSTALLED_LIVE_RTSP_WIRE_CAP];
} tm_installed_live_rtsp;

static int tm_installed_rtsp_announce(
    turbo_rtsp_session_t *session,const turbo_rtsp_request_t *req,
    turbo_rtsp_response_t *response,void *user) {
    (void)session;(void)req;(void)user;
    return turbo_rtsp_response_announce(response);
}
static int tm_installed_rtsp_setup(
    turbo_rtsp_session_t *session,const turbo_rtsp_request_t *req,
    turbo_rtsp_response_t *response,void *user) {
    (void)session;(void)user;
    return turbo_rtsp_response_setup(response,"tm-installed-102",req->transport);
}
static int tm_installed_rtsp_record(
    turbo_rtsp_session_t *session,const turbo_rtsp_request_t *req,
    turbo_rtsp_response_t *response,void *user) {
    (void)session;(void)req;(void)user;
    return turbo_rtsp_response_record(response,"npt=0-");
}
static int tm_installed_rtsp_interleaved(
    turbo_rtsp_session_t *session,uint8_t channel,
    const uint8_t *payload,size_t length,void *user) {
    tm_installed_live_rtsp *f=(tm_installed_live_rtsp *)user;
    (void)session;
    if(channel!=0u || !payload || length==0u ||
       length>sizeof(f->wire))return -1;
    f->channel=channel;
    memcpy(f->wire,payload,length);
    f->wire_len=length;
    atomic_store_explicit(&f->entered,1u,memory_order_release);
    while(!atomic_load_explicit(&f->release,memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&f->exited,1u,memory_order_release);
    return 0;
}
static int tm_installed_wait(_Atomic unsigned *flag) {
    const uint64_t start=cmeta_monotonic_ms();
    while(!atomic_load_explicit(flag,memory_order_acquire)) {
        if(cmeta_monotonic_ms()-start>=TM_INSTALLED_LIVE_RTSP_TIMEOUT_MS)
            return 0;
        sched_yield();
    }
    return 1;
}

/* One explicitly supported RFC2250 single MPEG Video ES picture, never TS,
 * H264, arbitrary fragmentation, or a second interpretation of B/E bits. */
static int tm_installed_make_rtp(
    const AVPacket *packet,uint8_t *wire,size_t capacity,size_t *used) {
    turbo_rtsp_rtp_header_t rtp={0};
    unsigned char es[TM_INSTALLED_LIVE_RTSP_WIRE_CAP];
    uint16_t tr=0u;
    uint8_t type=0u,sequence=0u;
    size_t i;
    int size;
    if(!packet || packet->size<=0 ||
       (size_t)packet->size>sizeof(es)-4u || packet->pts<0 ||
       packet->pts>INT32_MAX/3000)return -1;
    for(i=0u;i+5u<(size_t)packet->size;++i) {
        const uint8_t *p=packet->data+i;
        if(p[0]||p[1]||p[2]!=1u)continue;
        if(p[3]==0xb3u)sequence=1u;
        if(p[3]==0u) {
            tr=(uint16_t)(((uint16_t)p[4]<<2)|((uint16_t)p[5]>>6));
            type=(uint8_t)((p[5]>>3)&7u);
            break;
        }
    }
    if(i+5u>=(size_t)packet->size || type<1u || type>3u)return -1;
    es[0]=(uint8_t)((tr>>8)&3u);
    es[1]=(uint8_t)tr;
    es[2]=(uint8_t)((sequence?0x20u:0u)|0x18u|type);
    es[3]=0u;
    memcpy(es+4,packet->data,(size_t)packet->size);
    rtp.version=2u;
    rtp.payload_type=TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE;
    rtp.marker=1u;
    rtp.sequence_number=401u;
    rtp.timestamp=(uint32_t)packet->pts*3000u;
    rtp.ssrc=0x19641964u;
    size=turbo_rtsp_rtp_write_packet(
        wire,capacity,&rtp,es,(size_t)packet->size+4u);
    if(size<=0)return -1;
    *used=(size_t)size;
    return 0;
}

static tm_ffd_packet_v1 tm_installed_rtsp_received_packet(
    const tm_installed_live_rtsp *f,const AVPacket *original) {
    turbo_rtsp_rtp_header_t rtp={0};
    turbo_rtsp_mpeg_video_payload_t payload={0};
    tm_ffd_packet_v1 packet=TM_FFD_PACKET_V1_INIT;
    size_t offset=0u;
    assert(atomic_load_explicit(&f->entered,memory_order_acquire)==1u);
    assert(f->channel==0u);
    assert(turbo_rtsp_rtp_parse_header(
        f->wire,f->wire_len,&rtp,&offset)==0);
    assert(rtp.payload_type==TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE);
    assert(rtp.marker==1u && rtp.sequence_number==401u);
    assert(rtp.ssrc==0x19641964u);
    assert(turbo_rtsp_mpeg_video_payload_parse(
        rtp.payload,rtp.payload_len,&payload)==0);
    assert(turbo_rtsp_mpeg_video_one_picture_validate(
        &payload,rtp.marker)==0);
    assert(payload.es_len==(size_t)original->size);
    assert(memcmp(payload.es,original->data,payload.es_len)==0);
    assert(rtp.timestamp==(uint32_t)original->pts*3000u);
    packet.data=payload.es;packet.bytes=payload.es_len;
    packet.pts=(int64_t)(rtp.timestamp/3000u);
    packet.dts=AV_NOPTS_VALUE;
    return packet;
}

static void tm_installed_rtsp_open(
    tm_installed_live_rtsp *f,const AVPacket *packet) {
    static const char sdp[]=
        "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=Installed RTSP Provider\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 32\r\na=rtpmap:32 MPV/90000\r\n"
        "a=control:trackID=0\r\n";
    static const char uri[]="rtsp://127.0.0.1:21576/live";
    static const char track_uri[]="rtsp://127.0.0.1:21576/live/trackID=0";
    turbo_rtsp_server_config_t server_cfg={0};
    turbo_rtsp_client_config_t client_cfg={0};
    turbo_rtsp_server_handlers_t callbacks={0};
    turbo_rtsp_push_announce_t announce={0};
    turbo_rtsp_push_track_t track={0};
    uint8_t wire[TM_INSTALLED_LIVE_RTSP_WIRE_CAP];
    size_t wire_len=0u;
    memset(f,0,sizeof(*f));
    atomic_init(&f->entered,0u);
    atomic_init(&f->release,0u);
    atomic_init(&f->exited,0u);
    callbacks.on_announce=tm_installed_rtsp_announce;
    callbacks.on_setup=tm_installed_rtsp_setup;
    callbacks.on_record=tm_installed_rtsp_record;
    callbacks.on_interleaved_frame=tm_installed_rtsp_interleaved;
    server_cfg.bind_host="127.0.0.1";
    server_cfg.port=TM_INSTALLED_LIVE_RTSP_PORT;
    server_cfg.control_transport=TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    server_cfg.client_timeout_ms=TM_INSTALLED_LIVE_RTSP_TIMEOUT_MS;
    server_cfg.connection_capacity=4u;
    server_cfg.send_queue_capacity=16u;
    server_cfg.send_queue_bytes=512u*1024u;
    f->server=turbo_rtsp_server_create(&server_cfg,&callbacks,f);
    assert(f->server!=NULL);
    assert(turbo_rtsp_server_start(f->server)==0);
    client_cfg.host="127.0.0.1";
    client_cfg.port=TM_INSTALLED_LIVE_RTSP_PORT;
    client_cfg.timeout_ms=TM_INSTALLED_LIVE_RTSP_TIMEOUT_MS;
    client_cfg.control_transport=TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    f->client=turbo_rtsp_client_create(&client_cfg);
    assert(f->client!=NULL);
    assert(turbo_rtsp_client_connect(f->client)==0);
    announce.uri=uri;announce.sdp=sdp;announce.sdp_len=sizeof(sdp)-1u;
    track.control_uri=track_uri;
    track.rtp_channel=0u;track.rtcp_channel=1u;
    assert(turbo_rtsp_client_announce(f->client,&announce)==0);
    assert(turbo_rtsp_client_setup_interleaved(f->client,&track)==0);
    assert(turbo_rtsp_client_record(f->client)==0);
    assert(tm_installed_make_rtp(packet,wire,sizeof(wire),&wire_len)==0);
    assert(turbo_rtsp_client_send_interleaved_frame(
        f->client,0u,wire,wire_len)==0);
    assert(tm_installed_wait(&f->entered));
    (void)tm_installed_rtsp_received_packet(f,packet);
}

/* The distinct Client Owner retires admission while the server callback
 * and the original Codec Worker remain borrowed on two other stacks. */
static void tm_installed_rtsp_cancel_client(tm_installed_live_rtsp *f) {
    assert(f->client!=NULL);
    if(!f->client_closed) {
        turbo_rtsp_client_close(f->client);
        f->client_closed=1u;
    }
}
/* A borrowed server callback must actually exit before any RTSP session,
 * Plugin Scope, or Fixture memory is released. No double Client close. */
static void tm_installed_rtsp_close(tm_installed_live_rtsp *f) {
    assert(f->client!=NULL && f->server!=NULL);
    tm_installed_rtsp_cancel_client(f);
    atomic_store_explicit(&f->release,1u,memory_order_release);
    assert(tm_installed_wait(&f->exited));
    turbo_rtsp_client_destroy(f->client);
    f->client=NULL;
    turbo_rtsp_server_destroy(f->server);
    f->server=NULL;
}

/* The test-only gated CMeta Worker is inside the real loaded DSO, but
 * deliberately BEFORE the Codec call. Do not credit this as in-libavcodec
 * cancellation (the separate FFmpeg-patched gate tests that). */
static void tm_installed_rtsp_cancel_under_scope(
    tm_test_provider *service,salts_component_plugin_runtime *runtime,
    salts_component_plugin_generation *generation,
    cmeta_plugin_registry *registry,cmeta_plugin_ref plugin,
    cnet_client *wake,const AVPacket *packet) {
    tm_installed_live_rtsp rtsp;
    tm_test_instance_request instance={sizeof(instance),904u,102u};
    tm_test_delayed_packet_request incoming={0};
    tm_ffd_packet_v1 accepted;
    tm_pc_portal *portal=make_portal(102u);
    tm_pc_ticket_v1 ticket={0},taken={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    tm_test_delayed_step_gate gate={0};
    tm_test_delayed_step_request req={0};
    tm_test_delayed_take_request take={0};
    tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
    tm_test_delayed_stats_request stats_req={0};
    step_worker worker={0};
    pthread_t thread;
    uint8_t pixels[LUMA];
    size_t copied=55u;
    int64_t pts=55;
    atomic_init(&gate.entered,0u);
    atomic_init(&gate.release,0u);
    atomic_init(&gate.exited,0u);
    tm_installed_rtsp_open(&rtsp,packet);
    accepted=tm_installed_rtsp_received_packet(&rtsp,packet);
    incoming.size=sizeof(incoming);
    incoming.instance_id=instance.instance_id;
    incoming.provider_generation=instance.provider_generation;
    incoming.packet=accepted;
    assert(tm_test_provider_create_delayed(service,&instance)==0);
    assert(tm_test_provider_submit_delayed(service,&incoming)==TM_FF_OK);
    /* Original borrowed RTSP callback storage may now be overwritten
     * only AFTER the Provider synchronously copied the packet bytes. */
    assert(tm_pc_begin(portal,20401u,&ticket)==TM_PC_OK);
    req.size=sizeof(req);
    req.instance_id=instance.instance_id;
    req.provider_generation=instance.provider_generation;
    req.portal=portal;req.ticket=ticket;req.wake_client=wake;
    req.test_gate=&gate;
    worker.service=*service;worker.request=req;
    atomic_init(&worker.done,0u);
    assert(pthread_create(&thread,NULL,run_step,&worker)==0);
    assert(tm_installed_wait(&gate.entered));
    assert(atomic_load_explicit(&rtsp.exited,memory_order_acquire)==0u);
    assert(salts_component_plugin_generation_drain(
               runtime,generation)==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(cmeta_plugin_registry_unload(registry,plugin)==CMETA_PLUGIN_BUSY);
    assert(tm_pc_request_cancel(portal,ticket)==TM_PC_OK);
    assert(tm_test_provider_cancel_delayed(service,&instance)==0);
    assert(tm_test_provider_destroy_delayed(service,&instance)==-4);
    tm_installed_rtsp_close(&rtsp);
    atomic_store_explicit(&gate.release,1u,memory_order_release);
    assert(pthread_join(thread,NULL)==0);
    assert(atomic_load_explicit(&gate.exited,memory_order_acquire)==1u);
    assert(worker.rc==TM_FF_ESTATE);
    drain_wake(wake);
    assert(tm_pc_take(portal,&taken,&result)==TM_PC_OK);
    assert(result.correlation==20401u);
    assert(result.provider_generation==102u);
    assert(result.outcome==TM_PC_CANCELLED);
    assert(result.completion_observed==1u);
    assert(result.provider_status==TM_FF_ESTATE);
    assert(tm_pc_settle(portal,taken)==TM_PC_OK);
    assert(tm_pc_take(portal,&taken,&result)!=TM_PC_OK);
    take.size=sizeof(take);
    take.instance_id=instance.instance_id;
    take.provider_generation=instance.provider_generation;
    take.ticket=ticket;
    take.luma=pixels;take.capacity=sizeof(pixels);
    take.out_bytes=&copied;take.out_pts=&pts;
    assert(tm_test_provider_take_delayed(service,&take)==-2);
    assert(copied==0u && pts==INT64_MIN);
    stats_req.size=sizeof(stats_req);
    stats_req.instance_id=instance.instance_id;
    stats_req.provider_generation=instance.provider_generation;
    stats_req.out=&stats;
    assert(tm_test_provider_stats_delayed(service,&stats_req)==TM_FF_OK);
    assert(stats.cancelled && stats.flush_sends==0u && stats.queued==0u);
    assert(tm_test_provider_destroy_delayed(service,&instance)==0);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(salts_component_plugin_generation_drain(
               runtime,generation)==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(cmeta_plugin_registry_unload(registry,plugin)==CMETA_PLUGIN_BUSY);
}
