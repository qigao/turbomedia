#include <turbo_streamgraph.h>
#include <turbo_frame_store.h>
#include <turbo_streamgraph_fanout.h>
#include <turbo_streamgraph_executor.h>
#include <turbo_streamgraph_handoff.h>
#include <turbo_streamgraph_owner_dispatch.h>
#include <type_traits>
static_assert(std::is_standard_layout<tm_sg_port_v1>::value, "public C ABI");
static_assert(std::is_trivially_copyable<tm_sg_edge_v1>::value, "no runtime ownership in Edge");
static_assert(std::is_trivially_copyable<tm_fs_frame_ref_v1>::value, "FrameRef is pointer-free");
static_assert(std::is_standard_layout<tm_fs_pin_v1>::value, "Pin identity has C ABI");
static_assert(std::is_standard_layout<tm_fo_ticket_v1>::value, "fanout ticket C ABI");
static_assert(std::is_trivially_copyable<tm_fo_config_v1>::value, "no pointers in Fanout config");
static_assert(std::is_standard_layout<tm_ex_config_v1>::value, "executor config is C ABI");
static_assert(std::is_trivially_copyable<tm_ho_command_v1>::value, "cross-owner command is a value");
static_assert(std::is_trivially_copyable<tm_ho_ticket_v1>::value, "handoff ticket is an identity");
static_assert(std::is_standard_layout<tm_od_config_v1>::value, "owner dispatcher config is C ABI");
static_assert(std::is_trivially_copyable<tm_od_receipt_v1>::value, "owner receipt is a value");
static_assert(std::is_trivially_copyable<tm_ex_binding_v1>::value, "callback table is POD");
int main() { tm_sg_limits_v1 l = TM_SG_LIMITS_V1_INIT; return l.version == TM_SG_ABI_VERSION ? 0 : 1; }
