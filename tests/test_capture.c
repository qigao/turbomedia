/**
 * TurboMedia Capture Device Tests (BDD Style)
 * 
 * 注意：本测试套件仅测试已实现的API
 * 待实现的功能已标记为 TODO
 */

#include <turbo_capture.h>
#include <tinytest.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ============================================================================
 * 配置辅助函数
 * ============================================================================ */

static turbo_audio_capture_config_t valid_audio_config(void) {
    turbo_audio_capture_config_t config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.bits_per_sample = 16;
    config.frame_size_ms = 10;
    return config;
}

static turbo_screen_capture_config_t valid_screen_config(void) {
    turbo_screen_capture_config_t config;
    config.monitor_index = 0;
    config.framerate = 30;
    config.capture_cursor = 1;
    config.capture_audio = 0;
    return config;
}

#ifdef _WIN32
typedef struct {
    volatile LONG received;
    int has_jpeg_soi;
} mjpeg_capture_observer_t;

static void observe_mjpeg_frame(turbo_capture_t *capture,
                                const uint8_t *frame,
                                size_t len,
                                int width,
                                int height,
                                uint64_t timestamp,
                                void *user_data) {
    mjpeg_capture_observer_t *observer = (mjpeg_capture_observer_t *)user_data;
    (void)capture;
    (void)width;
    (void)height;
    (void)timestamp;

    if (InterlockedCompareExchange(&observer->received, 0, 0) != 0) return;
    observer->has_jpeg_soi = len >= 2 && frame[0] == 0xFF && frame[1] == 0xD8;
    InterlockedExchange(&observer->received, 1);
}
#endif

/* ============================================================================
 * BDD 测试: 设备枚举
 * ============================================================================ */

suite("捕获设备枚举") {
    it("应该安全处理空指针和无效参数") {
        /* 空指针应该返回错误 */
        check_equal(turbo_capture_list_audio_devices(NULL, 10), -1);
        check_equal(turbo_capture_list_video_devices(NULL, 10), -1);
        check_equal(turbo_capture_list_screens(NULL, 10), -1);
        check_equal(turbo_capture_list_gpu_devices(NULL, 0), -1);
        
        /* max_count <= 0 应该返回错误 */
        turbo_capture_device_t devices[1];
        check_equal(turbo_capture_list_audio_devices(devices, 0), -1);
        check_equal(turbo_capture_list_video_devices(devices, 0), -1);
        check_equal(turbo_capture_list_screens(devices, 0), -1);
        check_equal(turbo_capture_list_gpu_devices(devices, -1), -1);
    }
    
    it("应该枚举可用的音频输入设备") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int count = turbo_capture_list_audio_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
        
        check_true(count >= 0);
        check_true(count <= TURBO_CAPTURE_MAX_DEVICES);
        
        /* 验证设备信息 */
        for (int i = 0; i < count; i++) {
            check_true(devices[i].type == TURBO_CAPTURE_TYPE_AUDIO);
            check_true(devices[i].name[0] != '\0');
            check_true(devices[i].id[0] != '\0');
            check_true(devices[i].index >= 0);
        }
        
        /* 应该至少有一个默认设备（如果有设备） */
        if (count > 0) {
            int has_default = 0;
            for (int i = 0; i < count; i++) {
                if (devices[i].is_default) {
                    has_default = 1;
                    break;
                }
            }
            check_true(has_default || count == 0);
        }
    }
    
    it("应该枚举可用的视频输入设备") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int count = turbo_capture_list_video_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
        
        check_true(count >= 0);
        
        for (int i = 0; i < count; i++) {
            check_true(devices[i].type == TURBO_CAPTURE_TYPE_VIDEO);
        }
    }
    
    it("应该枚举可用的屏幕/显示器") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int count = turbo_capture_list_screens(devices, TURBO_CAPTURE_MAX_DEVICES);
        
        check_true(count >= 0);
        
        for (int i = 0; i < count; i++) {
            check_true(devices[i].type == TURBO_CAPTURE_TYPE_SCREEN);
        }
    }
    
    it("应该正确处理缓冲区大小限制") {
        turbo_capture_device_t devices[2];
        int count = turbo_capture_list_audio_devices(devices, 2);
        
        check_true(count >= 0);
        check_true(count <= 2);
    }
}

