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
            check_equal(audio_count, 1);
            check_equal(devices[0].type, TURBO_CAPTURE_TYPE_AUDIO);
            check_true(devices[0].is_default);
            check_equal(devices[0].id, "default");

            int video_count = turbo_capture_list_video_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
            check_equal(video_count, 2);
            check_equal(devices[0].type, TURBO_CAPTURE_TYPE_VIDEO);
            check_equal(devices[0].id, "back");
            check_equal(devices[1].id, "front");

            int screen_count = turbo_capture_list_screens(devices, TURBO_CAPTURE_MAX_DEVICES);
            check_equal(screen_count, 1);
            check_equal(devices[0].type, TURBO_CAPTURE_TYPE_SCREEN);
            check_equal(devices[0].id, "screen:0");

            check_equal(turbo_capture_list_gpu_devices(devices, TURBO_CAPTURE_MAX_DEVICES), 0);
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

            check_equal(initial_state, TURBO_CAPTURE_STATE_STOPPED);
            check_equal(result, TURBO_CAPTURE_OK);
            check_equal(running_state, TURBO_CAPTURE_STATE_RUNNING);
            check_true(received_data);
            check_greater(sample_size, 0);
            check_equal(stopped_state, TURBO_CAPTURE_STATE_STOPPED);
            check_equal(last_state, TURBO_CAPTURE_STATE_STOPPED);
            check_greater_equal(state_callbacks, 2);
        }
    }

    group("video capture") {
        it("enumerates and captures an exact Camera2 native mode") {
            turbo_video_native_mode_t modes[TURBO_CAPTURE_MAX_VIDEO_MODES];
            const turbo_video_native_mode_t *selected_mode = NULL;
            turbo_video_device_t *device = NULL;
            turbo_capture_t *capture = NULL;
            capture_observer_t observer;
            size_t mode_count = 0;
            int result;

            result = turbo_video_device_open("back", &device);
            check_equal(result, TURBO_CAPTURE_OK);
            check_not_null(device);
            if (!device) return;

            result = turbo_video_device_list_modes(
                device, modes, TURBO_CAPTURE_MAX_VIDEO_MODES, &mode_count);
            check_equal(result, TURBO_CAPTURE_OK);
            check_true(mode_count > 0);
            if (result != TURBO_CAPTURE_OK || mode_count == 0) {
                turbo_video_device_close(device);
                return;
            }

            for (size_t i = 0; i < mode_count; ++i) {
                if (modes[i].format != TURBO_VIDEO_CAPTURE_FORMAT_I420) continue;
                if (!selected_mode) selected_mode = &modes[i];
                if (modes[i].width == 640 && modes[i].height == 480) {
                    selected_mode = &modes[i];
                    break;
                }
            }
            check_not_null(selected_mode);
            if (!selected_mode) {
                turbo_video_device_close(device);
                return;
            }

            result = turbo_video_device_create_capture(
                device, selected_mode, &capture);
            turbo_video_device_close(device);
            check_equal(result, TURBO_CAPTURE_OK);
            check_not_null(capture);
            if (!capture) return;

            observer_init(&observer);
            turbo_video_capture_set_callback(capture, on_video, &observer);
            turbo_capture_on_state(capture, on_state);
            result = turbo_capture_start(capture);
            check_equal(result, TURBO_CAPTURE_OK);
            if (result == TURBO_CAPTURE_OK) {
                check_true(wait_for_data(&observer, VIDEO_CAPTURE_WAIT_STEPS));
                check_equal(atomic_load(&observer.last_width),
                             selected_mode->width);
                check_equal(atomic_load(&observer.last_height),
                             selected_mode->height);
                check_greater(atomic_load(&observer.last_size), 0);
                turbo_capture_stop(capture);
            }
            turbo_capture_destroy(capture);
        }

        it("reports unsupported Camera2 controls without corrupting outputs") {
            turbo_camera_control_range_t range = {1, 2, 3, 4, 5};
            turbo_video_crop_t crop = {1, 2, 3, 4};
            int value = 7;

            int range_result = turbo_video_capture_get_control_range(
                NULL, TURBO_CAMERA_CONTROL_ZOOM, &range);
            int set_control_result = turbo_video_capture_set_control(
                NULL, TURBO_CAMERA_CONTROL_ZOOM, 200);
            int get_control_result = turbo_video_capture_get_control(
                NULL, TURBO_CAMERA_CONTROL_ZOOM, &value);
            int set_crop_result = turbo_video_capture_set_crop(NULL, &crop);
            int get_crop_result = turbo_video_capture_get_crop(NULL, &crop);

            check_equal(range_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_equal(range.min_value, 0);
            check_equal(set_control_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_equal(get_control_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_equal(value, 0);
            check_equal(set_crop_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_equal(get_crop_result, TURBO_CAPTURE_ERR_UNSUPPORTED);
            check_equal(crop.width, 0);
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

            check_equal(type, TURBO_CAPTURE_TYPE_SCREEN);
            check_equal(initial_state, TURBO_CAPTURE_STATE_STOPPED);
            check_equal(result, TURBO_CAPTURE_OK);
            check_equal(running_state, TURBO_CAPTURE_STATE_RUNNING);
            check_equal(stopped_state, TURBO_CAPTURE_STATE_STOPPED);
            check_equal(last_state, TURBO_CAPTURE_STATE_STOPPED);
            check_greater_equal(state_callbacks, 2);
        }
    }
}
