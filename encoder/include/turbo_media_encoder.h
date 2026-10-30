#ifndef TURBO_MEDIA_ENCODER_H
#define TURBO_MEDIA_ENCODER_H
#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TM_ENCODER_IMAGE_V1 1u
#define TM_ENCODER_MAX_IMAGE_SIDE 4096u

typedef enum tm_encoder_status {
    TM_ENCODER_OK = 0,
    TM_ENCODER_EINVAL = -1,
    TM_ENCODER_EVERSION = -2,
    TM_ENCODER_EUNSUPPORTED = -3,
    TM_ENCODER_ENOMEM = -4,
    TM_ENCODER_ECAPACITY = -5,
    TM_ENCODER_ECODEC = -6,
    TM_ENCODER_ESINK = -7,
    TM_ENCODER_ECANCELLED = -8
} tm_encoder_status;

typedef enum tm_encoder_image_format {
    TM_ENCODER_IMAGE_PNG = 1,
    TM_ENCODER_IMAGE_JPEG = 2,
    TM_ENCODER_IMAGE_WEBP = 3
} tm_encoder_image_format;
typedef enum tm_encoder_pixel_format {
    TM_ENCODER_PIXEL_GRAY8 = 1,
    TM_ENCODER_PIXEL_GRAY_ALPHA8 = 2,
    TM_ENCODER_PIXEL_RGB24 = 3,
    TM_ENCODER_PIXEL_RGBA32 = 4
} tm_encoder_pixel_format;

typedef struct tm_encoder_image_v1 {
    uint32_t size, version;
    uint32_t output_format, pixel_format;
    uint32_t width, height, stride_bytes, quality;
    const uint8_t *pixels;     /* BORROWED until call returns */
    size_t pixels_capacity;
    const char *metadata_utf8;
    size_t metadata_bytes;
    size_t max_output_bytes;   /* mandatory encoded-byte limit */
    uint32_t reserved;
} tm_encoder_image_v1;
#define TM_ENCODER_IMAGE_V1_INIT { \
    sizeof(tm_encoder_image_v1), TM_ENCODER_IMAGE_V1, \
    TM_ENCODER_IMAGE_PNG, TM_ENCODER_PIXEL_RGB24, \
    0u, 0u, 0u, 90u, NULL, 0u, NULL, 0u, 0u, 0u }

/* Zero means accepted; nonzero aborts. C++ callers must not throw through
 * this C callback. On ANY non-OK status the sink's bytes are discarded by
 * the caller (the sink may already have received an incomplete prefix). */
typedef int (*tm_encoder_write_fn)(void *user, const uint8_t *bytes, size_t size);

/* Exact selected FFmpeg backend only: no fallback, no other codec. */
TURBO_MEDIA_API int tm_encoder_image_available(uint32_t output_format);

/* Versioned, owner-serialized C11 one-frame image encoding.
 * The API owns all codec contexts and packets; no FFmpeg types are public.
 * Quality is 1..100 for lossy formats, ignored for lossless PNG.
 * Metadata bytes are the caller-prepared sd-style "parameters" string.
 * PNG adds the parameters tEXt chunk after IHDR; JPEG adds its exact COM
 * marker. Neither call borrows metadata after returning.
 *
 * LIMITATION: WebP XMP metadata, AVI/animated WebP and VP8+WebM encoding
 * remain unsupported. WebP with metadata fails EUNSUPPORTED BEFORE any
 * sink call; do not migrate sd until all selected formats preserve metadata.
 * Output byte cap is enforced before sink delivery, NOT before internal
 * FFmpeg packet/codec allocation; it is not a process RSS/GPU budget.
 */
TURBO_MEDIA_API tm_encoder_status tm_encoder_image_encode(
    const tm_encoder_image_v1 *options,
    tm_encoder_write_fn write,
    void *user,
    size_t *written);


/* C11 one-shot video frame contract. All pixel arrays are borrowed only
 * while tm_encoder_webm_encode() executes; they are never retained.
 * V1 supports even-sized packed Gray8/RGB24/RGBA32 -> I420 -> VP8 in
 * TurboMedia::Codec, then in-memory TurboMedia::Muxer WebM.
 * Audio is intentionally NOT accepted in V1; a requested soundtrack
 * must fail loudly rather than being dropped from the exported media.
 */
#define TM_ENCODER_VIDEO_V1 1u
typedef struct tm_encoder_video_frame_v1 {
    uint32_t size, version;
    uint32_t pixel_format, stride_bytes;
    const uint8_t *pixels;
    size_t pixels_capacity;
} tm_encoder_video_frame_v1;
#define TM_ENCODER_VIDEO_FRAME_V1_INIT { \
    sizeof(tm_encoder_video_frame_v1), TM_ENCODER_VIDEO_V1, \
    TM_ENCODER_PIXEL_RGB24, 0u, NULL, 0u }

typedef struct tm_encoder_video_v1 {
    uint32_t size, version;
    uint32_t width, height, fps, bitrate;
    const tm_encoder_video_frame_v1 *frames;
    size_t frame_count;
    const uint8_t *audio_pcm; /* non-NULL or audio_bytes > 0 => EUNSUPPORTED */
    size_t audio_bytes;
    size_t max_output_bytes;
    uint32_t reserved;
} tm_encoder_video_v1;
#define TM_ENCODER_VIDEO_V1_INIT { \
    sizeof(tm_encoder_video_v1), TM_ENCODER_VIDEO_V1, \
    0u, 0u, 24u, 0u, NULL, 0u, NULL, 0u, 0u, 0u }

typedef int (*tm_encoder_should_cancel_fn)(void *user);

/* Encodes one complete raw-video sequence into a single WebM container.
 * Sink is invoked only after success; error/cancel yields no partial file.
 * TurboMedia WebM memory-writer bytes are capped during muxing, and the
 * final container is checked again before publication. libvpx/swscale
 * internal allocations are NOT a full process-RSS bound.
 * Callback may request cancellation between encoded frames, not while a
 * single libvpx encode() call is in progress. No alternate codec/muxer.
 */
TURBO_MEDIA_API tm_encoder_status tm_encoder_webm_encode(
    const tm_encoder_video_v1 *options,
    tm_encoder_write_fn write, void *write_user,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user,
    size_t *written);

#ifdef __cplusplus
}
#endif
#endif