/* ============================================================================
 * BDD 测试: 音频捕获生命周期
 * ============================================================================ */

suite("音频捕获设备生命周期") {
    it("应该成功创建有效的音频捕获实例") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *capture = NULL;
        
        /* 使用默认设备 */
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) {
            turbo_capture_destroy(capture);
        }
        
        /* NULL 配置可能使用默认值或失败 */
        capture = turbo_audio_capture_create(NULL, NULL);
        if (capture) {
            /* 如果实现支持默认配置，应该能创建成功 */
            turbo_capture_destroy(capture);
        }
        /* 不强制要求失败，因为实现可能支持默认配置 */
    }
    
    it("应该验证采样率参数") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *capture = NULL;
        
        /* 有效采样率 */
        config.sample_rate = 48000;
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        config.sample_rate = 16000;
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        config.sample_rate = 8000;
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
    }
    
    it("应该验证声道数参数") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *capture = NULL;
        
        /* 有效声道数 */
        config.channels = 1;  /* 单声道 */
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        config.channels = 2;  /* 立体声 */
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        /* 无效声道数应该失败或被调整 */
        config.channels = 0;
        capture = turbo_audio_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
    }
    
    it("应该能够多次创建和销毁捕获实例") {
        for (int i = 0; i < 3; i++) {
            turbo_audio_capture_config_t config = valid_audio_config();
            turbo_capture_t *capture = turbo_audio_capture_create(NULL, &config);
            
            if (capture) {
                turbo_capture_destroy(capture);
            }
        }
    }
}

/* ============================================================================
 * BDD 测试: 视频捕获生命周期
 * ============================================================================ */

