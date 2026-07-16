#include <stdatomic.h>
#include <stdint.h>

#include <tinytest.h>
#include <turbo_capture.h>
#include <turbo_thread.h>

enum {
    CAPTURE_WAIT_STEP_MS = 20,
    AUDIO_CAPTURE_WAIT_STEPS = 50,
    VIDEO_CAPTURE_WAIT_STEPS = 100
};

typedef struct {
    atomic_int data_callbacks;
    atomic_int state_callbacks;
    atomic_int last_state;
    atomic_size_t last_size;
    atomic_int last_width;
    atomic_int last_height;
} capture_observer_t;

static void observer_init(capture_observer_t *observer) {
    atomic_init(&observer->data_callbacks, 0);
    atomic_init(&observer->state_callbacks, 0);
    atomic_init(&observer->last_state, TURBO_CAPTURE_STATE_STOPPED);
    atomic_init(&observer->last_size, 0);
    atomic_init(&observer->last_width, 0);
    atomic_init(&observer->last_height, 0);
}

static void on_audio(turbo_capture_t *capture,
                     const uint8_t *samples,
                     size_t len,
                     uint64_t timestamp,
                     void *user_data) {
    capture_observer_t *observer = (capture_observer_t *)user_data;
    (void)capture;
    (void)timestamp;
    if (!observer || !samples || len == 0) return;
    atomic_store(&observer->last_size, len);
    atomic_fetch_add(&observer->data_callbacks, 1);
}

static void on_video(turbo_capture_t *capture,
                     const uint8_t *frame,
                     size_t len,
                     int width,
                     int height,
                     uint64_t timestamp,
                     void *user_data) {
    capture_observer_t *observer = (capture_observer_t *)user_data;
    (void)capture;
    (void)timestamp;
    if (!observer || !frame || len == 0 || width <= 0 || height <= 0) return;
    atomic_store(&observer->last_size, len);
    atomic_store(&observer->last_width, width);
    atomic_store(&observer->last_height, height);
    atomic_fetch_add(&observer->data_callbacks, 1);
}

static void on_state(turbo_capture_t *capture,
                     turbo_capture_state_t state,
                     void *user_data) {
    capture_observer_t *observer = (capture_observer_t *)user_data;
    (void)capture;
    if (!observer) return;
    atomic_store(&observer->last_state, state);
    atomic_fetch_add(&observer->state_callbacks, 1);
}

static int wait_for_data(capture_observer_t *observer, int steps) {
    for (int i = 0; i < steps; ++i) {
        if (atomic_load(&observer->data_callbacks) > 0) return 1;
        turbo_sleep_ms(CAPTURE_WAIT_STEP_MS);
    }
    return atomic_load(&observer->data_callbacks) > 0;
}

