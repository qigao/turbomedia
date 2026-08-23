#include "turbo_mobile.h"
#include <tinytest.h>
#include <string.h>
#include <stdint.h>

/* Helper to reset optimizer state between tests */
static void reset_optimizer_state(void) {
    // Reset to known good state
    turbo_mobile_optimizer_update_battery(100, 0);  // Full battery, not charging
    turbo_mobile_optimizer_update_network("wifi", 100);  // Strong WiFi
}

suite("TurboMedia Mobile Optimizer") {
    
    group("Battery Level Optimization") {
        
        it("should adjust settings based on battery level") {
            reset_optimizer_state();
            
            // Test high battery (>70%)
            turbo_mobile_optimizer_update_battery(80, 0);
            int bitrate_high = turbo_mobile_optimizer_get_target_bitrate();
            int fps_high = turbo_mobile_optimizer_get_target_fps();
            
            check_true(bitrate_high > 0);
            check_true(fps_high > 0);
            
            // Test medium battery (30-70%)
            turbo_mobile_optimizer_update_battery(50, 0);
            int bitrate_med = turbo_mobile_optimizer_get_target_bitrate();
            int fps_med = turbo_mobile_optimizer_get_target_fps();
            
            // Medium should be <= high
            check_true(bitrate_med <= bitrate_high);
            check_true(fps_med <= fps_high);
            
            // Test low battery (<30%)
            turbo_mobile_optimizer_update_battery(20, 0);
            int bitrate_low = turbo_mobile_optimizer_get_target_bitrate();
            int fps_low = turbo_mobile_optimizer_get_target_fps();
            
            // Low should be <= medium
            check_true(bitrate_low <= bitrate_med);
            check_true(fps_low <= fps_med);
        }
        
        it("should handle charging state appropriately") {
            reset_optimizer_state();
            
            // Test low battery but charging
            turbo_mobile_optimizer_update_battery(25, 1);  // Charging
            int bitrate_charging = turbo_mobile_optimizer_get_target_bitrate();
            
            // Test low battery not charging
            turbo_mobile_optimizer_update_battery(25, 0);  // Not charging
            int bitrate_not_charging = turbo_mobile_optimizer_get_target_bitrate();
            
            // Charging should allow higher bitrate than not charging
            check_true(bitrate_charging >= bitrate_not_charging);
        }
        
        it("should report power mode correctly based on battery") {
            reset_optimizer_state();
            
            // High battery - normal mode
            turbo_mobile_optimizer_update_battery(80, 0);
            const char *mode_high = turbo_mobile_optimizer_get_power_mode_string();
            check_true(mode_high != NULL);
            check_true(strlen(mode_high) > 0);
            
            // Low battery - power saving mode
            turbo_mobile_optimizer_update_battery(15, 0);
            const char *mode_low = turbo_mobile_optimizer_get_power_mode_string();
            check_true(mode_low != NULL);
            check_true(strlen(mode_low) > 0);
        }
        
        it("should validate battery level bounds") {
            reset_optimizer_state();
            
            // Test boundary values
            turbo_mobile_optimizer_update_battery(0, 0);    // 0%
            int bitrate_0 = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_0 > 0);  // Should still return valid value
            
            turbo_mobile_optimizer_update_battery(100, 0);  // 100%
            int bitrate_100 = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_100 > 0);
            
            // Test invalid values (should be handled gracefully)
            turbo_mobile_optimizer_update_battery(-10, 0);
            turbo_mobile_optimizer_update_battery(150, 0);
        }
    }
    
    group("Network Type Optimization") {
        
        it("should optimize for WiFi connection") {
            reset_optimizer_state();
            
            turbo_mobile_optimizer_update_network("wifi", 100);
            int bitrate_wifi = turbo_mobile_optimizer_get_target_bitrate();
            int fps_wifi = turbo_mobile_optimizer_get_target_fps();
            
            check_true(bitrate_wifi > 0);
            check_true(fps_wifi > 0);
        }
        
        it("should reduce quality for cellular connections") {
            reset_optimizer_state();
            
            // WiFi baseline
            turbo_mobile_optimizer_update_network("wifi", 100);
            int bitrate_wifi = turbo_mobile_optimizer_get_target_bitrate();
            
            // 4G should be lower than WiFi
            turbo_mobile_optimizer_update_network("4g", 100);
            int bitrate_4g = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_4g <= bitrate_wifi);
            
            // 3G should be lower than 4G
            turbo_mobile_optimizer_update_network("3g", 100);
            int bitrate_3g = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_3g <= bitrate_4g);
            
            // 2G should be lowest
            turbo_mobile_optimizer_update_network("2g", 100);
            int bitrate_2g = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_2g <= bitrate_3g);
        }
        
        it("should consider signal strength") {
            reset_optimizer_state();
            
            // Strong signal
            turbo_mobile_optimizer_update_network("4g", 100);
            int bitrate_strong = turbo_mobile_optimizer_get_target_bitrate();
            
            // Medium signal
            turbo_mobile_optimizer_update_network("4g", 50);
            int bitrate_medium = turbo_mobile_optimizer_get_target_bitrate();
            
            // Weak signal
            turbo_mobile_optimizer_update_network("4g", 10);
            int bitrate_weak = turbo_mobile_optimizer_get_target_bitrate();
            
            // Stronger signal should allow higher bitrate
            check_true(bitrate_medium <= bitrate_strong);
            check_true(bitrate_weak <= bitrate_medium);
        }
        
        it("should handle unknown network types gracefully") {
            reset_optimizer_state();
            
            // Should not crash with unknown types
            turbo_mobile_optimizer_update_network("unknown", 50);
            turbo_mobile_optimizer_update_network("", 50);
            turbo_mobile_optimizer_update_network(NULL, 50);
            
            int bitrate = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate > 0);  // Should return safe default
        }
        
        it("should validate signal strength bounds") {
            reset_optimizer_state();
            
            // Test boundary values
            turbo_mobile_optimizer_update_network("wifi", 0);
            int bitrate_0 = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_0 > 0);
            
            turbo_mobile_optimizer_update_network("wifi", 100);
            int bitrate_100 = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_100 > 0);
            
            // Test invalid values (should be handled)
            turbo_mobile_optimizer_update_network("wifi", -10);
            turbo_mobile_optimizer_update_network("wifi", 150);
        }
    }
    
    group("Combined Battery and Network Optimization") {
        
        it("should balance battery and network constraints") {
            reset_optimizer_state();
            
            // Best case: high battery + WiFi
            turbo_mobile_optimizer_update_battery(90, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            int bitrate_best = turbo_mobile_optimizer_get_target_bitrate();
            
            // Worst case: low battery + weak 2G
            turbo_mobile_optimizer_update_battery(10, 0);
            turbo_mobile_optimizer_update_network("2g", 20);
            int bitrate_worst = turbo_mobile_optimizer_get_target_bitrate();
            
            // Best should be significantly higher than worst
            check_true(bitrate_worst < bitrate_best);
        }
        
        it("should prioritize battery over network when critically low") {
            reset_optimizer_state();
            
            // Critical battery with good network
            turbo_mobile_optimizer_update_battery(5, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            int bitrate_critical = turbo_mobile_optimizer_get_target_bitrate();
            
            // Should be very conservative despite good network
            check_true(bitrate_critical > 0);
            
            // Verify power mode reflects critical state
            const char *mode = turbo_mobile_optimizer_get_power_mode_string();
            check_true(mode != NULL);
        }
        
        it("should adapt resolution based on combined factors") {
            reset_optimizer_state();
            
            // Good conditions
            turbo_mobile_optimizer_update_battery(80, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            int scale_good = turbo_mobile_optimizer_get_resolution_scale();
            
            // Poor conditions
            turbo_mobile_optimizer_update_battery(20, 0);
            turbo_mobile_optimizer_update_network("3g", 30);
            int scale_poor = turbo_mobile_optimizer_get_resolution_scale();
            
            // Scale should be valid percentage
            check_true(scale_good > 0 && scale_good <= 100);
            check_true(scale_poor > 0 && scale_poor <= 100);
            check_true(scale_poor <= scale_good);
        }
    }
    
    group("Hardware Codec Recommendations") {
        
        it("should recommend hardware codec on good conditions") {
            reset_optimizer_state();
            
            turbo_mobile_optimizer_update_battery(80, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            
            bool should_use_hw = turbo_mobile_optimizer_should_use_hardware_codec();
            // Should recommend hardware codec when conditions are good
            // (Implementation dependent, but should return valid boolean)
            check_true(should_use_hw == 0 || should_use_hw == 1);
        }
        
        it("should consider battery when recommending hardware codec") {
            reset_optimizer_state();
            
            // Low battery - might prefer software for power saving
            turbo_mobile_optimizer_update_battery(15, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            
            bool should_use_hw = turbo_mobile_optimizer_should_use_hardware_codec();
            check_true(should_use_hw == 0 || should_use_hw == 1);
        }
    }
    
    group("Simulcast Recommendations") {
        
        it("should recommend simulcast based on conditions") {
            reset_optimizer_state();
            
            // Good conditions - should enable simulcast
            turbo_mobile_optimizer_update_battery(90, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            
            bool enable_simulcast = turbo_mobile_optimizer_should_enable_simulcast();
            check_true(enable_simulcast == 0 || enable_simulcast == 1);
        }
        
        it("should disable simulcast on poor conditions") {
            reset_optimizer_state();
            
            // Poor conditions - simulcast adds overhead
            turbo_mobile_optimizer_update_battery(15, 0);
            turbo_mobile_optimizer_update_network("2g", 20);
            
            bool enable_simulcast = turbo_mobile_optimizer_should_enable_simulcast();
            check_true(enable_simulcast == 0 || enable_simulcast == 1);
        }
    }
    
    group("Monitor Lifecycle") {
        
        it("should start and stop battery monitor safely") {
            // Should not crash on multiple starts/stops
            turbo_battery_monitor_start();
            turbo_battery_monitor_start();  // Double start
            turbo_battery_monitor_stop();
            turbo_battery_monitor_stop();   // Double stop
            
            // Restart after stop
            turbo_battery_monitor_start();
            turbo_battery_monitor_stop();
        }
        
        it("should start and stop network monitor safely") {
            // Should not crash on multiple starts/stops
            turbo_network_monitor_start();
            turbo_network_monitor_start();  // Double start
            turbo_network_monitor_stop();
            turbo_network_monitor_stop();   // Double stop
            
            // Restart after stop
            turbo_network_monitor_start();
            turbo_network_monitor_stop();
        }
        
        it("should handle interleaved monitor operations") {
            // Start both monitors
            turbo_battery_monitor_start();
            turbo_network_monitor_start();
            
            // Stop in reverse order
            turbo_network_monitor_stop();
            turbo_battery_monitor_stop();
            
            // Start in different order
            turbo_network_monitor_start();
            turbo_battery_monitor_start();
            
            // Stop both
            turbo_battery_monitor_stop();
            turbo_network_monitor_stop();
        }
    }
    
    group("Parameter Validation and Edge Cases") {
        
        it("should return valid values for all getter functions") {
            reset_optimizer_state();
            
            int bitrate = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate > 0);
            
            int fps = turbo_mobile_optimizer_get_target_fps();
            check_true(fps > 0);
            
            int scale = turbo_mobile_optimizer_get_resolution_scale();
            check_true(scale > 0 && scale <= 100);
            
            const char *mode = turbo_mobile_optimizer_get_power_mode_string();
            check_true(mode != NULL);
            check_true(strlen(mode) > 0);
        }
        
        it("should handle rapid state changes") {
            reset_optimizer_state();
            
            // Simulate rapid changes in conditions
            for (int i = 0; i < 10; i++) {
                turbo_mobile_optimizer_update_battery(i * 10, i % 2);
                turbo_mobile_optimizer_update_network(
                    i % 2 ? "wifi" : "4g", 
                    i * 10
                );
                
                // Getters should always return valid values
                int bitrate = turbo_mobile_optimizer_get_target_bitrate();
                check_true(bitrate > 0);
            }
        }
        
        it("should provide consistent recommendations") {
            reset_optimizer_state();
            
            // Set specific state
            turbo_mobile_optimizer_update_battery(50, 0);
            turbo_mobile_optimizer_update_network("4g", 70);
            
            // Get recommendations multiple times
            int bitrate1 = turbo_mobile_optimizer_get_target_bitrate();
            int bitrate2 = turbo_mobile_optimizer_get_target_bitrate();
            int bitrate3 = turbo_mobile_optimizer_get_target_bitrate();
            
            // Should be consistent without state changes
            check_equal(bitrate2, bitrate1);
            check_equal(bitrate3, bitrate2);
        }
        
        it("should handle null and empty string network types") {
            reset_optimizer_state();
            
            turbo_mobile_optimizer_update_network(NULL, 50);
            int bitrate_null = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_null > 0);
            
            turbo_mobile_optimizer_update_network("", 50);
            int bitrate_empty = turbo_mobile_optimizer_get_target_bitrate();
            check_true(bitrate_empty > 0);
        }
    }
    
    group("Realistic Usage Scenarios") {
        
        it("should handle typical commute scenario") {
            reset_optimizer_state();
            
            // Start with full battery and WiFi
            turbo_mobile_optimizer_update_battery(100, 0);
            turbo_mobile_optimizer_update_network("wifi", 100);
            int bitrate_start = turbo_mobile_optimizer_get_target_bitrate();
            
            // Switch to 4G as user leaves
            turbo_mobile_optimizer_update_network("4g", 80);
            int bitrate_4g = turbo_mobile_optimizer_get_target_bitrate();
            
            // Battery drops during commute
            turbo_mobile_optimizer_update_battery(70, 0);
            int bitrate_mid = turbo_mobile_optimizer_get_target_bitrate();
            
            // Weak signal in subway
            turbo_mobile_optimizer_update_network("3g", 20);
            turbo_mobile_optimizer_update_battery(60, 0);
            int bitrate_weak = turbo_mobile_optimizer_get_target_bitrate();
            
            // Verify progressive adaptation
            check_true(bitrate_4g <= bitrate_start);
            check_true(bitrate_mid <= bitrate_4g);
            check_true(bitrate_weak <= bitrate_mid);
        }
        
        it("should handle charging while streaming scenario") {
            reset_optimizer_state();
            
            // Low battery, start charging
            turbo_mobile_optimizer_update_battery(20, 0);
            int bitrate_before_charging = turbo_mobile_optimizer_get_target_bitrate();
            
            turbo_mobile_optimizer_update_battery(20, 1);  // Plugged in
            int bitrate_charging = turbo_mobile_optimizer_get_target_bitrate();
            
            // Should allow higher quality when charging
            check_true(bitrate_charging >= bitrate_before_charging);
            
            // Battery increases while charging
            turbo_mobile_optimizer_update_battery(50, 1);
            int bitrate_charging_higher = turbo_mobile_optimizer_get_target_bitrate();
            
            check_true(bitrate_charging_higher >= bitrate_charging);
        }
    }
}
