/* M15b: ONLY link against FFmpeg 6.1.1 patched at the actual
 * avcodec_send_packet() entry. No wrapper/pre-call gate is accepted here.
 * The test proves owner cancellation while worker A is suspended INSIDE
 * libavcodec, B progresses through an independent real decoder, and A's
 * terminal/callback/luma ownership settles exactly once.
 */
#include <turbo_streamgraph_ffmpeg_async.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

/* Test symbols injected into the pinned FFmpeg build, not production APIs.
 * The version-script avcodec_* export pattern makes these linkable. */
extern int avcodec_tm_test_arm_send_packet(void);
extern unsigned avcodec_tm_test_send_packet_state(void);
extern int avcodec_tm_test_release_send_packet(void);

enum { WIDTH=16, HEIGHT=16, LUMA=WIDTH*HEIGHT, MAX_PACKET=65536 };
typedef struct encoder {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *packet;
} encoder;
static void encoder_init(encoder *e) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    assert(codec!=NULL);
    e->ctx=avcodec_alloc_context3(codec);
    assert(e->ctx!=NULL);
    e->ctx->width=WIDTH;e->ctx->height=HEIGHT;
    e->ctx->time_base=(AVRational){1,30};
    e->ctx->pix_fmt=AV_PIX_FMT_YUVJ420P;
    e->ctx->color_range=AVCOL_RANGE_JPEG;
    e->ctx->thread_count=1;
    assert(avcodec_open2(e->ctx,codec,NULL)==0);
    e->frame=av_frame_alloc();e->packet=av_packet_alloc();
    assert(e->frame && e->packet);
    e->frame->width=WIDTH;e->frame->height=HEIGHT;
    e->frame->format=AV_PIX_FMT_YUVJ420P;
    assert(av_frame_get_buffer(e->frame,32)==0);
}
static size_t encode(encoder *e,unsigned level,uint8_t *dst,size_t cap) {
    unsigned y;
    size_t n;
    assert(av_frame_make_writable(e->frame)==0);
    for(y=0u;y<HEIGHT;++y)
        memset(e->frame->data[0]+(size_t)y*e->frame->linesize[0],(int)level,WIDTH);
    for(y=0u;y<HEIGHT/2u;++y) {
        memset(e->frame->data[1]+(size_t)y*e->frame->linesize[1],128,WIDTH/2);
        memset(e->frame->data[2]+(size_t)y*e->frame->linesize[2],128,WIDTH/2);
    }
    e->frame->pts++;
    assert(avcodec_send_frame(e->ctx,e->frame)==0);
    assert(avcodec_receive_packet(e->ctx,e->packet)==0);
    n=(size_t)e->packet->size;
    assert(n && n<=cap);
    memcpy(dst,e->packet->data,n);
    av_packet_unref(e->packet);
    return n;
}
static void encoder_destroy(encoder *e) {
    av_frame_free(&e->frame);
    av_packet_free(&e->packet);
    avcodec_free_context(&e->ctx);
}
typedef struct worker {
    tm_ffa_decoder *decoder;
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    uint8_t packet[MAX_PACKET];
    size_t bytes;
    _Atomic unsigned wake_count;
    tm_ff_status outcome;
} worker;
static int on_wake(void *ctx) {
    worker *w=(worker *)ctx;
    atomic_fetch_add_explicit(&w->wake_count,1u,memory_order_acq_rel);
    return 0;
}
static void *run(void *ctx) {
    worker *w=(worker *)ctx;
    w->outcome=tm_ffa_run_mjpeg(w->decoder,w->packet,w->bytes,
                               w->portal,w->ticket,on_wake,w);
    return NULL;
}
static tm_pc_portal *portal(void) {
    tm_pc_config_v1 cfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *out=NULL;
    cfg.capacity=1u;cfg.provider_generation=188u;
    assert(tm_pc_create(&cfg,&out)==TM_PC_OK);
    return out;
}
int main(void) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_ffa_decoder *a=NULL,*b=NULL;
    tm_pc_portal *pa,*pb;
    tm_pc_ticket_v1 ta={0},tb={0},taken={0};
    tm_pc_result_v1 receipt=TM_PC_RESULT_V1_INIT;
    tm_ff_codec_activity_v1 act=TM_FF_CODEC_ACTIVITY_V1_INIT;
    tm_ffa_stats_v1 stats=TM_FFA_STATS_V1_INIT;
    encoder e={0};
    worker wa={0},wb={0};
    pthread_t thread_a,thread_b;
    uint8_t pixels[LUMA];
    size_t copied=0u;
    unsigned i;

    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=MAX_PACKET;
    assert(tm_ffa_create(&cfg,&a)==TM_FF_OK);
    assert(tm_ffa_create(&cfg,&b)==TM_FF_OK);
    assert(a!=b);
    pa=portal();pb=portal();
    encoder_init(&e);
    wa.decoder=a;wa.portal=pa;
    wb.decoder=b;wb.portal=pb;
    wa.bytes=encode(&e,43u,wa.packet,sizeof(wa.packet));
    wb.bytes=encode(&e,194u,wb.packet,sizeof(wb.packet));
    atomic_init(&wa.wake_count,0u);atomic_init(&wb.wake_count,0u);
    assert(tm_pc_begin(pa,18801u,&ta)==TM_PC_OK);
    assert(tm_pc_begin(pb,18802u,&tb)==TM_PC_OK);
    assert(ta.incarnation!=tb.incarnation &&
           ta.slot==tb.slot && ta.generation==tb.generation);
    wa.ticket=ta;wb.ticket=tb;

    /* Arming happens AFTER the encoder is finished. Thus the first
     * instrumented send call belongs to A's actual decoder. */
    assert(avcodec_tm_test_arm_send_packet()==0);
    assert(pthread_create(&thread_a,NULL,run,&wa)==0);
    for(i=0u;i<5000000u && avcodec_tm_test_send_packet_state()!=2u;++i)
        sched_yield();
    assert(avcodec_tm_test_send_packet_state()==2u);
    assert(tm_ffa_codec_activity(a,&act)==TM_FF_OK);
    assert(act.in_call==1u && act.packet_calls==0u);
    assert(tm_ffa_destroy(a)==TM_FF_ESTATE);

    /* B must progress THROUGH real FFmpeg, not the test gate in A. */
    assert(pthread_create(&thread_b,NULL,run,&wb)==0);
    assert(pthread_join(thread_b,NULL)==0);
    assert(wb.outcome==TM_FF_OK);
    assert(atomic_load_explicit(&wb.wake_count,memory_order_acquire)==1u);
    assert(tm_pc_take(pb,&taken,&receipt)==TM_PC_OK);
    assert(receipt.correlation==18802u && receipt.outcome==TM_PC_COMPLETED &&
           receipt.completion_observed==1u && receipt.provider_status==TM_FF_OK);
    assert(tm_pc_settle(pb,taken)==TM_PC_OK);
    assert(tm_ffa_owner_accept(b,ta)==TM_FF_ESTALE);
    assert(tm_ffa_owner_accept(b,tb)==TM_FF_OK);
    assert(tm_ffa_owner_next_luma(b,pixels,sizeof(pixels),&copied)==TM_EX_SOURCE_FRAME);
    assert(copied==LUMA && abs((int)pixels[0]-194)<=7 &&
           pixels[0]==pixels[LUMA-1u]);
    assert(tm_ffa_owner_next_luma(b,pixels,sizeof(pixels),&copied)==TM_EX_SOURCE_FAILED);

    /* A is STILL INSIDE the real libavcodec function. The Owner changes
     * only Portal and async-decoder metadata, never the Codec context. */
    assert(avcodec_tm_test_send_packet_state()==2u);
    assert(tm_pc_request_cancel(pa,ta)==TM_PC_OK);
    assert(tm_ffa_owner_cancel(a)==TM_FF_OK);
    assert(tm_ffa_destroy(a)==TM_FF_ESTATE);
    assert(tm_ffa_owner_accept(a,ta)==TM_FF_ESTALE);
    assert(avcodec_tm_test_send_packet_state()==2u);
    assert(avcodec_tm_test_release_send_packet()==0);
    assert(pthread_join(thread_a,NULL)==0);
    assert(avcodec_tm_test_send_packet_state()==4u);
    assert(wa.outcome==TM_FF_ESTATE);
    assert(atomic_load_explicit(&wa.wake_count,memory_order_acquire)==1u);

    act=(tm_ff_codec_activity_v1)TM_FF_CODEC_ACTIVITY_V1_INIT;
    assert(tm_ffa_codec_activity(a,&act)==TM_FF_OK);
    assert(!act.in_call && act.packet_calls==1u && act.flush_calls==0u);
    assert(tm_pc_take(pa,&taken,&receipt)==TM_PC_OK);
    assert(receipt.correlation==18801u && receipt.outcome==TM_PC_CANCELLED &&
           receipt.completion_observed==1u &&
           receipt.provider_status==TM_FF_ESTATE);
    assert(tm_pc_settle(pa,taken)==TM_PC_OK);
    assert(tm_pc_take(pa,&taken,&receipt)!=TM_PC_OK);
    assert(tm_ffa_owner_accept(a,ta)==TM_FF_ESTALE);
    assert(tm_ffa_stats(a,&stats)==TM_FF_OK);
    assert(stats.cancelled==1u && stats.decoded==0u &&
           stats.delivered==0u && stats.discarded==1u &&
           stats.worker_active==0u);
    stats=(tm_ffa_stats_v1)TM_FFA_STATS_V1_INIT;
    assert(tm_ffa_stats(b,&stats)==TM_FF_OK);
    assert(stats.decoded==1u && stats.delivered==1u &&
           stats.discarded==0u && !stats.worker_active);

    assert(tm_pc_seal(pa)==TM_PC_OK);
    assert(tm_pc_seal(pb)==TM_PC_OK);
    assert(tm_pc_destroy(pa)==TM_PC_OK);
    assert(tm_pc_destroy(pb)==TM_PC_OK);
    assert(tm_ffa_destroy(a)==TM_FF_OK);
    assert(tm_ffa_destroy(b)==TM_FF_OK);
    encoder_destroy(&e);
    puts("instrumented FFmpeg in-send cancellation and independent B: PASS");
    return 0;
}
