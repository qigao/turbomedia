/* M15d Linux/macOS/Windows test-only integration. The codec implementation
 * is the canonical desktop tm_ffd Source compiled into this TEST executable,
 * not a second production decoder, scheduler, or RTSP registry.
 *
 * Both RTP packets and RTSP session controls are transported by the real
 * TurboMedia RTSP Server/Client (Salts CNet NativeIO). The test explicitly
 * limits the accepted RFC2250 profile to one complete MPEG-2 Video ES picture
 * per RTP packet (PT 32, T=0, B=E=M=1), fixed 90k/30 PTS clock, no
 * fragmentation/reassembly, and one fixed SSRC/session generation.
 */
#include <turbo_streamgraph_ffmpeg_delayed.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <limits.h>

enum {
    RTSP_MPV_WIDTH=64, RTSP_MPV_HEIGHT=64,
    RTSP_MPV_LUMA=RTSP_MPV_WIDTH*RTSP_MPV_HEIGHT,
    RTSP_MPV_FRAMES=12, RTSP_MPV_MAX_PACKET=16384,
    RTSP_MPV_RTP_SEQUENCE=300
};
typedef struct rtsp_mpv_fixture {
    AVCodecContext *encoder;
    AVFrame *frame;
    AVPacket *scratch;
    AVPacket *packets[RTSP_MPV_FRAMES];
    unsigned packets_count;
    unsigned encoded_before_flush;
    unsigned display_frames;
    unsigned eagain;
    unsigned backpressure;
} rtsp_mpv_fixture;

static unsigned rtsp_mpv_level(unsigned i) { return 36u+13u*i; }

static void rtsp_mpv_collect(rtsp_mpv_fixture *f,int final) {
    for(;;) {
        int rc=avcodec_receive_packet(f->encoder,f->scratch);
        if(rc==AVERROR(EAGAIN)) {
            check_true(!final);
            return;
        }
        if(rc==AVERROR_EOF) {
            check_true(final);
            return;
        }
        check_equal(rc,0);
        check_true(f->packets_count<RTSP_MPV_FRAMES);
        if(f->packets_count>=RTSP_MPV_FRAMES)return;
        f->packets[f->packets_count]=av_packet_clone(f->scratch);
        check_not_null(f->packets[f->packets_count]);
        ++f->packets_count;
        av_packet_unref(f->scratch);
    }
}
static void rtsp_mpv_fixture_make(rtsp_mpv_fixture *f) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    unsigned i,y,reordered=0;
    int64_t previous=INT64_MIN;
    check_not_null(codec);
    f->encoder=avcodec_alloc_context3(codec);
    check_not_null(f->encoder);
    f->encoder->width=RTSP_MPV_WIDTH;f->encoder->height=RTSP_MPV_HEIGHT;
    f->encoder->time_base=(AVRational){1,30};
    f->encoder->framerate=(AVRational){30,1};
    f->encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    f->encoder->bit_rate=1200000;
    f->encoder->gop_size=12;
    f->encoder->max_b_frames=2;
    f->encoder->thread_count=1;
    check_equal(avcodec_open2(f->encoder,codec,NULL),0);
    f->frame=av_frame_alloc();f->scratch=av_packet_alloc();
    check_not_null(f->frame);check_not_null(f->scratch);
    f->frame->width=RTSP_MPV_WIDTH;f->frame->height=RTSP_MPV_HEIGHT;
    f->frame->format=AV_PIX_FMT_YUV420P;
    check_equal(av_frame_get_buffer(f->frame,32),0);
    for(i=0u;i<RTSP_MPV_FRAMES;++i) {
        check_equal(av_frame_make_writable(f->frame),0);
        for(y=0u;y<RTSP_MPV_HEIGHT;++y)
            memset(f->frame->data[0]+(size_t)y*f->frame->linesize[0],
                   (int)rtsp_mpv_level(i),RTSP_MPV_WIDTH);
        for(y=0u;y<RTSP_MPV_HEIGHT/2u;++y) {
            memset(f->frame->data[1]+(size_t)y*f->frame->linesize[1],128,
                   RTSP_MPV_WIDTH/2);
            memset(f->frame->data[2]+(size_t)y*f->frame->linesize[2],128,
                   RTSP_MPV_WIDTH/2);
        }
        f->frame->pts=(int64_t)i;
        check_equal(avcodec_send_frame(f->encoder,f->frame),0);
        rtsp_mpv_collect(f,0);
    }
    f->encoded_before_flush=f->packets_count;
    check_true(f->encoded_before_flush<RTSP_MPV_FRAMES);
    check_equal(avcodec_send_frame(f->encoder,NULL),0);
    rtsp_mpv_collect(f,1);
    check_equal(f->packets_count,RTSP_MPV_FRAMES);
    for(i=0u;i<f->packets_count;++i) {
        const AVPacket *pkt=f->packets[i];
        check_true(pkt->size>8 && pkt->size<RTSP_MPV_MAX_PACKET-16);
        check_true(pkt->pts!=AV_NOPTS_VALUE);
        if(pkt->pts<previous)++reordered;
        previous=pkt->pts;
    }
    check_true(reordered>0u); /* genuine B-picture output reorder */
}
static void rtsp_mpv_fixture_destroy(rtsp_mpv_fixture *f) {
    unsigned i;
    for(i=0u;i<f->packets_count;++i)av_packet_free(&f->packets[i]);
    av_packet_free(&f->scratch);
    av_frame_free(&f->frame);
    avcodec_free_context(&f->encoder);
}

