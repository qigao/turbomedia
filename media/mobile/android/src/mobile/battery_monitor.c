/**
 * Battery Monitor for Android
 * Monitors battery level and charging state
 */

#include "turbo_mobile.h"
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>

#define LOG_TAG "BatteryMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static JavaVM* g_jvm = NULL;
static jobject g_battery_receiver = NULL;

JNIEXPORT void JNICALL
Java_com_turbonet_media_MobileOptimizer_nativeUpdateBattery(
    JNIEnv* env, jobject obj, jint level, jboolean charging) {
    
    LOGI("Battery update: %d%% %s", level, charging ? "charging" : "discharging");
    turbo_mobile_optimizer_update_battery(level, charging);
}

void turbo_battery_monitor_init(JavaVM* jvm) {
    g_jvm = jvm;
    LOGI("Battery monitor initialized");
}

void turbo_battery_monitor_start(void) {
    LOGI("Battery monitor uses Java callbacks on Android");
}

void turbo_battery_monitor_stop(void) {
    LOGI("Battery monitor stopped");
}

void turbo_battery_monitor_cleanup(void) {
    if (g_jvm && g_battery_receiver) {
        JNIEnv* env;
        (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
        if (env) {
            (*env)->DeleteGlobalRef(env, g_battery_receiver);
        }
        g_battery_receiver = NULL;
    }
}
