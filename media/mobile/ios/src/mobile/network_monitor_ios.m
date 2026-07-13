/**
 * Network Monitor for iOS
 * Monitors network type and connectivity using Network framework
 */

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#include "turbo_mobile.h"

static nw_path_monitor_t pathMonitor = NULL;
static dispatch_queue_t monitorQueue = NULL;

static const char* get_network_type(nw_path_t path) {
    if (nw_path_uses_interface_type(path, nw_interface_type_wifi)) {
        return "WiFi";
    } else if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) {
        // Try to determine cellular generation
        // Note: iOS doesn't provide direct API for this, so we estimate
        return "4G"; // Default to 4G for cellular
    } else if (nw_path_uses_interface_type(path, nw_interface_type_wired)) {
        return "Ethernet";
    } else {
        return "Unknown";
    }
}

void turbo_network_monitor_start(void) {
    @autoreleasepool {
        if (pathMonitor) {
            NSLog(@"[NetworkMonitor] Already running");
            return;
        }
        
        monitorQueue = dispatch_queue_create("com.turbonet.network.monitor", DISPATCH_QUEUE_SERIAL);
        pathMonitor = nw_path_monitor_create();
        
        nw_path_monitor_set_queue(pathMonitor, monitorQueue);
        
        nw_path_monitor_set_update_handler(pathMonitor, ^(nw_path_t path) {
            nw_path_status_t status = nw_path_get_status(path);
            
            if (status == nw_path_status_satisfied) {
                const char* networkType = get_network_type(path);
                
                bool isExpensive = nw_path_is_expensive(path);
                bool isConstrained = nw_path_is_constrained(path);
                
                NSLog(@"[NetworkMonitor] Network: %s (expensive=%d, constrained=%d)",
                      networkType, isExpensive, isConstrained);
                
                turbo_mobile_optimizer_update_network(networkType, 100);
            } else {
                NSLog(@"[NetworkMonitor] Network unavailable");
                turbo_mobile_optimizer_update_network("Unknown", 0);
            }
        });
        
        nw_path_monitor_start(pathMonitor);
        
        NSLog(@"[NetworkMonitor] Started");
    }
}

void turbo_network_monitor_stop(void) {
    @autoreleasepool {
        if (pathMonitor) {
            nw_path_monitor_cancel(pathMonitor);
            pathMonitor = NULL;
        }
        
        if (monitorQueue) {
            monitorQueue = NULL;
        }
        
        NSLog(@"[NetworkMonitor] Stopped");
    }
}
