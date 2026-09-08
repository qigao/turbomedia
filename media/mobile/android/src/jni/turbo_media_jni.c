/**
 * TurboMedia Android JNI Bindings
 *
 * Staged bindings for the mobile-only pieces currently owned by this
 * repository. WebRTC media-engine, simulcast, and recorder bindings were kept
 * out of this file because those APIs are not part of TurboMedia.
 */

#include "turbo_mobile.h"

#include <android/log.h>
#include <jni.h>
#include <salts_capture_android.h>
#include <stdint.h>

#define LOG_TAG "TurboMediaJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static jlong ptr_to_jlong(void *ptr) {
    return (jlong)(uintptr_t)ptr;
}

static void *jlong_to_ptr(jlong handle) {
    return (void *)(uintptr_t)handle;
}

JNIEXPORT jlong JNICALL
Java_com_turbonet_media_ScreenCapture_nativeCreate(JNIEnv *env,
                                                   jobject thiz,
                                                   jint width,
                                                   jint height,
                                                   jint framerate) {
    (void)env;
    (void)thiz;

    salts_android_screen_capture_config_t config = {
        .width = width,
        .height = height,
        .framerate = framerate,
    };
    salts_capture_t *screen = NULL;
    if (salts_android_screen_capture_create(&config, &screen) !=
        SALTS_CAPTURE_OK) {
        return 0;
    }
    LOGI("ScreenCapture create: %dx%d @ %dfps", width, height, framerate);
    return ptr_to_jlong(screen);
}

JNIEXPORT void JNICALL
Java_com_turbonet_media_ScreenCapture_nativeDestroy(JNIEnv *env,
                                                    jobject thiz,
                                                    jlong handle) {
    (void)env;
    (void)thiz;

    salts_capture_t *screen = (salts_capture_t *)jlong_to_ptr(handle);
    if (screen) {
        salts_capture_destroy(screen);
    }
}

JNIEXPORT jobject JNICALL
Java_com_turbonet_media_ScreenCapture_nativeGetSurface(JNIEnv *env,
                                                       jobject thiz,
                                                       jlong handle) {
    (void)thiz;

    salts_capture_t *screen = (salts_capture_t *)jlong_to_ptr(handle);
    if (!screen) return NULL;

    jobject surface = NULL;
    return salts_android_screen_capture_get_surface(env, screen, &surface) ==
                   SALTS_CAPTURE_OK
               ? surface
               : NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_turbonet_media_ScreenCapture_nativeStart(JNIEnv *env,
                                                  jobject thiz,
                                                  jlong handle) {
    (void)env;
    (void)thiz;

    salts_capture_t *screen = (salts_capture_t *)jlong_to_ptr(handle);
    if (!screen) return JNI_FALSE;

    return salts_capture_start(screen) == SALTS_CAPTURE_OK ? JNI_TRUE
                                                           : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_turbonet_media_ScreenCapture_nativeStop(JNIEnv *env,
                                                 jobject thiz,
                                                 jlong handle) {
    (void)env;
    (void)thiz;

    salts_capture_t *screen = (salts_capture_t *)jlong_to_ptr(handle);
    if (!screen) return JNI_FALSE;

    salts_capture_stop(screen);
    return JNI_TRUE;
}

JNIEXPORT jlong JNICALL
Java_com_turbonet_media_ScreenCapture_nativeGetFrameCount(JNIEnv *env,
                                                          jobject thiz,
                                                          jlong handle) {
    (void)env;
    (void)thiz;

    salts_capture_t *screen = (salts_capture_t *)jlong_to_ptr(handle);
    uint64_t frame_count = 0;
    if (salts_android_screen_capture_get_frame_count(screen, &frame_count) !=
        SALTS_CAPTURE_OK) {
        return 0;
    }
    return (jlong)frame_count;
}

JNIEXPORT void JNICALL
Java_com_turbonet_media_MobileOptimizer_nativeEnableBatterySaver(JNIEnv *env,
                                                                 jobject thiz,
                                                                 jlong handle,
                                                                 jboolean enable) {
    (void)env;
    (void)thiz;
    (void)handle;
    LOGI("Battery saver: %s", enable ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_com_turbonet_media_MobileOptimizer_nativeSetNetworkType(JNIEnv *env,
                                                             jobject thiz,
                                                             jlong handle,
                                                             jint network_type) {
    (void)env;
    (void)thiz;
    (void)handle;

    const char *type = "UNKNOWN";
    switch (network_type) {
        case 0:
            type = "WIFI";
            break;
        case 1:
            type = "4G";
            break;
        case 2:
            type = "3G";
            break;
        case 3:
            type = "2G";
            break;
        default:
            type = "UNKNOWN";
            break;
    }

    turbo_mobile_optimizer_update_network(type, 100);
}

JNIEXPORT void JNICALL
Java_com_turbonet_media_MobileOptimizer_nativeEnableHardwareAcceleration(JNIEnv *env,
                                                                         jobject thiz,
                                                                         jlong handle,
                                                                         jboolean enable) {
    (void)env;
    (void)thiz;
    (void)handle;
    LOGI("Hardware acceleration preference: %s", enable ? "enabled" : "disabled");
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm;
    (void)reserved;
    LOGI("TurboMedia JNI loaded");
    return JNI_VERSION_1_6;
}
