#ifndef TURBO_MEDIA_HLS_STREAMER_INTERNAL_H
#define TURBO_MEDIA_HLS_STREAMER_INTERNAL_H

#include "turbo_streamer.h"

int turbo_hls_streamer_get_playlist(void *ctx, char **playlist, size_t *size);
void turbo_hls_streamer_set_playlist_type(void *ctx, turbo_hls_playlist_type_t type);

#endif
