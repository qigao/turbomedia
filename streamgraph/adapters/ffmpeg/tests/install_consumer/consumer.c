#include <turbo_streamgraph_ffmpeg.h>
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
    return 0;
}
