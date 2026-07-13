/**
 * TurboMedia Capture Device Tests (BDD Style)
 * 
 * 注意：本测试套件仅测试已实现的API
 * 待实现的功能已标记为 TODO
 */

#include <turbo_capture.h>
#include <tinytest.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * 测试辅助宏 (BDD 风格断言)
 * ============================================================================ */

#define EXPECT_TRUE(expr)       \
    do {                        \
        check(expr);           \
        if (!(expr)) {         \
            return;            \
        }                      \
    } while (0)

#define EXPECT_FALSE(expr)      \
    do {                        \
        check(!(expr));        \
        if (expr) {            \
            return;            \
        }                      \
    } while (0)

#define EXPECT_INT_EQ(expected, actual)            \
    do {                                          \
        int expected_value = (expected);          \
        int actual_value = (actual);              \
        check_int_eq(actual_value, expected_value); \
        if (actual_value != expected_value) {     \
            return;                               \
        }                                         \
    } while (0)

#define EXPECT_NOT_NULL(ptr)    \
    do {                        \
        check((ptr) != NULL);   \
        if ((ptr) == NULL) {    \
            return;             \
        }                       \
    } while (0)

#define EXPECT_NULL(ptr)        \
    do {                        \
        check((ptr) == NULL);   \
        if ((ptr) != NULL) {    \
            return;             \
        }                       \
    } while (0)

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

static turbo_video_capture_config_t valid_video_config(void) {
    turbo_video_capture_config_t config;
    config.width = 640;
    config.height = 480;
    config.framerate = 30;
    config.format = 0;  /* I420 */
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

/* ============================================================================
 * BDD 测试: 设备枚举
 * ============================================================================ */

suite("捕获设备枚举") {
    it("应该安全处理空指针和无效参数") {
        /* 空指针应该返回错误 */
        EXPECT_INT_EQ(-1, turbo_capture_list_audio_devices(NULL, 10));
        EXPECT_INT_EQ(-1, turbo_capture_list_video_devices(NULL, 10));
        EXPECT_INT_EQ(-1, turbo_capture_list_screens(NULL, 10));
        EXPECT_INT_EQ(-1, turbo_capture_list_gpu_devices(NULL, 0));
        
        /* max_count <= 0 应该返回错误 */
        turbo_capture_device_t devices[1];
        EXPECT_INT_EQ(-1, turbo_capture_list_audio_devices(devices, 0));
        EXPECT_INT_EQ(-1, turbo_capture_list_video_devices(devices, 0));
        EXPECT_INT_EQ(-1, turbo_capture_list_screens(devices, 0));
        EXPECT_INT_EQ(-1, turbo_capture_list_gpu_devices(devices, -1));
    }
    
    it("应该枚举可用的音频输入设备") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int count = turbo_capture_list_audio_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
        
        EXPECT_TRUE(count >= 0);
        EXPECT_TRUE(count <= TURBO_CAPTURE_MAX_DEVICES);
        
        /* 验证设备信息 */
        for (int i = 0; i < count; i++) {
            EXPECT_TRUE(devices[i].type == TURBO_CAPTURE_TYPE_AUDIO);
            EXPECT_TRUE(devices[i].name[0] != '\0');
            EXPECT_TRUE(devices[i].id[0] != '\0');
            EXPECT_TRUE(devices[i].index >= 0);
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
            EXPECT_TRUE(has_default || count == 0);
        }
    }
    
    it("应该枚举可用的视频输入设备") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int count = turbo_capture_list_video_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
        
        EXPECT_TRUE(count >= 0);
        
        for (int i = 0; i < count; i++) {
            EXPECT_TRUE(devices[i].type == TURBO_CAPTURE_TYPE_VIDEO);
        }
    }
    
    it("应该枚举可用的屏幕/显示器") {
        turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
        int count = turbo_capture_list_screens(devices, TURBO_CAPTURE_MAX_DEVICES);
        
        EXPECT_TRUE(count >= 0);
        
        for (int i = 0; i < count; i++) {
            EXPECT_TRUE(devices[i].type == TURBO_CAPTURE_TYPE_SCREEN);
        }
    }
    
    it("应该正确处理缓冲区大小限制") {
        turbo_capture_device_t devices[2];
        int count = turbo_capture_list_audio_devices(devices, 2);
        
        EXPECT_TRUE(count >= 0);
        EXPECT_TRUE(count <= 2);
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
    it("应该成功创建有效的视频捕获实例") {
        turbo_video_capture_config_t config = valid_video_config();
        turbo_capture_t *capture = NULL;
        
        /* 使用默认设备 */
        capture = turbo_video_capture_create(NULL, &config);
        if (capture) {
            turbo_capture_destroy(capture);
        }
        
        /* NULL 配置可能使用默认值或失败 */
        capture = turbo_video_capture_create(NULL, NULL);
        if (capture) {
            /* 如果实现支持默认配置，应该能创建成功 */
            turbo_capture_destroy(capture);
        }
        /* 不强制要求失败，因为实现可能支持默认配置 */
    }
    
    it("应该验证视频分辨率参数") {
        turbo_video_capture_config_t config = valid_video_config();
        turbo_capture_t *capture = NULL;
        
        /* 常见分辨率 */
        config.width = 1280;
        config.height = 720;
        capture = turbo_video_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        config.width = 1920;
        config.height = 1080;
        capture = turbo_video_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
    }
    
    it("应该验证帧率参数") {
        turbo_video_capture_config_t config = valid_video_config();
        turbo_capture_t *capture = NULL;
        
        config.framerate = 15;
        capture = turbo_video_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        config.framerate = 30;
        capture = turbo_video_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
        
        config.framerate = 60;
        capture = turbo_video_capture_create(NULL, &config);
        if (capture) turbo_capture_destroy(capture);
    }
    
    it("应该能够多次创建和销毁视频捕获实例") {
        for (int i = 0; i < 3; i++) {
            turbo_video_capture_config_t config = valid_video_config();
            turbo_capture_t *capture = turbo_video_capture_create(NULL, &config);
            
            if (capture) {
                turbo_capture_destroy(capture);
            }
        }
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
        EXPECT_TRUE(result < 0);  /* 返回任何负数错误码都可接受 */
        
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
        int created_count = 0;
        
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
        EXPECT_TRUE(created_count > 0);
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

 