static int rtsp_mpv_picture_header(const AVPacket *pkt,
                                   uint16_t *tr,uint8_t *type,
                                   uint8_t *sequence) {
    size_t i;
    *sequence=0u;
    for(i=0u;i+5u<(size_t)pkt->size;++i) {
        if(pkt->data[i]||pkt->data[i+1]||pkt->data[i+2]!=1u)continue;
        if(pkt->data[i+3]==0xb3u)*sequence=1u;
        if(pkt->data[i+3]==0u) {
            *tr=(uint16_t)(((uint16_t)pkt->data[i+4]<<2)|
                            ((uint16_t)pkt->data[i+5]>>6));
            *type=(uint8_t)((pkt->data[i+5]>>3)&0x07u);
            return *type>=1u && *type<=3u ? 0:-1;
        }
    }
    return -1;
}
static int rtsp_mpv_make_rtp(const AVPacket *pkt,uint16_t seq,uint32_t ssrc,
                             uint8_t *wire,size_t cap,size_t *wire_len) {
    turbo_rtsp_rtp_header_t rtp={0};
    uint16_t tr=0;
    uint8_t picture_type=0,seq_header=0;
    uint8_t es[RTSP_MPV_MAX_PACKET];
    int n;
    if(!pkt || pkt->size<=0 || (size_t)pkt->size>sizeof(es)-4u ||
       pkt->pts<0 || pkt->pts>INT32_MAX/3000 ||
       rtsp_mpv_picture_header(pkt,&tr,&picture_type,&seq_header)!=0)
        return -1;
    es[0]=(uint8_t)((tr>>8)&3u); /* MBZ=0, T=0 */
    es[1]=(uint8_t)(tr&255u);
    es[2]=(uint8_t)((seq_header?0x20u:0u)|0x18u|picture_type);
    es[3]=0u; /* no codec-resilience extension in the tested profile */
    memcpy(es+4,pkt->data,(size_t)pkt->size);
    rtp.version=2u;
    rtp.payload_type=TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE;
    rtp.marker=1u;
    rtp.sequence_number=seq;
    rtp.timestamp=(uint32_t)pkt->pts*3000u;
    rtp.ssrc=ssrc;
    n=turbo_rtsp_rtp_write_packet(wire,cap,&rtp,es,
                                 (size_t)pkt->size+4u);
    if(n<=0)return -1;
    *wire_len=(size_t)n;
    return 0;
}
/* No public bridge has been shipped yet. This deliberately narrow test
 * admission gate validates a ONE-packet RFC2250 MPEG Video ES profile and
 * transfers one copied packet into the actual bounded tm_ffd implementation.
 * A TS (PT=33), H.264 (PT=96), fragment or different Session Generation is
 * never admitted; no hidden depacketizer/reassembly fallback is present. */
