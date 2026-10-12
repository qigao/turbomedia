#include <turbo_streamgraph_ffmpeg.h>
#include <turbo_streamgraph_ffmpeg_async.h>
#include <assert.h>

static int count_wake(void *ctx) {
    unsigned *count=(unsigned *)ctx;
    ++*count;
    return 0;
}

int main(void) {
    tm_ff_config_v1 config=TM_FF_CONFIG_V1_INIT;
    tm_ff_stats_v1 stats=TM_FF_STATS_V1_INIT;
    tm_ff_source *source=NULL;
    int ready=-1;
    config.width=16u; config.height=16u;
    config.max_packet_bytes=4096u;
    if (tm_ff_create(&config,&source)!=TM_FF_OK) return 1;
    if (tm_ff_ready(source,&ready)!=TM_FF_OK || ready!=0) return 2;
    if (tm_ff_finish(source)!=TM_FF_OK) return 3;
    if (tm_ff_ready(source,&ready)!=TM_FF_OK || ready!=1) return 4;
    if (tm_ff_stats(source,&stats)!=TM_FF_OK || stats.frames_decoded) return 5;
    if (tm_ff_cancel(source)!=TM_FF_OK ||
        tm_ff_destroy(source)!=TM_FF_OK) return 6;
    {
        tm_ffa_decoder *async=NULL;
        tm_ffa_stats_v1 a=TM_FFA_STATS_V1_INIT;
        if (tm_ffa_create(&config,&async)!=TM_FF_OK) return 7;
        if (tm_ffa_stats(async,&a)!=TM_FF_OK || a.decoded || a.worker_active) return 8;
        {
            tm_pc_config_v1 pconfig=TM_PC_CONFIG_V1_INIT;
            tm_pc_portal *portal=NULL;
            tm_pc_ticket_v1 ticket={0},taken={0};
            tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
            tm_ff_codec_activity_v1 activity=TM_FF_CODEC_ACTIVITY_V1_INIT;
            unsigned wakes=0u;
            pconfig.capacity=1u;pconfig.provider_generation=195u;
            if(tm_pc_create(&pconfig,&portal)!=TM_PC_OK)return 10;
            if(tm_pc_begin(portal,19501u,&ticket)!=TM_PC_OK)return 11;
            /* Installed, independently linked SDK: real one-shot NULL
             * flush, original Portal terminal, and one wake callback. */
            if(tm_ffa_run_eof(async,portal,ticket,count_wake,&wakes)!=TM_FF_EOF)
                return 12;
            if(wakes!=1u)return 13;
            if(tm_pc_take(portal,&taken,&result)!=TM_PC_OK ||
               result.correlation!=19501u ||
               result.outcome!=TM_PC_COMPLETED ||
               result.completion_observed!=1u ||
               result.provider_status!=TM_FF_EOF)return 14;
            if(tm_pc_settle(portal,taken)!=TM_PC_OK)return 15;
            if(tm_ffa_run_eof(async,portal,ticket,count_wake,&wakes)!=TM_FF_ESTATE ||
               wakes!=1u)return 16;
            if(tm_pc_take(portal,&taken,&result)==TM_PC_OK)return 17;
            if(tm_ffa_codec_activity(async,&activity)!=TM_FF_OK ||
               activity.in_call || activity.flush_calls!=1u ||
               activity.packet_calls!=0u)return 18;
            if(tm_pc_seal(portal)!=TM_PC_OK ||
               tm_pc_destroy(portal)!=TM_PC_OK)return 19;
        }
        if (tm_ffa_destroy(async)!=TM_FF_OK) return 9;
    }
    return 0;
}