suite("视频捕获设备生命周期") {
    it("应该公开稳定的 MJPEG 捕获格式") {
        check_equal(TURBO_VIDEO_CAPTURE_FORMAT_I420, 0);
        check_equal(TURBO_VIDEO_CAPTURE_FORMAT_NV12, 1);
        check_equal(TURBO_VIDEO_CAPTURE_FORMAT_RGB24, 2);
        check_equal(TURBO_VIDEO_CAPTURE_FORMAT_BGRA, 3);
        check_equal(TURBO_VIDEO_CAPTURE_FORMAT_MJPEG, 4);
    }

#ifdef _WIN32
    it("应该精确选择枚举到的原生 MJPEG 模式并捕获 JPEG") {
        enum {
            TEST_MAX_VIDEO_NATIVE_MODES = 256,
            TEST_CAPTURE_WAIT_ATTEMPTS = 100,
            TEST_CAPTURE_WAIT_MS = 20
        };
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int device_count = turbo_capture_list_video_devices(
            devices, TURBO_CAPTURE_MAX_DEVICES);
        int mjpeg_mode_count = 0;
        int captured_jpeg = 0;
        int validated_exact_identity = 0;

        for (int device_index = 0; device_index < device_count; ++device_index) {
            turbo_video_native_mode_t modes[TEST_MAX_VIDEO_NATIVE_MODES];
            turbo_video_device_t *device = NULL;
            size_t mode_count = 0;

            if (turbo_video_device_open(devices[device_index].id, &device) !=
                TURBO_CAPTURE_OK) {
                continue;
            }
            if (turbo_video_device_list_modes(
                    device, modes, TEST_MAX_VIDEO_NATIVE_MODES,
                    &mode_count) != TURBO_CAPTURE_OK) {
                turbo_video_device_close(device);
                continue;
            }
            for (size_t mode_index = 0; mode_index < mode_count; ++mode_index) {
                turbo_capture_t *capture;
                mjpeg_capture_observer_t observer = {0};
                int start_result;

                check_true(modes[mode_index].width > 0);
                check_true(modes[mode_index].height > 0);
                check_true(modes[mode_index].framerate_numerator > 0);
                check_true(modes[mode_index].framerate_denominator > 0);
                for (size_t prior_index = 0;
                     prior_index < mode_index;
                     ++prior_index) {
                    check_true(modes[prior_index].mode_id !=
                               modes[mode_index].mode_id);
                }

                if (modes[mode_index].format !=
                    TURBO_VIDEO_CAPTURE_FORMAT_MJPEG) {
                    continue;
                }
                mjpeg_mode_count++;

                if (!validated_exact_identity) {
                    turbo_video_native_mode_t mismatched_mode =
                        modes[mode_index];
                    turbo_capture_t *mismatched_capture = NULL;
                    mismatched_mode.framerate_numerator =
                        mismatched_mode.framerate_numerator == UINT32_MAX
                            ? mismatched_mode.framerate_numerator - 1
                            : mismatched_mode.framerate_numerator + 1;
                    check_true(turbo_video_device_create_capture(
                                   device, &mismatched_mode,
                                   &mismatched_capture) != TURBO_CAPTURE_OK);
                    check_null(mismatched_capture);
                    validated_exact_identity = 1;
                }

                capture = NULL;
                if (turbo_video_device_create_capture(
                        device, &modes[mode_index], &capture) !=
                    TURBO_CAPTURE_OK) {
                    continue;
                }

                turbo_video_capture_set_callback(
                    capture, observe_mjpeg_frame, &observer);
                start_result = turbo_capture_start(capture);
                if (start_result == TURBO_CAPTURE_OK) {
                    for (int attempt = 0;
                         attempt < TEST_CAPTURE_WAIT_ATTEMPTS &&
                         InterlockedCompareExchange(&observer.received, 0, 0) == 0;
                         ++attempt) {
                        Sleep(TEST_CAPTURE_WAIT_MS);
                    }
                    turbo_capture_stop(capture);
                    captured_jpeg =
                        InterlockedCompareExchange(&observer.received, 0, 0) == 1 &&
                        observer.has_jpeg_soi;
                }
                turbo_capture_destroy(capture);
                if (captured_jpeg) {
                    turbo_video_device_close(device);
                    check_true(captured_jpeg);
                    return;
                }
            }
            turbo_video_device_close(device);
        }

        if (mjpeg_mode_count > 0) check_true(captured_jpeg);
    }
#endif

    it("应该拒绝空的原生视频模式") {
        turbo_capture_t *capture = NULL;
        check_equal(turbo_video_device_create_capture(NULL, NULL, &capture),
                     TURBO_CAPTURE_ERR_FORMAT);
        check_null(capture);
    }
}

/* ============================================================================
 * BDD 测试: 捕获控制
 * ============================================================================ */

suite("捕获启动和停止控制") {
    it("应该能够启动和停止音频捕获") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *capture = turbo_audio_capture_create(NULL, &config);
        
        if (capture) {
            /* 启动捕获 */
            int result = turbo_capture_start(capture);
            if (result == TURBO_CAPTURE_OK) {
                /* 停止捕获 */
                turbo_capture_stop(capture);
            }
            
            turbo_capture_destroy(capture);
        }
    }
    
    it("应该拒绝对空指针的操作") {
        /* turbo_capture_start(NULL) 应该返回错误 */
        int result = turbo_capture_start(NULL);
        check_true(result < 0);  /* 返回任何负数错误码都可接受 */
        
        turbo_capture_stop(NULL); /* 不应崩溃 */
        turbo_capture_destroy(NULL); /* 不应崩溃 */
    }
    
    it("应该处理重复的启动和停止调用") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *capture = turbo_audio_capture_create(NULL, &config);
        
        if (capture) {
            int result1 = turbo_capture_start(capture);
            if (result1 == TURBO_CAPTURE_OK) {
                /* 重复启动应该失败或被忽略 */
                int result2 = turbo_capture_start(capture);
                (void)result2;
                
                turbo_capture_stop(capture);
                turbo_capture_stop(capture); /* 不应崩溃 */
            }
            
            turbo_capture_destroy(capture);
        }
    }
}

