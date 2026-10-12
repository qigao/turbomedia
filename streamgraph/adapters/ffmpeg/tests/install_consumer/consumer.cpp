#include <turbo_streamgraph_ffmpeg.h>
#include <turbo_streamgraph_ffmpeg_async.h>
#include <type_traits>
static_assert(std::is_standard_layout<tm_ff_config_v1>::value, "C ABI config");
static_assert(std::is_trivially_copyable<tm_ff_stats_v1>::value, "POD stats");
static_assert(std::is_pointer<decltype(&tm_ff_next_cpu_luma)>::value, "Source is native callback");
static_assert(std::is_standard_layout<tm_ffa_stats_v1>::value, "async stats C ABI");
static_assert(std::is_trivially_copyable<tm_pc_ticket_v1>::value, "portal ticket identity");
int main() {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    return cfg.version==TM_FF_ABI_V1 ? 0 : 1;
}
