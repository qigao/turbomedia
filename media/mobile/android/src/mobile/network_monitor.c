/**
 * Network Monitor for Android
 * Monitors network type and signal strength
 */

#include "turbo_mobile.h"
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "NetworkMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static JavaVM* g_jvm = NULL;

JNIEXPORT void JNICALL
Java_com_turbonet_media_MobileOptimizer_nativeUpdateNetwork(
    JNIEnv* env, jobject obj, jstring type, jint signal_strength) {
    
    const char* type_str = (*env)->GetStringUTFChars(env, type, NULL);
    if (type_str) {
        LOGI("Network update: %s, signal=%d%%", type_str, signal_strength);
        turbo_mobile_optimizer_update_network(type_str, signal_strength);
        (*env)->ReleaseStringUTFChars(env, type, type_str);
    }
}

void turbo_network_monitor_init(JavaVM* jvm) {
    g_jvm = jvm;
    LOGI("Network monitor initialized");
}

void turbo_network_monitor_start(void) {
    LOGI("Network monitor uses Java callbacks on Android");
}

void turbo_network_monitor_stop(void) {
    LOGI("Network monitor stopped");
}

void turbo_network_monitor_cleanup(void) {
    // Cleanup if needed
}
