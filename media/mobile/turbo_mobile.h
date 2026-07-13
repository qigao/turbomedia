/**
 * TurboMedia Mobile Optimization Helpers
 *
 * Staged mobile-only helpers for battery and network adaptive settings.
 */
#ifndef TURBO_MOBILE_H
#define TURBO_MOBILE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void turbo_mobile_optimizer_update_battery(int level, bool charging);
void turbo_mobile_optimizer_update_network(const char *type, int signal_strength);

int turbo_mobile_optimizer_get_target_bitrate(void);
int turbo_mobile_optimizer_get_target_fps(void);
int turbo_mobile_optimizer_get_resolution_scale(void);
bool turbo_mobile_optimizer_should_use_hardware_codec(void);
bool turbo_mobile_optimizer_should_enable_simulcast(void);
const char *turbo_mobile_optimizer_get_power_mode_string(void);

void turbo_battery_monitor_start(void);
void turbo_battery_monitor_stop(void);
void turbo_network_monitor_start(void);
void turbo_network_monitor_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MOBILE_H */
