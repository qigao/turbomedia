#ifndef TURBO_MEDIA_STREAMER_CHTTP_UPLOAD_H
#define TURBO_MEDIA_STREAMER_CHTTP_UPLOAD_H

#include <chttp/chttp.h>
#include <stdint.h>

int turbo_streamer_chttp_post_file(
    chttp_client *client, const chttp_tls_profile *tls_profile,
    const char *url, const char *path, uint32_t timeout_ms);

#endif
