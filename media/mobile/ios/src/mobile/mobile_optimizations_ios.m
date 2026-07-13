/**
 * Mobile Optimizations for iOS
 * Battery-aware, network-adaptive streaming
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <Network/Network.h>
#include "turbo_mobile.h"
#include <string.h>

typedef enum {
    POWER_MODE_NORMAL,
    POWER_MODE_LOW_POWER,
    POWER_MODE_ULTRA_LOW_POWER
} power_mode_t;

typedef enum {
    NETWORK_WIFI,
    NETWORK_CELLULAR_5G,
    NETWORK_CELLULAR_4G,
    NETWORK_CELLULAR_3G,
    NETWORK_UNKNOWN
} network_type_t;

typedef struct {
    power_mode_t power_mode;
    network_type_t network_type;
    int battery_level;
    bool is_charging;
    bool low_power_mode;
    
    // Adaptive settings
    int target_bitrate;
    int target_fps;
    int target_resolution_scale; // 100 = full, 50 = half
    bool enable_hardware_codec;
    bool enable_simulcast;
} mobile_optimizer_t;

static mobile_optimizer_t g_optimizer = {
    .power_mode = POWER_MODE_NORMAL,
    .network_type = NETWORK_WIFI,
    .battery_level = 100,
    .is_charging = false,
    .low_power_mode = false,
    .target_bitrate = 1500000,
    .target_fps = 30,
    .target_resolution_scale = 100,
    .enable_hardware_codec = true,
    .enable_simulcast = true
};

static void update_optimization_settings(mobile_optimizer_t* opt) {
    @autoreleasepool {
        // Check iOS low power mode
        opt->low_power_mode = [[NSProcessInfo processInfo] isLowPowerModeEnabled];
        
        // Determine power mode
        if (opt->low_power_mode || (opt->battery_level < 15 && !opt->is_charging)) {
            opt->power_mode = POWER_MODE_ULTRA_LOW_POWER;
        } else if (opt->battery_level < 30 && !opt->is_charging) {
            opt->power_mode = POWER_MODE_LOW_POWER;
        } else {
            opt->power_mode = POWER_MODE_NORMAL;
        }
        
        // Adjust settings based on power mode and network
        switch (opt->power_mode) {
            case POWER_MODE_ULTRA_LOW_POWER:
                opt->target_fps = 15;
                opt->target_resolution_scale = 50;
                opt->enable_simulcast = false;
                
                if (opt->network_type == NETWORK_WIFI) {
                    opt->target_bitrate = 500000;
                } else {
                    opt->target_bitrate = 300000;
                }
                break;
            
            case POWER_MODE_LOW_POWER:
                opt->target_fps = 24;
                opt->target_resolution_scale = 75;
                opt->enable_simulcast = false;
                
                if (opt->network_type == NETWORK_WIFI) {
                    opt->target_bitrate = 800000;
                } else {
                    opt->target_bitrate = 500000;
                }
                break;
            
            case POWER_MODE_NORMAL:
            default:
                opt->target_fps = 30;
                opt->target_resolution_scale = 100;
                opt->enable_simulcast = true;
                
                switch (opt->network_type) {
                    case NETWORK_WIFI:
                        opt->target_bitrate = 2000000;
                        break;
                    case NETWORK_CELLULAR_5G:
                        opt->target_bitrate = 1500000;
                        break;
                    case NETWORK_CELLULAR_4G:
                        opt->target_bitrate = 1000000;
                        break;
                    case NETWORK_CELLULAR_3G:
                        opt->target_bitrate = 500000;
                        break;
                    default:
                        opt->target_bitrate = 800000;
                        break;
                }
                break;
        }
        
        NSLog(@"[MobileOpt] Updated: power=%d, network=%d, battery=%d%%, bitrate=%d, fps=%d, scale=%d%%",
              opt->power_mode, opt->network_type, opt->battery_level,
              opt->target_bitrate, opt->target_fps, opt->target_resolution_scale);
    }
}

void turbo_mobile_optimizer_update_battery(int level, bool charging) {
    g_optimizer.battery_level = level;
    g_optimizer.is_charging = charging;
    update_optimization_settings(&g_optimizer);
}

void turbo_mobile_optimizer_update_network(const char* type, int signal_strength) {
    (void)signal_strength;

    @autoreleasepool {
        if (strcmp(type, "WiFi") == 0) {
            g_optimizer.network_type = NETWORK_WIFI;
        } else if (strcmp(type, "5G") == 0) {
            g_optimizer.network_type = NETWORK_CELLULAR_5G;
        } else if (strcmp(type, "4G") == 0 || strcmp(type, "LTE") == 0) {
            g_optimizer.network_type = NETWORK_CELLULAR_4G;
        } else if (strcmp(type, "3G") == 0) {
            g_optimizer.network_type = NETWORK_CELLULAR_3G;
        } else {
            g_optimizer.network_type = NETWORK_UNKNOWN;
        }
        
        update_optimization_settings(&g_optimizer);
    }
}

int turbo_mobile_optimizer_get_target_bitrate(void) {
    return g_optimizer.target_bitrate;
}

int turbo_mobile_optimizer_get_target_fps(void) {
    return g_optimizer.target_fps;
}

int turbo_mobile_optimizer_get_resolution_scale(void) {
    return g_optimizer.target_resolution_scale;
}

bool turbo_mobile_optimizer_should_use_hardware_codec(void) {
    return g_optimizer.enable_hardware_codec;
}

bool turbo_mobile_optimizer_should_enable_simulcast(void) {
    return g_optimizer.enable_simulcast;
}

const char* turbo_mobile_optimizer_get_power_mode_string(void) {
    switch (g_optimizer.power_mode) {
        case POWER_MODE_ULTRA_LOW_POWER: return "Ultra Low Power";
        case POWER_MODE_LOW_POWER: return "Low Power";
        case POWER_MODE_NORMAL: return "Normal";
        default: return "Unknown";
    }
}
