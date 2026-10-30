#ifndef TURBO_MEDIA_WEBP_XMP_INTERNAL_H
#define TURBO_MEDIA_WEBP_XMP_INTERNAL_H
#include "turbo_media_encoder.h"
#ifdef __cplusplus
extern "C" {
#endif

/* TurboMedia-private RIFF WebP XMP writer. It owns no memory and borrows the
 * encoded WebP packet only for this synchronous call. Intended to be exercised
 * by the in-repo native encoder tests even when the selected FFmpeg build
 * lacks a WebP encoder. Not installed as a consumer API.
 *
 * xmp is a fully prepared UTF-8 XMP XML document, NOT raw sd parameters.
 * Accepts valid still-image VP8/VP8L or extended VP8X without preexisting XMP.
 * Rejects animation, duplicate XMP, unsupported chunks, malformed RIFF,
 * invalid dimensions, size/overflow errors and sink refusal.
 * The caller MUST discard all output on any non-OK result.
 */
TURBO_MEDIA_API tm_encoder_status tm_encoder_webp_wrap_xmp(
    const uint8_t *riff, size_t riff_size,
    uint32_t width, uint32_t height,
    const uint8_t *xmp, size_t xmp_size,
    size_t max_output_bytes,
    tm_encoder_write_fn write, void *user, size_t *written);

#ifdef __cplusplus
}
#endif
#endif
