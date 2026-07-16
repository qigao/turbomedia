/**
 * TurboMedia Mobile Optimization Helpers
 *
 * Staged mobile-only helpers for battery and network adaptive settings.
 */
#ifndef TURBO_MOBILE_H
#define TURBO_MOBILE_H

#include <stdbool.h>
#include "turbo_export.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API void turbo_mobile_optimizer_update_battery(int level, bool charging);
CXX_C_API void turbo_mobile_optimizer_update_network(const char *type, int signal_strength);

CXX_C_API int turbo_mobile_optimizer_get_target_bitrate(void);
CXX_C_API int turbo_mobile_optimizer_get_target_fps(void);
CXX_C_API int turbo_mobile_optimizer_get_resolution_scale(void);
CXX_C_API bool turbo_mobile_optimizer_should_use_hardware_codec(void);
CXX_C_API bool turbo_mobile_optimizer_should_enable_simulcast(void);
CXX_C_API const char *turbo_mobile_optimizer_get_power_mode_string(void);

CXX_C_API void turbo_battery_monitor_start(void);
CXX_C_API void turbo_battery_monitor_stop(void);
CXX_C_API void turbo_network_monitor_start(void);
CXX_C_API void turbo_network_monitor_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MOBILE_H */
