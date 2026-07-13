/**
 * Battery Monitor for iOS
 * Monitors battery level and charging state using UIDevice
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include "turbo_mobile.h"

static id batteryLevelObserver = nil;
static id batteryStateObserver = nil;

static void update_battery_status(void) {
    @autoreleasepool {
        UIDevice* device = [UIDevice currentDevice];
        device.batteryMonitoringEnabled = YES;
        
        int level = (int)(device.batteryLevel * 100);
        bool charging = (device.batteryState == UIDeviceBatteryStateCharging ||
                        device.batteryState == UIDeviceBatteryStateFull);
        
        NSLog(@"[BatteryMonitor] Battery: %d%% %@", level, charging ? @"charging" : @"discharging");
        
        turbo_mobile_optimizer_update_battery(level, charging);
    }
}

void turbo_battery_monitor_start(void) {
    @autoreleasepool {
        UIDevice* device = [UIDevice currentDevice];
        device.batteryMonitoringEnabled = YES;
        
        // Initial update
        update_battery_status();
        
        // Observe battery level changes
        batteryLevelObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:UIDeviceBatteryLevelDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* note) {
                        update_battery_status();
                    }];
        
        // Observe battery state changes
        batteryStateObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:UIDeviceBatteryStateDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* note) {
                        update_battery_status();
                    }];
        
        NSLog(@"[BatteryMonitor] Started");
    }
}

void turbo_battery_monitor_stop(void) {
    @autoreleasepool {
        if (batteryLevelObserver) {
            [[NSNotificationCenter defaultCenter] removeObserver:batteryLevelObserver];
            batteryLevelObserver = nil;
        }
        
        if (batteryStateObserver) {
            [[NSNotificationCenter defaultCenter] removeObserver:batteryStateObserver];
            batteryStateObserver = nil;
        }
        
        UIDevice* device = [UIDevice currentDevice];
        device.batteryMonitoringEnabled = NO;
        
        NSLog(@"[BatteryMonitor] Stopped");
    }
}
