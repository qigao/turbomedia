#include "turbo_media_webp_xmp.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* WebP RFC 9649: RIFF header, 8-byte chunk headers, even-byte padding,
 * VP8X flags and XMP after image data. This module never re-encodes image
 * bits or installs a second WebP codec.
 */
static uint32_t tm_webp_le32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}
static uint32_t tm_webp_le24(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16);
}
static void tm_webp_put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}
static void tm_webp_put_le24(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
}

typedef struct tm_webp_write_state {
    tm_encoder_write_fn write;
    void *user;
    size_t limit;
    size_t used;
} tm_webp_write_state;
static tm_encoder_status tm_webp_emit(tm_webp_write_state *state,
                                      const void *data, size_t length) {
    if (length == 0)
        return TM_ENCODER_OK;
    if (!data || length > state->limit - state->used)
        return TM_ENCODER_ECAPACITY;
    if (state->write(state->user, (const uint8_t *)data, length) != 0)
        return TM_ENCODER_ESINK;
    state->used += length;
    return TM_ENCODER_OK;
}

tm_encoder_status tm_encoder_webp_wrap_xmp(
    const uint8_t *riff, size_t riff_size,
    uint32_t width, uint32_t height,
    const uint8_t *xmp, size_t xmp_size,
    size_t max_output_bytes,
    tm_encoder_write_fn write, void *user, size_t *written) {
    static const uint8_t riff_magic[4] = {'R','I','F','F'};
    static const uint8_t webp_magic[4] = {'W','E','B','P'};
    uint8_t header[12];
    uint8_t extended[18] = {'V','P','8','X',10,0,0,0,0,0,0,0,0,0,0,0,0,0};
    uint8_t xmp_header[8] = {'X','M','P',' ',0,0,0,0};
    const uint8_t padding = 0;
    tm_webp_write_state sink;
    tm_encoder_status status;
    size_t position = 12, remaining, source_first_chunk;
    size_t addition, final_size;
    unsigned image_count = 0, seen_vp8x = 0, alpha = 0;
    unsigned seen_xmp = 0, seen_animation = 0;
    unsigned chunk_count = 0;
    int simple = 0;

    if (written)
        *written = 0;
    if (!written || !write || !riff || !xmp || !xmp_size ||
        !max_output_bytes || !width || !height ||
        width > 0x1000000u || height > 0x1000000u ||
        xmp_size > UINT32_MAX || xmp_size > 1024u * 1024u)
        return TM_ENCODER_EINVAL;
    if (riff_size < 20u || riff_size > (size_t)UINT32_MAX ||
        memcmp(riff, riff_magic, 4) != 0 ||
        memcmp(riff + 8, webp_magic, 4) != 0 ||
        tm_webp_le32(riff + 4) != riff_size - 8u)
        return TM_ENCODER_EINVAL;

    /* Parse *every* RIFF chunk before writing even the first byte.
     * Reject duplicate XMP and animated WebP rather than publishing a
     * structurally plausible but semantically incorrect still image.
     */
    source_first_chunk = position;
    while (position < riff_size) {
        size_t length, padded;
        const uint8_t *chunk;
        if (riff_size - position < 8u)
            return TM_ENCODER_EINVAL;
        chunk = riff + position;
        length = tm_webp_le32(chunk + 4);
        remaining = riff_size - position - 8u;
        if (length > remaining)
            return TM_ENCODER_EINVAL;
        if (length & 1u) {
            if (length == SIZE_MAX)
                return TM_ENCODER_EINVAL;
            padded = length + 1u;
        } else {
            padded = length;
        }
        if (padded > remaining)
            return TM_ENCODER_EINVAL;

        if (chunk_count == 0u) {
            simple = memcmp(chunk, "VP8 ", 4) == 0 ||
                     memcmp(chunk, "VP8L", 4) == 0;
            if (!simple && memcmp(chunk, "VP8X", 4) != 0)
                return TM_ENCODER_EUNSUPPORTED;
        }
        if (memcmp(chunk, "VP8X", 4) == 0) {
            uint8_t flags;
            if (chunk_count != 0u || length != 10u || seen_vp8x)
                return TM_ENCODER_EUNSUPPORTED;
            flags = chunk[8]; /* Only read after 10-byte length check. */
            if ((flags & 0x02u) ||
                tm_webp_le24(chunk + 12) + 1u != width ||
                tm_webp_le24(chunk + 15) + 1u != height)
                return TM_ENCODER_EUNSUPPORTED;
            if (flags & 0x04u)
                seen_xmp = 1u;
            seen_vp8x = 1u;
            alpha = flags & 0x10u;
        } else if (memcmp(chunk, "VP8 ", 4) == 0 ||
                   memcmp(chunk, "VP8L", 4) == 0) {
            ++image_count;
            if (image_count != 1u)
                return TM_ENCODER_EINVAL;
            if (memcmp(chunk, "VP8L", 4) == 0) {
                /* Lossless image header encodes alpha-used in bit 4 of
                 * its fifth byte, following its 0x2f signature. */
                if (length < 5u || chunk[8] != 0x2fu)
                    return TM_ENCODER_EINVAL;
                if (chunk[12] & 0x10u)
                    alpha = 0x10u;
            } else if (length < 10u) {
                return TM_ENCODER_EINVAL;
            }
        } else if (memcmp(chunk, "XMP ", 4) == 0) {
            seen_xmp = 1u;
            return TM_ENCODER_EUNSUPPORTED;
        } else if (memcmp(chunk, "ANIM", 4) == 0 ||
                   memcmp(chunk, "ANMF", 4) == 0) {
            seen_animation = 1u;
            return TM_ENCODER_EUNSUPPORTED;
        } else if (seen_vp8x &&
                   (memcmp(chunk, "ICCP", 4) == 0 ||
                    memcmp(chunk, "ALPH", 4) == 0 ||
                    memcmp(chunk, "EXIF", 4) == 0)) {
            /* These chunks are preserved byte-for-byte. */
        } else {
            return TM_ENCODER_EUNSUPPORTED;
        }
        position += 8u + padded;
        ++chunk_count;
    }
    if (position != riff_size || image_count != 1u ||
        seen_xmp || seen_animation || (!simple && !seen_vp8x) ||
        (simple && chunk_count != 1u))
        return TM_ENCODER_EINVAL;

    /* Strictly preflight RIFF size, requested output size and the chunk's
     * even-byte pad before invoking a possibly side-effecting output sink.
     */
    addition = 8u + xmp_size + (xmp_size & 1u);
    if (simple) {
        if (addition > SIZE_MAX - sizeof(extended))
            return TM_ENCODER_ECAPACITY;
        addition += sizeof(extended);
    }
    if (addition > SIZE_MAX - riff_size ||
        riff_size + addition > (size_t)UINT32_MAX ||
        addition > max_output_bytes ||
        riff_size > max_output_bytes - addition)
        return TM_ENCODER_ECAPACITY;
    final_size = riff_size + addition;

    memcpy(header, riff, sizeof(header));
    tm_webp_put_le32(header + 4, (uint32_t)(final_size - 8u));
    tm_webp_put_le32(xmp_header + 4, (uint32_t)xmp_size);
    sink = (tm_webp_write_state){write, user, max_output_bytes, 0u};

    if ((status = tm_webp_emit(&sink, header, sizeof(header))) != TM_ENCODER_OK)
        return status;
    if (simple) {
        /* Inserting VP8X turns a simple lossy/lossless file into an
         * extended still image, retaining the original bitstream intact. */
        extended[8] = (uint8_t)(0x04u | alpha);
        tm_webp_put_le24(extended + 12, width - 1u);
        tm_webp_put_le24(extended + 15, height - 1u);
        if ((status = tm_webp_emit(&sink, extended, sizeof(extended))) !=
            TM_ENCODER_OK)
            return status;
        if ((status = tm_webp_emit(&sink, riff + source_first_chunk,
                                   riff_size - source_first_chunk)) !=
            TM_ENCODER_OK)
            return status;
    } else {
        uint8_t prefix[18];
        memcpy(prefix, riff + source_first_chunk, sizeof(prefix));
        prefix[8] |= 0x04u;
        if ((status = tm_webp_emit(&sink, prefix, sizeof(prefix))) !=
            TM_ENCODER_OK)
            return status;
        if ((status = tm_webp_emit(&sink,
                                   riff + source_first_chunk + sizeof(prefix),
                                   riff_size - source_first_chunk -
                                       sizeof(prefix))) != TM_ENCODER_OK)
            return status;
    }
    if ((status = tm_webp_emit(&sink, xmp_header, sizeof(xmp_header))) !=
            TM_ENCODER_OK ||
        (status = tm_webp_emit(&sink, xmp, xmp_size)) != TM_ENCODER_OK ||
        ((xmp_size & 1u) &&
         (status = tm_webp_emit(&sink, &padding, 1u)) != TM_ENCODER_OK))
        return status;
    if (sink.used != final_size)
        return TM_ENCODER_ECODEC;
    *written = sink.used;
    return TM_ENCODER_OK;
}