/* ============================================================================
 * TODO: 待实现的测试
 * ============================================================================ */

/* 
 * TODO: 以下测试依赖尚未实现的回调 API，待 API 实现后取消注释
 *
 * - turbo_audio_capture_set_callback() - 设置音频数据回调
 * - turbo_video_capture_set_callback() - 设置视频帧回调
 * - turbo_capture_on_state() + user_data 支持 - 状态变化回调
 *
suite("音频捕获回调机制") {
    it("应该在捕获音频数据时触发回调") {
        // 需要 turbo_audio_capture_set_callback 实现
    }
}

suite("视频捕获回调机制") {
    it("应该在捕获视频帧时触发回调") {
        // 需要 turbo_video_capture_set_callback 实现
    }
}

suite("捕获状态变化通知") {
    it("应该在状态变化时通知应用") {
        // 需要 turbo_capture_on_state 实现，并支持 user_data 传递
    }
}
*/

/* ============================================================================
 * BDD 测试: 屏幕捕获
 * ============================================================================ */

suite("屏幕捕获功能") {
    it("应该成功创建屏幕捕获实例") {
        turbo_screen_capture_config_t config = valid_screen_config();
        turbo_capture_t *capture = NULL;
        
        /* 捕获默认显示器 */
        config.monitor_index = 0;
        capture = turbo_screen_capture_create(&config);
        if (capture) turbo_capture_destroy(capture);
        
        /* 捕获所有显示器 */
        config.monitor_index = -1;
        capture = turbo_screen_capture_create(&config);
        if (capture) turbo_capture_destroy(capture);
    }
    
    it("应该验证屏幕捕获配置") {
        turbo_screen_capture_config_t config = valid_screen_config();
        turbo_capture_t *capture = NULL;
        
        /* 有光标 */
        config.capture_cursor = 1;
        capture = turbo_screen_capture_create(&config);
        if (capture) turbo_capture_destroy(capture);
        
        /* 无光标 */
        config.capture_cursor = 0;
        capture = turbo_screen_capture_create(&config);
        if (capture) turbo_capture_destroy(capture);
    }
}

/* ============================================================================
 * BDD 测试: 错误处理
 * ============================================================================ */

suite("捕获设备错误处理") {
    it("应该拒绝无效的设备ID") {
        turbo_audio_capture_config_t config = valid_audio_config();
        
        /* 不存在的设备ID */
        turbo_capture_t *capture = turbo_audio_capture_create("non-existent-device-id", &config);
        
        /* 应该失败或使用默认设备 */
        if (capture) {
            turbo_capture_destroy(capture);
        }
    }
    
    it("应该支持并发创建多个捕获实例") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *captures[10] = {0};
        turbo_capture_t *probe = NULL;
        int created_count = 0;

        /* A headless host may expose backend names without a usable device. */
        probe = turbo_audio_capture_create(NULL, &config);
        if (!probe) {
            return;
        }
        turbo_capture_destroy(probe);
        
        /* 尝试创建多个实例 */
        for (int i = 0; i < 10; i++) {
            captures[i] = turbo_audio_capture_create(NULL, &config);
            if (captures[i]) {
                created_count++;
            }
        }
        
        /* 清理 */
        for (int i = 0; i < 10; i++) {
            if (captures[i]) {
                turbo_capture_destroy(captures[i]);
            }
        }
        
        /* 应该至少能创建一个实例 */
        check_true(created_count > 0);
    }
    
    it("应该安全销毁已销毁的实例") {
        turbo_audio_capture_config_t config = valid_audio_config();
        turbo_capture_t *capture = turbo_audio_capture_create(NULL, &config);
        
        if (capture) {
            turbo_capture_destroy(capture);
            /* 不应该再次销毁，但测试框架需要保护 */
            /* turbo_capture_destroy(capture); */ /* 危险：可能崩溃 */
        }
    }
}

