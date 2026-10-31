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

void mkv_buffer_write_signed_size(struct mkv_ioutil_t* io, int64_t size)
{
    uint64_t tmp;
    tmp = 2*(size < 0 ? size^-1 : size);
    size |= (1ULL << ebml_size_length(tmp) * 7);
    do
    {
        mkv_buffer_w8(io, (uint8_t)size);
        size >>= 8;
    } while(size > 0);
}