static int rtsp_mpv_admit_wire(tm_ffd_decoder *d,uint64_t expected_epoch,
                               uint64_t epoch,uint8_t channel,
                               const uint8_t *wire,size_t length,
                               uint16_t seq,uint32_t ssrc) {
    turbo_rtsp_rtp_header_t rtp;
    turbo_rtsp_mpeg_video_payload_t video;
    tm_ffd_packet_v1 packet=TM_FFD_PACKET_V1_INIT;
    size_t header_size=0u;
    if(epoch!=expected_epoch || channel!=1u || !d ||
       turbo_rtsp_rtp_parse_header(wire,length,&rtp,&header_size)!=0 ||
       rtp.payload_type!=TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE ||
       rtp.sequence_number!=seq || rtp.ssrc!=ssrc || !rtp.marker ||
       rtp.timestamp%3000u || rtp.extension ||
       turbo_rtsp_mpeg_video_payload_parse(rtp.payload,rtp.payload_len,
                                           &video)!=0 ||
       turbo_rtsp_mpeg_video_one_picture_validate(&video,rtp.marker)!=0)
        return -1;
    packet.data=video.es;packet.bytes=video.es_len;
    packet.pts=(int64_t)(rtp.timestamp/3000u);
    /* RFC2250 carries presentation timestamps, NOT decode timestamps.
     * Never invent a DTS from transport sequence or arrival order. */
    packet.dts=AV_NOPTS_VALUE;
    return (int)tm_ffd_submit(d,&packet);
}
static void rtsp_mpv_drain(tm_ffd_decoder *decoder,rtsp_mpv_fixture *fixture,
                            unsigned *eof) {
    unsigned spins;
    for(spins=0u;spins<2u*RTSP_MPV_FRAMES;++spins) {
        uint8_t pixels[RTSP_MPV_LUMA];
        size_t bytes=0u;
        int64_t pts=INT64_MIN;
        tm_ff_status status=tm_ffd_next_luma(
            decoder,pixels,sizeof(pixels),&bytes,&pts);
        if(status==TM_FF_EAGAIN) {
            ++fixture->eagain;
            return;
        }
        if(status==TM_FF_EOF) {
            *eof=1u;
            return;
        }
        check_equal(status,TM_FF_OK);
        check_equal(bytes,RTSP_MPV_LUMA);
        check_equal(pts,(int64_t)fixture->display_frames);
        check_true(fixture->display_frames<RTSP_MPV_FRAMES);
        if(fixture->display_frames<RTSP_MPV_FRAMES) {
            unsigned level=rtsp_mpv_level(fixture->display_frames);
            check_true(abs((int)pixels[0]-(int)level)<=22);
            check_true(abs((int)pixels[RTSP_MPV_LUMA-1]-(int)level)<=22);
        }
        ++fixture->display_frames;
    }
    check_true(0); /* never an unbounded internal drain loop */
}
static tm_ffd_decoder *rtsp_mpv_make_decoder(void) {
    tm_ffd_config_v1 cfg=TM_FFD_CONFIG_V1_INIT;
    tm_ffd_decoder *decoder=NULL;
    cfg.width=RTSP_MPV_WIDTH;cfg.height=RTSP_MPV_HEIGHT;
    cfg.queue_capacity=2u;cfg.max_packet_bytes=RTSP_MPV_MAX_PACKET;
    check_equal(tm_ffd_create(&cfg,&decoder),TM_FF_OK);
    return decoder;
}
static int rtsp_mpv_begin_record(turbo_rtsp_client_t *client) {
    static const char sdp[] =
        "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=mpeg2-es\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 32\r\na=rtpmap:32 MPV/90000\r\n"
        "a=control:trackID=0\r\n";
    const turbo_rtsp_push_announce_t a={
        "rtsp://127.0.0.1/live/mpeg2-es",sdp,sizeof(sdp)-1u};
    const turbo_rtsp_push_track_t track={
        "rtsp://127.0.0.1/live/mpeg2-es/trackID=0",0u,1u};
    return turbo_rtsp_client_announce(client,&a)==0 &&
           turbo_rtsp_client_setup_interleaved(client,&track)==0 &&
           turbo_rtsp_client_record(client)==0;
}
static void rtsp_mpv_wire_exchange(turbo_rtsp_client_t *client,
                                    const AVPacket *encoded,uint16_t seq,
                                    uint32_t ssrc,
                                    uint8_t *wire,size_t *bytes) {
    uint8_t tx[RTSP_MPV_MAX_PACKET];
    size_t tx_len=0u;
    uint8_t channel=0xffu;
    check_equal(rtsp_mpv_make_rtp(encoded,seq,ssrc,tx,sizeof(tx),&tx_len),0);
    check_equal(turbo_rtsp_client_send_interleaved_frame(
        client,0u,tx,tx_len),0);
    check_equal(turbo_rtsp_client_recv_interleaved_frame(
        client,&channel,wire,RTSP_MPV_MAX_PACKET,bytes),0);
    check_equal(channel,1u);
    check_equal(*bytes,tx_len);
    check_equal(memcmp(wire,tx,tx_len),0);
}

