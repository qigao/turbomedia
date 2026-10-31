#include "ebml.h"
#include <assert.h>

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

static uint64_t ebml_read_uint64(struct ebml_t* ebml, int max)
{
    size_t n;
    uint64_t v;

    if (ebml->err || ebml->off >= ebml->len)
    {
        ebml->err = 1;
        return 0;
    }

    n = 8 - s_ebml_log2_tab[ebml->ptr[ebml->off]];
    if (n > (size_t)max || n + ebml->off > ebml->len)
    {
        ebml->err = 1;
        return 0;
    }

    v = ebml->ptr[ebml->off] & ~(1 << s_ebml_log2_tab[ebml->ptr[ebml->off]]);
    for (++ebml->off; n-- > 1; ebml->off++)
        v = (v << 8) | ebml->ptr[ebml->off];

    return v;
}

/* Zero means the value cannot be encoded as a KNOWN EBML size.
 * Each width reserves its all-ones VINT_DATA for unknown-size Masters.
 * Explicit-width callers may still encode that sentinel deliberately. */
unsigned int ebml_size_length(uint64_t size)
{
    unsigned int bytes;
    for (bytes = 1u; bytes <= 8u; ++bytes) {
        uint64_t unknown = (UINT64_C(1) << (7u * bytes)) - 1u;
        if (size < unknown)
            return bytes;
    }
    return 0u;
}

unsigned int ebml_uint_length(uint64_t v)
{
    unsigned int bytes = 1u;
    /* Never right-shift a 64-bit value by 64. */
    while (bytes < 8u && (v >> (8u * bytes)) != 0u)
        ++bytes;
    return bytes;
}

unsigned int ebml_write_uint(uint8_t buf[8], uint64_t v)
{
    unsigned int i, bytes;
    if (!buf) return 0u;
    bytes = ebml_uint_length(v);
    for (i = 0u; i < bytes; ++i)
        buf[i] = (uint8_t)(v >> (8u * (bytes - 1u - i)));
    return bytes;
}

unsigned int ebml_write_element(uint8_t buf[12], uint32_t id, uint64_t size, unsigned int bytes)
{
    unsigned int id_bytes, i, width;
    uint64_t maximum;

    if (!buf || !id) return 0u;
    id_bytes = ebml_uint_length(id);
    if (!id_bytes || id_bytes > 4u) return 0u;
    width = bytes ? bytes : ebml_size_length(size);
    if (!width || width > 8u) return 0u;
    maximum = (UINT64_C(1) << (7u * width)) - 1u;
    /* Reject an unrepresentable size BEFORE touching the output.
     * The all-ones unknown-size code is allowed only with explicit width. */
    if (size > maximum || (!bytes && size == maximum)) return 0u;

    for (i = 0u; i < id_bytes; ++i)
        buf[i] = (uint8_t)(id >> (8u * (id_bytes - 1u - i)));
    size |= UINT64_C(1) << (7u * width);
    for (i = 0u; i < width; ++i)
        buf[id_bytes + i] =
            (uint8_t)(size >> (8u * (width - 1u - i)));
    return id_bytes + width;
}