spec("Android capture backends") {
    group("device enumeration") {
        it("reports the Android audio, camera, and screen endpoints") {
            turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];

            int audio_count = turbo_capture_list_audio_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
            check_int_eq(audio_count, 1);
            check_int_eq(devices[0].type, TURBO_CAPTURE_TYPE_AUDIO);
            check_true(devices[0].is_default);
            check_str_eq(devices[0].id, "default");

            int video_count = turbo_capture_list_video_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
            check_int_eq(video_count, 2);
            check_int_eq(devices[0].type, TURBO_CAPTURE_TYPE_VIDEO);
            check_str_eq(devices[0].id, "back");
            check_str_eq(devices[1].id, "front");

            int screen_count = turbo_capture_list_screens(devices, TURBO_CAPTURE_MAX_DEVICES);
            check_int_eq(screen_count, 1);
            check_int_eq(devices[0].type, TURBO_CAPTURE_TYPE_SCREEN);
            check_str_eq(devices[0].id, "screen:0");

            check_int_eq(turbo_capture_list_gpu_devices(devices, TURBO_CAPTURE_MAX_DEVICES), 0);
        }
    }

    group("audio capture") {
        it("captures microphone samples and reports lifecycle state") {
            turbo_audio_capture_config_t config = {48000, 1, 16, 10};
            capture_observer_t observer;
            observer_init(&observer);

            turbo_capture_t *capture = turbo_audio_capture_create(NULL, &config);
            check_not_null(capture);
            if (!capture) return;

            turbo_capture_state_t initial_state = turbo_capture_get_state(capture);
            turbo_audio_capture_set_callback(capture, on_audio, &observer);
            turbo_capture_on_state(capture, on_state);

            int result = turbo_capture_start(capture);
            int received_data = 0;
            size_t sample_size = 0;
            turbo_capture_state_t running_state = turbo_capture_get_state(capture);
            if (result == TURBO_CAPTURE_OK) {
                received_data = wait_for_data(&observer, AUDIO_CAPTURE_WAIT_STEPS);
                sample_size = atomic_load(&observer.last_size);
                turbo_capture_stop(capture);
            }

            turbo_capture_state_t stopped_state = turbo_capture_get_state(capture);
            int last_state = atomic_load(&observer.last_state);
            int state_callbacks = atomic_load(&observer.state_callbacks);
            turbo_capture_destroy(capture);

            check_int_eq(initial_state, TURBO_CAPTURE_STATE_STOPPED);
            check_int_eq(result, TURBO_CAPTURE_OK);
            check_int_eq(running_state, TURBO_CAPTURE_STATE_RUNNING);
            check_true(received_data);
            check_size_gt(sample_size, 0);
            check_int_eq(stopped_state, TURBO_CAPTURE_STATE_STOPPED);
            check_int_eq(last_state, TURBO_CAPTURE_STATE_STOPPED);
            check_int_ge(state_callbacks, 2);
        }
    }

    group("video capture") {
        it("captures Camera2 frames with the requested dimensions") {
            turbo_video_capture_config_t config = {640, 480, 30, 0};
            capture_observer_t observer;
            observer_init(&observer);

            turbo_capture_t *capture = turbo_video_capture_create("back", &config);
            check_not_null(capture);
            if (!capture) return;

            turbo_video_capture_set_callback(capture, on_video, &observer);
            turbo_capture_on_state(capture, on_state);

            int result = turbo_capture_start(capture);
            int received_data = 0;
            int frame_width = 0;
            int frame_height = 0;
            size_t frame_size = 0;
            if (result == TURBO_CAPTURE_OK) {
                received_data = wait_for_data(&observer, VIDEO_CAPTURE_WAIT_STEPS);
                frame_width = atomic_load(&observer.last_width);
                frame_height = atomic_load(&observer.last_height);
                frame_size = atomic_load(&observer.last_size);
                turbo_capture_stop(capture);
            }

            turbo_capture_state_t stopped_state = turbo_capture_get_state(capture);
            turbo_capture_destroy(capture);

            check_int_eq(result, TURBO_CAPTURE_OK);
            check_true(received_data);
            check_int_eq(frame_width, config.width);
            check_int_eq(frame_height, config.height);
            check_size_eq(frame_size,
                          (size_t)config.width * (size_t)config.height * 3u / 2u);
            check_int_eq(stopped_state, TURBO_CAPTURE_STATE_STOPPED);
        }

        it("reports unsupported Camera2 controls without corrupting outputs") {
            turbo_video_capture_config_t config = {640, 480, 30, 0};
            turbo_camera_control_range_t range = {1, 2, 3, 4, 5};
            turbo_video_crop_t crop = {1, 2, 3, 4};
            int value = 7;
            turbo_capture_t *capture = turbo_video_capture_create("back", &config);
            check_not_null(capture);
            if (!capture) return;

            int range_result = turbo_video_capture_get_control_range(
                capture, TURBO_CAMERA_CONTROL_ZOOM, &range);
            int set_control_result = turbo_video_capture_set_control(
                capture, TURBO_CAMERA_CONTROL_ZOOM, 200);
            int get_control_result = turbo_video_capture_get_control(
                capture, TURBO_CAMERA_CONTROL_ZOOM, &value);
            int set_crop_result = turbo_video_capture_set_crop(capture, &crop);
            int get_crop_result = turbo_video_capture_get_crop(capture, &crop);

            turbo_capture_destroy(capture);

            check_int_eq(range_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_int_eq(range.min_value, 0);
            check_int_eq(set_control_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_int_eq(get_control_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_int_eq(value, 0);
            check_int_eq(set_crop_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_int_eq(get_crop_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_int_eq(crop.width, 0);
        }
    }

    group("screen capture") {
        it("creates a screen surface and completes the native lifecycle") {
            turbo_screen_capture_config_t config = {0, 30, 1, 0};
            capture_observer_t observer;
            observer_init(&observer);

            turbo_capture_t *capture = turbo_screen_capture_create(&config);
            check_not_null(capture);
            if (!capture) return;

            turbo_capture_type_t type = capture->type;
            turbo_capture_state_t initial_state = turbo_capture_get_state(capture);
            turbo_screen_capture_set_callback(capture, on_video, &observer);
            turbo_capture_on_state(capture, on_state);

            int result = turbo_capture_start(capture);
            turbo_capture_state_t running_state = turbo_capture_get_state(capture);
            if (result == TURBO_CAPTURE_OK) {
                turbo_capture_stop(capture);
            }

            turbo_capture_state_t stopped_state = turbo_capture_get_state(capture);
            int last_state = atomic_load(&observer.last_state);
            int state_callbacks = atomic_load(&observer.state_callbacks);
            turbo_capture_destroy(capture);

            check_int_eq(type, TURBO_CAPTURE_TYPE_SCREEN);
            check_int_eq(initial_state, TURBO_CAPTURE_STATE_STOPPED);
            check_int_eq(result, TURBO_CAPTURE_OK);
            check_int_eq(running_state, TURBO_CAPTURE_STATE_RUNNING);
            check_int_eq(stopped_state, TURBO_CAPTURE_STATE_STOPPED);
            check_int_eq(last_state, TURBO_CAPTURE_STATE_STOPPED);
            check_int_ge(state_callbacks, 2);
        }
    }
}