static void rtsp_test_mpeg2_video_real_rtp_to_delayed_codec(void) {
    rtsp_test_state_t state;
    rtsp_mpv_fixture fixture={0};
    turbo_rtsp_server_t *server=NULL;
    turbo_rtsp_client_t *client=NULL;
    tm_ffd_decoder *decoder=NULL;
    tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
    uint8_t wire[RTSP_MPV_MAX_PACKET]={0};
    size_t bytes=0u;
    unsigned i,eof=0;
    const uint64_t first_epoch=19501u,second_epoch=19502u;
    const uint32_t first_ssrc=0x11223344u,second_ssrc=0x55667788u;

    rtsp_test_state_init(&state);
    rtsp_mpv_fixture_make(&fixture);
    server=rtsp_test_server_start(&state,TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                  20562,NULL);
    check_not_null(server);
    if(!server)goto cleanup;
    client=rtsp_test_client_connect(TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                    20562,NULL);
    check_not_null(client);
    if(!client)goto cleanup;
    check_true(rtsp_mpv_begin_record(client));
    decoder=rtsp_mpv_make_decoder();
    if(!decoder)goto cleanup;

    for(i=0u;i<fixture.packets_count;++i) {
        int admitted;
        rtsp_mpv_wire_exchange(client,fixture.packets[i],
                               (uint16_t)(RTSP_MPV_RTP_SEQUENCE+i),
                               first_ssrc,wire,&bytes);
        check_equal(rtsp_mpv_admit_wire(decoder,first_epoch,first_epoch,
                                       1u,wire,bytes,
                                       (uint16_t)(RTSP_MPV_RTP_SEQUENCE+i),
                                       first_ssrc),TM_FF_OK);
        /* The Source must own a deep bounded compressed COPY. */
        memset(wire,0,bytes);
        rtsp_mpv_drain(decoder,&fixture,&eof);
        check_equal(eof,0u);
        admitted=(int)fixture.display_frames;
        check_true(admitted<=RTSP_MPV_FRAMES);
    }
    check_equal(tm_ffd_finish(decoder),TM_FF_OK);
    rtsp_mpv_drain(decoder,&fixture,&eof);
    check_equal(eof,1u);
    check_equal(fixture.display_frames,RTSP_MPV_FRAMES);
    check_equal(tm_ffd_stats(decoder,&stats),TM_FF_OK);
    check_equal(stats.packets_admitted,RTSP_MPV_FRAMES);
    check_equal(stats.packets_sent,RTSP_MPV_FRAMES);
    check_equal(stats.frames_decoded,RTSP_MPV_FRAMES);
    check_true(stats.frames_after_flush>=1u);
    check_equal(stats.flush_sends,1u);
    check_equal(stats.queued,0u);
    check_equal(tm_ffd_destroy(decoder),TM_FF_OK);
    decoder=NULL;
    check_equal(turbo_rtsp_client_teardown(client),0);
    turbo_rtsp_client_destroy(client);client=NULL;

    /* New RTSP Session + new Decoder Generation. Deliberately admit
     * only a few real packets, then abort without TEARDOWN/NULL flush.
     * The old Session epoch must be rejected before Codec admission. */
    client=rtsp_test_client_connect(TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                    20562,NULL);
    check_not_null(client);
    if(!client)goto cleanup;
    check_true(rtsp_mpv_begin_record(client));
    decoder=rtsp_mpv_make_decoder();
    if(!decoder)goto cleanup;
    for(i=0u;i<2u;++i) {
        rtsp_mpv_wire_exchange(client,fixture.packets[i],
                               (uint16_t)(RTSP_MPV_RTP_SEQUENCE+i),
                               second_ssrc,wire,&bytes);
        check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,first_epoch,
                                       1u,wire,bytes,
                                       (uint16_t)(RTSP_MPV_RTP_SEQUENCE+i),
                                       second_ssrc),-1);
        check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                                       0u,wire,bytes,
                                       (uint16_t)(RTSP_MPV_RTP_SEQUENCE+i),
                                       second_ssrc),-1);
        if(i==0u) {
            uint8_t modified[RTSP_MPV_MAX_PACKET];
            tm_ffd_stats_v1 before=TM_FFD_STATS_V1_INIT;
            memcpy(modified,wire,bytes);
            /* The admitted MPEG-2 ES Source may not accept TS PT33,
             * H.264 PT96, incomplete picture fragments, optional
             * unsupported RFC2250 extensions or wrong SSRC. */
            modified[1]=(uint8_t)((modified[1]&0x80u)|33u);
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            modified[1]=(uint8_t)((modified[1]&0x80u)|96u);
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            memcpy(modified,wire,bytes);
            modified[1]=(uint8_t)(modified[1]&0x7fu); /* no marker */
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            memcpy(modified,wire,bytes);
            modified[TURBO_RTSP_RTP_HEADER_SIZE+2u] &= (uint8_t)~0x10u;
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            memcpy(modified,wire,bytes);
            /* Parser can recognize slices, but it MUST cross-check the
             * elementary Picture Header's actual TR/Picture Type before
             * granting bounded Codec ingress. */
            modified[TURBO_RTSP_RTP_HEADER_SIZE+1u] ^= 0x01u;
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            memcpy(modified,wire,bytes);
            modified[TURBO_RTSP_RTP_HEADER_SIZE+2u] ^= 0x01u;
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            memcpy(modified,wire,bytes);
            modified[TURBO_RTSP_RTP_HEADER_SIZE] |= 0x04u; /* T=1 */
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,modified,bytes,RTSP_MPV_RTP_SEQUENCE,
                            second_ssrc),-1);
            check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                            1u,wire,bytes,RTSP_MPV_RTP_SEQUENCE,
                            first_ssrc),-1);
            check_equal(tm_ffd_stats(decoder,&before),TM_FF_OK);
            check_equal(before.packets_admitted,0u);
            check_equal(before.flush_sends,0u);
        }
        check_equal(rtsp_mpv_admit_wire(decoder,second_epoch,second_epoch,
                                       1u,wire,bytes,
                                       (uint16_t)(RTSP_MPV_RTP_SEQUENCE+i),
                                       second_ssrc),TM_FF_OK);
        memset(wire,0,bytes);
    }
    /* Close actual NativeIO transport before exclusive Codec cancellation.
     * No hidden teardown, decoder flush, secondary Actor, or retry. */
    turbo_rtsp_client_close(client);
    check_equal(tm_ffd_cancel(decoder),TM_FF_OK);
    stats=(tm_ffd_stats_v1)TM_FFD_STATS_V1_INIT;
    check_equal(tm_ffd_stats(decoder,&stats),TM_FF_OK);
    check_equal(stats.cancelled,1u);
    check_equal(stats.queued,0u);
    check_equal(stats.flush_sends,0u);
    check_equal(stats.frames_decoded,0u);
    check_equal(tm_ffd_destroy(decoder),TM_FF_OK);
    decoder=NULL;
    check_equal(rtsp_test_read(&state,&state.record_count),2);
    check_equal(rtsp_test_read(&state,&state.teardown_count),1);
cleanup:
    if(decoder) {
        (void)tm_ffd_cancel(decoder);
        (void)tm_ffd_destroy(decoder);
    }
    if(client)turbo_rtsp_client_destroy(client);
    if(server)turbo_rtsp_server_destroy(server);
    rtsp_mpv_fixture_destroy(&fixture);
    rtsp_test_state_destroy(&state);
}
