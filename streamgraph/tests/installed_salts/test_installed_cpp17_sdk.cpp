/* M16 #225: independent installed-SDK C++17 link and ABI consumer.
 * This is intentionally NOT a plugin loader or codec execution wrapper.
 * Actual Generation N->N+1 / cancellation / unload run in the C11 DSO test;
 * this gate checks the same exact public SDK is consumable by an unrelated
 * C++17 binary, not by internal build-tree include directories. */
#include <salts/component_plugin.h>
#include <salts/plugin.h>
#include <cnet/cnet.h>
#include <turbo_streamgraph.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <cstdio>

static_assert(__cplusplus >= 201703L, "Installed SDK requires C++17");
static_assert(TM_SG_ABI_VERSION == 1u, "Unexpected StreamGraph ABI version");
static_assert(std::is_standard_layout<tm_sg_port_v1>::value,
              "C11 StreamGraph Port must have C++17 standard layout");
static_assert(std::is_standard_layout<salts_component_plugin_scope>::value,
              "C11 ComponentPlugin Scope must have C++17 standard layout");
static_assert(sizeof(cnet_client) == sizeof(void *),
              "CNet owner wrapper layout changed unexpectedly");

int main() {
    const auto status = salts_component_plugin_status_string(
        SALTS_COMPONENT_PLUGIN_BUSY);
    if (status == nullptr || status[0] == '\0') return 1;

    /* Calling the actual installed Core/Plugin C symbols verifies that the
     * C++17 consumer resolves the canonical published Salts targets. */
    if (salts_component_plugin_generation_drain(nullptr, nullptr)
        != SALTS_COMPONENT_PLUGIN_INVALID_ARGUMENT) return 2;

    /* Never initialize a fake CNet Owner or forge a NativeIO handle.
     * This null owner is invalid and must be rejected without side effects. */
    cnet_client client{};
    if (client.impl != nullptr) return 3;
    if (cnet_client_wake(&client) == 0) return 4;

    tm_sg_limits_v1 limits = TM_SG_LIMITS_V1_INIT;
    tm_sg_diagnostic_v1 diagnostic = TM_SG_DIAGNOSTIC_V1_INIT;
    tm_sg_program *program = nullptr;
    const tm_sg_status code =
        tm_sg_compile(nullptr, 0u, nullptr, 0u,
                      &limits, &program, &diagnostic);
    /* Empty graphs are either rejected or compiled to a valid empty plan;
     * neither outcome should return a dangling partially built program. */
    if (code == TM_SG_OK) {
        if (program == nullptr) return 5;
        tm_sg_program_destroy(program);
    } else if (program != nullptr) {
        return 6;
    }

    std::puts("exact installed Salts ComponentPlugin/CNet + StreamGraph C++17 SDK consumer PASS");
    return 0;
}
