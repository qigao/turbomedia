#include <turbo_streamgraph.h>
#include <turbo_frame_store.h>
#include <turbo_streamgraph_fanout.h>
#include <type_traits>
static_assert(std::is_standard_layout<tm_sg_port_v1>::value, "public C ABI");
static_assert(std::is_trivially_copyable<tm_sg_edge_v1>::value, "no runtime ownership in Edge");
static_assert(std::is_trivially_copyable<tm_fs_frame_ref_v1>::value, "FrameRef is pointer-free");
static_assert(std::is_standard_layout<tm_fs_pin_v1>::value, "Pin identity has C ABI");
static_assert(std::is_standard_layout<tm_fo_ticket_v1>::value, "fanout ticket C ABI");
static_assert(std::is_trivially_copyable<tm_fo_config_v1>::value, "no pointers in Fanout config");
int main() { tm_sg_limits_v1 l = TM_SG_LIMITS_V1_INIT; return l.version == TM_SG_ABI_VERSION ? 0 : 1; }
