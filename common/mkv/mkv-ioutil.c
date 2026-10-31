#include "mkv-ioutil.h"
#include <errno.h>

static const uint8_t s_ebml_log2_tab[256] = {
    0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

/* Matroska constrains Element IDs to 1..4 octets, while EBML data
 * sizes and lacing deltas can use 1..8. A zero VINT marker is illegal;
 * reject it before attempting reads or shifting the decoded integer. */
static int mkv_read_vint_header(
    struct mkv_ioutil_t* io, uint8_t* first, size_t* width, size_t max_width)
{
    if (!io || !first || !width) return -EINVAL;
    if (io->error != 0) return io->error;
    mkv_buffer_read(io, first, 1);
    if (io->error != 0) return io->error;
    if (*first == 0u) {
        io->error = -EINVAL;
        return io->error;
    }
    *width = 8u - s_ebml_log2_tab[*first];
    if (*width < 1u || *width > max_width) {
        io->error = -EINVAL;
        return io->error;
    }
    return 0;
}

uint32_t mkv_buffer_read_id(struct mkv_ioutil_t* io)
{
    uint8_t first = 0, byte = 0;
    uint32_t value, data_mask, data;
    size_t width, i;
    if (mkv_read_vint_header(io, &first, &width, 4u) != 0)
        return 0;
    value = first;
    for (i = 1u; i < width; ++i) {
        mkv_buffer_read(io, &byte, 1);
        if (io->error != 0) return 0;
        value = (value << 8) | byte;
    }
    data_mask = (UINT32_C(1) << (7u * width)) - 1u;
    data = value & data_mask;
    /* Matroska RFC 9559 permits the one-byte ID 0x80; other Element
     * IDs may not have all-zero or all-one VINT_DATA (RFC 8794). */
    if ((data == 0u && !(width == 1u && value == 0x80u)) ||
        data == data_mask) {
        io->error = -EINVAL;
        return 0;
    }
    return value;
}

int64_t mkv_buffer_read_size(struct mkv_ioutil_t* io)
{
    uint8_t first = 0, byte = 0;
    uint64_t value, maximum;
    size_t width, i;
    if (mkv_read_vint_header(io, &first, &width, 8u) != 0)
        return 0;
    value = first & (UINT64_C(0xff) >> width);
    for (i = 1u; i < width; ++i) {
        mkv_buffer_read(io, &byte, 1);
        if (io->error != 0) return 0;
        value = (value << 8) | byte;
    }
    maximum = (UINT64_C(1) << (7u * width)) - 1u;
    if (value == maximum)
        return -1; /* unknown-size sentinel, only valid on Master */
    return (int64_t)value;
}

/* EBML lacing signed VINT uses the regular VINT payload minus a
 * width-dependent bias. Width <=8 keeps both shifts and signed
 * subtraction representable, even on malformed input. */
int64_t mkv_buffer_read_signed_size(struct mkv_ioutil_t* io)
{
    uint8_t first = 0, byte = 0;
    uint64_t value;
    int64_t bias;
    size_t width, i;
    if (mkv_read_vint_header(io, &first, &width, 8u) != 0)
        return 0;
    value = first & (UINT64_C(0xff) >> width);
    for (i = 1u; i < width; ++i) {
        mkv_buffer_read(io, &byte, 1);
        if (io->error != 0) return 0;
        value = (value << 8) | byte;
    }
    bias = (INT64_C(1) << (7u * width - 1u)) - 1;
    return (int64_t)value - bias;
}

/* EBML signed VINT (used for lacing deltas): encode biased signed
 * payload in network byte order. The former writer ORed the marker into a
 * negative signed integer, emitted least-significant bytes first, and could
 * overflow on INT64_MIN. It did not round-trip through the native reader. */
void mkv_buffer_write_signed_size(struct mkv_ioutil_t* io, int64_t value)
{
    uint8_t bytes[8];
    uint64_t encoded;
    int64_t bias = 0;
    unsigned int width, i;

    if (!io || io->error != 0)
        return;
    for (width = 1u; width <= 8u; ++width) {
        bias = (INT64_C(1) << (7u * width - 1u)) - 1;
        if (value >= -bias && value <= bias + 1)
            break;
    }
    if (width > 8u) {
        io->error = -ERANGE;
        return;
    }

    /* All intermediate arithmetic is signed-representable for width <= 8.
     * The leading marker bit is inserted into unsigned storage only. */
    encoded = (uint64_t)(value + bias) |
              (UINT64_C(1) << (7u * width));
    for (i = 0u; i < width; ++i)
        bytes[i] = (uint8_t)(encoded >> (8u * (width - 1u - i)));
    mkv_buffer_write(io, bytes, width);
}
