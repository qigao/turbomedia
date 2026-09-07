#include <salts_playback.h>
#include <tinytest.h>
#include <turbo_player.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void player_test_sleep_ms(unsigned int milliseconds) {
    Sleep(milliseconds);
}
#else
#include <time.h>
static void player_test_sleep_ms(unsigned int milliseconds) {
    struct timespec duration = {
        .tv_sec = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L};
    nanosleep(&duration, NULL);
}
#endif

enum {
    PLAYER_TEST_SAMPLE_RATE = 8000,
    PLAYER_TEST_SAMPLE_COUNT = 80,
    PLAYER_TEST_WAV_HEADER_SIZE = 44
};

static void write_u16_le(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value & 0xffu);
    destination[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xffu);
    destination[1] = (uint8_t)((value >> 8u) & 0xffu);
    destination[2] = (uint8_t)((value >> 16u) & 0xffu);
    destination[3] = (uint8_t)(value >> 24u);
}

static int write_player_test_wav(const char *path) {
    const size_t data_size = PLAYER_TEST_SAMPLE_COUNT * sizeof(int16_t);
    const size_t file_size = PLAYER_TEST_WAV_HEADER_SIZE + data_size;
    uint8_t *wav = (uint8_t *)calloc(1, file_size);
    int result;
    if (!wav) {
        return -1;
    }
    memcpy(wav, "RIFF", 4);
    write_u32_le(wav + 4, (uint32_t)(file_size - 8u));
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_u32_le(wav + 16, 16);
    write_u16_le(wav + 20, 1);
    write_u16_le(wav + 22, 1);
    write_u32_le(wav + 24, PLAYER_TEST_SAMPLE_RATE);
    write_u32_le(wav + 28, PLAYER_TEST_SAMPLE_RATE * sizeof(int16_t));
    write_u16_le(wav + 32, sizeof(int16_t));
    write_u16_le(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    write_u32_le(wav + 40, (uint32_t)data_size);
    result = tt_write_file(path, wav, file_size);
    free(wav);
    return result;
}

typedef struct player_test_control {
    _Atomic int audio_entered;
    _Atomic int release_audio;
    _Atomic int completion_count;
} player_test_control_t;

static void block_audio_callback(turbo_player_t *player, const float *samples,
                                 size_t frame_count, int sample_rate,
                                 int channels, int64_t pts_ms,
                                 void *user_data) {
    player_test_control_t *control = (player_test_control_t *)user_data;
    (void)player;
    (void)samples;
    (void)frame_count;
    (void)sample_rate;
    (void)channels;
    (void)pts_ms;
    atomic_store_explicit(&control->audio_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&control->release_audio,
                                 memory_order_acquire)) {
        player_test_sleep_ms(1);
    }
}

static void count_completion(turbo_player_t *player, void *user_data) {
    player_test_control_t *control = (player_test_control_t *)user_data;
    (void)player;
    atomic_fetch_add_explicit(&control->completion_count, 1,
                              memory_order_relaxed);
}

suite("Player Salts playback contract") {
    char *path;

    path = tt_make_temp_file("turbomedia-player-audio-", ".wav");
    check_not_null(path);
    if (!path) {
        return;
    }
    check_equal(write_player_test_wav(path), 0);

    it("opens the audio fixture when device playback is disabled") {
        turbo_player_config_t config = {0};
        turbo_player_t *player = turbo_player_open(path, &config);
        check_not_null(player);
        turbo_player_close(player);
    }

    it("reports natural completion exactly once") {
        turbo_player_config_t config = {0};
        player_test_control_t control = {0};
        turbo_player_t *player = turbo_player_open(path, &config);
        check_not_null(player);
        if (player) {
            turbo_player_on_complete(player, count_completion, &control);
            check_equal(turbo_player_play_to_end(player), TURBO_PLAYER_OK);
            check_equal(atomic_load_explicit(&control.completion_count,
                                             memory_order_relaxed),
                        1);
            turbo_player_close(player);
        }
    }

    it("lets stop win without reporting completion") {
        enum { PLAYER_TEST_CALLBACK_WAIT_MS = 1000 };
        turbo_player_config_t config = {0};
        player_test_control_t control = {0};
        turbo_player_t *player = turbo_player_open(path, &config);
        check_not_null(player);
        if (player) {
            turbo_player_set_audio_callback(player, block_audio_callback,
                                            &control);
            turbo_player_on_complete(player, count_completion, &control);
            check_equal(turbo_player_start(player), TURBO_PLAYER_OK);
            for (unsigned int elapsed = 0;
                 elapsed < PLAYER_TEST_CALLBACK_WAIT_MS &&
                 !atomic_load_explicit(&control.audio_entered,
                                       memory_order_acquire);
                 ++elapsed) {
                player_test_sleep_ms(1);
            }
            check_true(atomic_load_explicit(&control.audio_entered,
                                            memory_order_acquire));
            turbo_player_stop(player);
            atomic_store_explicit(&control.release_audio, 1,
                                  memory_order_release);
            check_equal(turbo_player_wait(player), TURBO_PLAYER_OK);
            check_equal(atomic_load_explicit(&control.completion_count,
                                             memory_order_relaxed),
                        0);
            turbo_player_close(player);
        }
    }

    it("rejects an invalid Salts device identity") {
        salts_playback_device_t invalid_device = {0};
        turbo_player_config_t config = {0};
        config.play_audio = 1;
        config.audio_device = &invalid_device;
        check_null(turbo_player_open(path, &config));
    }

    it("rejects a playback sample rate outside the Salts contract") {
        turbo_player_config_t config = {0};
        config.play_audio = 1;
        config.audio_sample_rate = 44100;
        check_null(turbo_player_open(path, &config));
    }

    it("rejects a playback buffer outside the Salts contract") {
        turbo_player_config_t config = {0};
        config.play_audio = 1;
        config.audio_buffer_ms = SALTS_PLAYBACK_MIN_BUFFER_MS - 1;
        check_null(turbo_player_open(path, &config));
    }

    check_equal(remove(path), 0);
    free(path);
}
