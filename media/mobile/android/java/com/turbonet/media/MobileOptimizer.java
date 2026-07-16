package com.turbonet.media;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.BatteryManager;
import android.content.Intent;
import android.content.IntentFilter;

/**
 * Mobile-specific optimizations
 * 
 * Automatically adjusts quality based on battery and network conditions
 */
public class MobileOptimizer {
    static {
        System.loadLibrary("turbo_media_android");
    }
    
    public enum NetworkType {
        WIFI(0),
        MOBILE_4G(1),
        MOBILE_3G(2),
        MOBILE_2G(3),
        UNKNOWN(4);
        
        private final int value;
        NetworkType(int value) { this.value = value; }
        public int getValue() { return value; }
    }
    
    private Context context;
    private long mediaEngineHandle;
    private boolean batterySaverEnabled = false;
    
    public MobileOptimizer(Context context, long mediaEngineHandle) {
        this.context = context;
        this.mediaEngineHandle = mediaEngineHandle;
    }
    
    /**
     * Enable battery saver mode
     * 
     * Reduces quality when battery is low
     */
    public void enableBatterySaver(boolean enable) {
        batterySaverEnabled = enable;
        nativeEnableBatterySaver(mediaEngineHandle, enable);
        
        if (enable) {
            // Monitor battery level
            monitorBattery();
        }
    }
    
    /**
     * Set network type
     * 
     * Adjusts quality based on network conditions
     */
    public void setNetworkType(NetworkType type) {
        nativeSetNetworkType(mediaEngineHandle, type.getValue());
    }
    
    /**
     * Enable hardware acceleration
     * 
     * Uses hardware codecs when available
     */
    public void enableHardwareAcceleration(boolean enable) {
        nativeEnableHardwareAcceleration(mediaEngineHandle, enable);
    }
    
    /**
     * Auto-detect and optimize for current conditions
     */
    public void autoOptimize() {
        // Detect network type
        NetworkType networkType = detectNetworkType();
        setNetworkType(networkType);
        
        // Check battery level
        int batteryLevel = getBatteryLevel();
        if (batteryLevel < 20) {
            enableBatterySaver(true);
        }
        
        // Enable hardware acceleration by default
        enableHardwareAcceleration(true);
    }
    
    /**
     * Get recommended settings for current conditions
     */
    public RecommendedSettings getRecommendedSettings() {
        NetworkType networkType = detectNetworkType();
        int batteryLevel = getBatteryLevel();
        
        RecommendedSettings settings = new RecommendedSettings();
        
        // Adjust based on network
        switch (networkType) {
            case WIFI:
                settings.maxBitrate = 2500000;  // 2.5 Mbps
                settings.maxResolution = "1280x720";
                settings.maxFramerate = 30;
                break;
            case MOBILE_4G:
                settings.maxBitrate = 1500000;  // 1.5 Mbps
                settings.maxResolution = "1280x720";
                settings.maxFramerate = 30;
                break;
            case MOBILE_3G:
                settings.maxBitrate = 500000;   // 500 kbps
                settings.maxResolution = "640x360";
                settings.maxFramerate = 15;
                break;
            case MOBILE_2G:
                settings.maxBitrate = 150000;   // 150 kbps
                settings.maxResolution = "320x180";
                settings.maxFramerate = 10;
                break;
            default:
                settings.maxBitrate = 1000000;  // 1 Mbps
                settings.maxResolution = "640x360";
                settings.maxFramerate = 24;
                break;
        }
        
        // Reduce quality if battery is low
        if (batteryLevel < 20) {
            settings.maxBitrate = (int)(settings.maxBitrate * 0.7);
            settings.maxFramerate = (int)(settings.maxFramerate * 0.7);
        }
        
        return settings;
    }
    
    private NetworkType detectNetworkType() {
        ConnectivityManager cm = (ConnectivityManager) 
            context.getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkInfo info = cm.getActiveNetworkInfo();
        
        if (info == null || !info.isConnected()) {
            return NetworkType.UNKNOWN;
        }
        
        if (info.getType() == ConnectivityManager.TYPE_WIFI) {
            return NetworkType.WIFI;
        }
        
        if (info.getType() == ConnectivityManager.TYPE_MOBILE) {
            int subtype = info.getSubtype();
            switch (subtype) {
                case android.telephony.TelephonyManager.NETWORK_TYPE_LTE:
                case android.telephony.TelephonyManager.NETWORK_TYPE_HSPAP:
                case android.telephony.TelephonyManager.NETWORK_TYPE_EHRPD:
                    return NetworkType.MOBILE_4G;
                case android.telephony.TelephonyManager.NETWORK_TYPE_UMTS:
                case android.telephony.TelephonyManager.NETWORK_TYPE_EVDO_0:
                case android.telephony.TelephonyManager.NETWORK_TYPE_EVDO_A:
                case android.telephony.TelephonyManager.NETWORK_TYPE_HSDPA:
                case android.telephony.TelephonyManager.NETWORK_TYPE_HSUPA:
                case android.telephony.TelephonyManager.NETWORK_TYPE_HSPA:
                    return NetworkType.MOBILE_3G;
                case android.telephony.TelephonyManager.NETWORK_TYPE_GPRS:
                case android.telephony.TelephonyManager.NETWORK_TYPE_EDGE:
                case android.telephony.TelephonyManager.NETWORK_TYPE_CDMA:
                    return NetworkType.MOBILE_2G;
            }
        }
        
        return NetworkType.UNKNOWN;
    }
    
    private int getBatteryLevel() {
        IntentFilter ifilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent batteryStatus = context.registerReceiver(null, ifilter);
        
        int level = batteryStatus.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = batteryStatus.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
        
        return (int)((level / (float)scale) * 100);
    }
    
    private void monitorBattery() {
        // Implementation would register battery broadcast receiver
        // and adjust quality dynamically
    }
    
    public static class RecommendedSettings {
        public int maxBitrate;
        public String maxResolution;
        public int maxFramerate;
    }
    
    // Native methods
    private native void nativeEnableBatterySaver(long handle, boolean enable);
    private native void nativeSetNetworkType(long handle, int networkType);
    private native void nativeEnableHardwareAcceleration(long handle, boolean enable);
}
