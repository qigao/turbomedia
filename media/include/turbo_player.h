/**
 * TurboMedia universal player
 *
 * FFmpeg-backed demux/decode layer.
 */
#ifndef TURBO_PLAYER_H
#define TURBO_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_player_s turbo_player_t;

typedef enum {
    TURBO_PLAYER_OK = 0,
    TURBO_PLAYER_ERR_NOMEM = -1,
    TURBO_PLAYER_ERR_OPEN = -2,
    TURBO_PLAYER_ERR_STREAM = -3,
    TURBO_PLAYER_ERR_CODEC = -4,
    TURBO_PLAYER_ERR_PLAYBACK = -5
} turbo_player_result_t;

typedef enum {
    TURBO_PLAYER_VIDEO_RGBA = 0,
    TURBO_PLAYER_VIDEO_I420 = 1
} turbo_player_video_format_t;

typedef struct {
    int play_audio;                         /* Decode audio into turbo_playback. */
    const char *audio_device_id;            /* NULL selects default output device. */
    int audio_sample_rate;                  /* 0 keeps source rate, fallback 48000. */
    int audio_channels;                     /* 0 keeps source channels, fallback 2. */
    int audio_buffer_ms;                    /* 0 uses 500 ms. */
    turbo_player_video_format_t video_format;
} turbo_player_config_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    int width;
    int height;
    int stride[4];
    int64_t pts_ms;
    turbo_player_video_format_t format;
} turbo_player_video_frame_t;

typedef void (*turbo_player_video_cb)(turbo_player_t *player,
                                      const turbo_player_video_frame_t *frame,
                                      void *user_data);

typedef void (*turbo_player_audio_cb)(turbo_player_t *player,
                                      const float *samples,
                                      size_t frame_count,
                                      int sample_rate,
                                      int channels,
                                      int64_t pts_ms,
                                      void *user_data);

typedef void (*turbo_player_complete_cb)(turbo_player_t *player, void *user_data);

TURBO_MEDIA_API turbo_player_t *turbo_player_open(const char *url,
                                            const turbo_player_config_t *config);
TURBO_MEDIA_API void turbo_player_close(turbo_player_t *player);

TURBO_MEDIA_API void turbo_player_set_video_callback(turbo_player_t *player,
                                               turbo_player_video_cb cb,
                                               void *user_data);
TURBO_MEDIA_API void turbo_player_set_audio_callback(turbo_player_t *player,
                                               turbo_player_audio_cb cb,
                                               void *user_data);
TURBO_MEDIA_API void turbo_player_on_complete(turbo_player_t *player,
                                        turbo_player_complete_cb cb,
                                        void *user_data);

TURBO_MEDIA_API int turbo_player_play_to_end(turbo_player_t *player);
TURBO_MEDIA_API int turbo_player_start(turbo_player_t *player);
TURBO_MEDIA_API int turbo_player_wait(turbo_player_t *player);
TURBO_MEDIA_API int turbo_player_is_running(turbo_player_t *player);
TURBO_MEDIA_API void turbo_player_stop(turbo_player_t *player);
TURBO_MEDIA_API void turbo_player_pause(turbo_player_t *player);
TURBO_MEDIA_API void turbo_player_resume(turbo_player_t *player);
TURBO_MEDIA_API int turbo_player_seek_ms(turbo_player_t *player, int64_t position_ms);

TURBO_MEDIA_API int64_t turbo_player_get_duration_ms(turbo_player_t *player);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_PLAYER_H */
