#include <turbo_streamgraph_ffmpeg.h>
#include <turbo_streamgraph_ffmpeg_async.h>
#include <assert.h>

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
        if (tm_ffa_destroy(async)!=TM_FF_OK) return 9;
    }
    return 0;
}
