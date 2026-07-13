#include "turbo_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TURBO_MEDIA_HAS_FFMPEG

typedef struct {
    size_t audio_frames;
    size_t video_frames;
    int64_t first_audio_pts_ms;
    int64_t first_video_pts_ms;
    int64_t last_audio_pts_ms;
    int64_t last_video_pts_ms;
} probe_stats_t;

static void on_audio(turbo_player_t *player,
                     const float *samples,
                     size_t frame_count,
                     int sample_rate,
                     int channels,
                     int64_t pts_ms,
                     void *user_data) {
    probe_stats_t *stats = (probe_stats_t *)user_data;
    (void)player;
    (void)samples;
    (void)sample_rate;
    (void)channels;
    if (stats->audio_frames == 0) {
        stats->first_audio_pts_ms = pts_ms;
    }
    stats->last_audio_pts_ms = pts_ms;
    stats->audio_frames += frame_count;
}

static void on_video(turbo_player_t *player,
                     const turbo_player_video_frame_t *frame,
                     void *user_data) {
    probe_stats_t *stats = (probe_stats_t *)user_data;
    (void)player;
    (void)frame;
    if (stats->video_frames == 0) {
        stats->first_video_pts_ms = frame->pts_ms;
    }
    stats->last_video_pts_ms = frame->pts_ms;
    stats->video_frames++;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: ffmpeg_probe <media-file-or-url> [--seek-ms N] [--async]\n");
        return 0;
    }

    turbo_player_config_t config = {0};
    config.play_audio = 0;
    config.audio_sample_rate = 48000;
    config.audio_channels = 2;
    config.video_format = TURBO_PLAYER_VIDEO_RGBA;

    turbo_player_t *player = turbo_player_open(argv[1], &config);
    if (!player) {
        printf("failed to open %s\n", argv[1]);
        return 1;
    }

    probe_stats_t stats = {0};
    turbo_player_set_audio_callback(player, on_audio, &stats);
    turbo_player_set_video_callback(player, on_video, &stats);

    int use_async = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--async") == 0) {
            use_async = 1;
        } else if (strcmp(argv[i], "--seek-ms") == 0 && i + 1 < argc) {
            int64_t position_ms = (int64_t)strtoll(argv[++i], NULL, 10);
            if (turbo_player_seek_ms(player, position_ms) != 0) {
                printf("failed to seek to %lld ms\n", (long long)position_ms);
                turbo_player_close(player);
                return 1;
            }
        }
    }

    int rc;
    if (use_async) {
        rc = turbo_player_start(player);
        if (rc == 0) {
            rc = turbo_player_wait(player);
        }
    } else {
        rc = turbo_player_play_to_end(player);
    }
    printf("duration=%lld ms audio_frames=%zu video_frames=%zu "
           "audio_pts=%lld..%lld video_pts=%lld..%lld rc=%d\n",
           (long long)turbo_player_get_duration_ms(player),
           stats.audio_frames,
           stats.video_frames,
           (long long)stats.first_audio_pts_ms,
           (long long)stats.last_audio_pts_ms,
           (long long)stats.first_video_pts_ms,
           (long long)stats.last_video_pts_ms,
           rc);

    turbo_player_close(player);
    return rc == 0 ? 0 : 1;
}

#else

int main(void) {
    printf("ffmpeg_probe requires TURBO_MEDIA_ENABLE_FFMPEG=ON\n");
    return 0;
}

#endif
