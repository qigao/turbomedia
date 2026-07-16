/**
 * TurboMedia Android JNI Bindings
 *
 * Staged bindings for the mobile-only pieces currently owned by this
 * repository. WebRTC media-engine, simulcast, and recorder bindings were kept
 * out of this file because those APIs are not part of TurboMedia.
 */

#include "turbo_mobile.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdint.h>

#define LOG_TAG "TurboMediaJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

typedef struct android_screen_ctx_t android_screen_ctx_t;

extern android_screen_ctx_t *android_screen_create(int width, int height, int framerate);
extern void android_screen_destroy(android_screen_ctx_t *ctx);
extern int android_screen_start(android_screen_ctx_t *ctx, jobject media_projection);
extern int android_screen_stop(android_screen_ctx_t *ctx);
extern ANativeWindow *android_screen_get_surface(android_screen_ctx_t *ctx);
extern uint64_t android_screen_get_frame_count(android_screen_ctx_t *ctx);

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

    android_screen_ctx_t *screen = android_screen_create(width, height, framerate);
    LOGI("ScreenCapture create: %dx%d @ %dfps", width, height, framerate);
    return ptr_to_jlong(screen);
}

JNIEXPORT void JNICALL
Java_com_turbonet_media_ScreenCapture_nativeDestroy(JNIEnv *env,
                                                    jobject thiz,
                                                    jlong handle) {
    (void)env;
    (void)thiz;

    android_screen_ctx_t *screen = (android_screen_ctx_t *)jlong_to_ptr(handle);
    if (screen) {
        android_screen_destroy(screen);
    }
}

JNIEXPORT jobject JNICALL
Java_com_turbonet_media_ScreenCapture_nativeGetSurface(JNIEnv *env,
                                                       jobject thiz,
                                                       jlong handle) {
    (void)thiz;

    android_screen_ctx_t *screen = (android_screen_ctx_t *)jlong_to_ptr(handle);
    if (!screen) return NULL;

    ANativeWindow *window = android_screen_get_surface(screen);
    if (!window) return NULL;

    return ANativeWindow_toSurface(env, window);
}

JNIEXPORT jboolean JNICALL
Java_com_turbonet_media_ScreenCapture_nativeStart(JNIEnv *env,
                                                  jobject thiz,
                                                  jlong handle) {
    (void)env;
    (void)thiz;

    android_screen_ctx_t *screen = (android_screen_ctx_t *)jlong_to_ptr(handle);
    if (!screen) return JNI_FALSE;

    return android_screen_start(screen, NULL) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_turbonet_media_ScreenCapture_nativeStop(JNIEnv *env,
                                                 jobject thiz,
                                                 jlong handle) {
    (void)env;
    (void)thiz;

    android_screen_ctx_t *screen = (android_screen_ctx_t *)jlong_to_ptr(handle);
    if (!screen) return JNI_FALSE;

    return android_screen_stop(screen) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_turbonet_media_ScreenCapture_nativeGetFrameCount(JNIEnv *env,
                                                          jobject thiz,
                                                          jlong handle) {
    (void)env;
    (void)thiz;

    android_screen_ctx_t *screen = (android_screen_ctx_t *)jlong_to_ptr(handle);
    return (jlong)android_screen_get_frame_count(screen);
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
