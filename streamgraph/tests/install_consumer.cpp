#include <turbo_streamgraph.h>
#include <type_traits>
static_assert(std::is_standard_layout<tm_sg_port_v1>::value, "public C ABI");
static_assert(std::is_trivially_copyable<tm_sg_edge_v1>::value, "no runtime ownership in Edge");
int main() { tm_sg_limits_v1 l = TM_SG_LIMITS_V1_INIT; return l.version == TM_SG_ABI_VERSION ? 0 : 1; }