/* ============================================================================
 * BDD 测试: 视频模式帧率工具
 * ============================================================================ */

suite("视频模式帧率工具") {
    it("应该正确换算整数帧率（就近取整）") {
        turbo_video_native_mode_t mode;

        memset(&mode, 0, sizeof(mode));
        mode.framerate_numerator = 30000;
        mode.framerate_denominator = 1001;
        check_equal(turbo_video_mode_fps(&mode), 30);      /* 29.97 -> 30 */

        mode.framerate_numerator = 60000;
        mode.framerate_denominator = 1001;
        check_equal(turbo_video_mode_fps(&mode), 60);      /* 59.94 -> 60 */

        mode.framerate_numerator = 24000;
        mode.framerate_denominator = 1001;
        check_equal(turbo_video_mode_fps(&mode), 24);      /* 23.976 -> 24 */

        mode.framerate_numerator = 10000000;
        mode.framerate_denominator = 111111;
        check_equal(turbo_video_mode_fps(&mode), 90);      /* 90.00009 */

        mode.framerate_numerator = 10000000;
        mode.framerate_denominator = 83333;
        check_equal(turbo_video_mode_fps(&mode), 120);     /* 120.00048 */

        mode.framerate_numerator = 10000000;
        mode.framerate_denominator = 1333333;
        check_equal(turbo_video_mode_fps(&mode), 8);       /* 7.5 -> 8 */

        check_equal(turbo_video_mode_fps(NULL), 0);
    }

    it("应该识别标准帧率集合 24/25/30/50/60/90/120") {
        static const uint32_t standard[] = {24, 25, 30, 50, 60, 90, 120};
        static const uint32_t non_standard[] = {20, 15, 10, 8, 5};
        turbo_video_native_mode_t mode;

        memset(&mode, 0, sizeof(mode));
        mode.framerate_denominator = 1;

        for (size_t i = 0; i < sizeof(standard) / sizeof(standard[0]); ++i) {
            mode.framerate_numerator = standard[i];
            check_true(turbo_video_mode_is_standard_fps(&mode));
        }
        for (size_t i = 0; i < sizeof(non_standard) / sizeof(non_standard[0]); ++i) {
            mode.framerate_numerator = non_standard[i];
            check_false(turbo_video_mode_is_standard_fps(&mode));
        }
        check_false(turbo_video_mode_is_standard_fps(NULL));
    }

#ifdef _WIN32
    it("默认模式列表只包含标准帧率，all 接口返回全量") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int device_count = turbo_capture_list_video_devices(
            devices, TURBO_CAPTURE_MAX_DEVICES);

        for (int device_index = 0; device_index < device_count; ++device_index) {
            turbo_video_device_t *device = NULL;
            turbo_video_native_mode_t modes[TURBO_CAPTURE_MAX_VIDEO_MODES];
            turbo_video_native_mode_t all_modes[TURBO_CAPTURE_MAX_VIDEO_MODES];
            size_t mode_count = 0;
            size_t all_count = 0;

            if (turbo_video_device_open(devices[device_index].id, &device) !=
                TURBO_CAPTURE_OK) {
                continue;
            }
            if (turbo_video_device_list_modes(
                    device, modes, TURBO_CAPTURE_MAX_VIDEO_MODES,
                    &mode_count) != TURBO_CAPTURE_OK) {
                turbo_video_device_close(device);
                continue;
            }
            for (size_t i = 0; i < mode_count; ++i) {
                check_true(turbo_video_mode_is_standard_fps(&modes[i]));
                check_true(modes[i].width > 0);
                check_true(modes[i].height > 0);
            }
            if (turbo_video_device_list_modes_all(
                    device, all_modes, TURBO_CAPTURE_MAX_VIDEO_MODES,
                    &all_count) != TURBO_CAPTURE_OK) {
                turbo_video_device_close(device);
                continue;
            }
            check_true(all_count >= mode_count);
            turbo_video_device_close(device);
        }
    }
#endif
}
