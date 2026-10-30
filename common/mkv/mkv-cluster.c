#include "mkv-internal.h"
#include "mkv-buffer.h"
#include <limits.h>

enum
{
	MKV_LACING_NONE = 0,
	MKV_LACING_XIPH = 1,
	MKV_LACING_EBML = 3,
	MKV_LACING_FIXED_SIZE = 2,
};

static int mkv_alloc_samples(struct mkv_t* mkv, int n)
{
    void* p;
    size_t required;
    if (!mkv || n <= 0 || mkv->count < 0 || mkv->capacity < 0 ||
        mkv->count >= INT_MAX - n)
        return -EINVAL;
    required = (size_t)mkv->count + (size_t)n + 1u;
    if (required > SIZE_MAX / sizeof(struct mkv_sample_t))
        return -ENOMEM;
    if ((size_t)mkv->count + (size_t)n > (size_t)mkv->capacity) {
        p = realloc(mkv->samples, required * sizeof(struct mkv_sample_t));
        if (!p) return -ENOMEM;
        mkv->samples = (struct mkv_sample_t*)p;
        mkv->capacity = (int)required;
    }
    /* Every new sample must start with zero trim/flags even if the
     * allocation was recycled from an earlier cluster. */
    memset(mkv->samples + mkv->count, 0,
           (size_t)n * sizeof(struct mkv_sample_t));
    return 0;
}

static int mkv_add_rap(struct mkv_t* mkv, int i)
{
	void* p;
	if (mkv->rap.count >= mkv->rap.capacity)
	{
		p = realloc(mkv->rap.raps, sizeof(mkv->rap.raps[0]) * (mkv->rap.capacity * 4 / 3 + 16));
		if (!p) return -ENOMEM;
		mkv->rap.raps = (int*)p;
		mkv->rap.capacity = mkv->rap.capacity * 4 / 3 + 16;
	}

	mkv->rap.raps[mkv->rap.count++] = i;
	return 0;
}

int mkv_cluster_simple_block_read(struct mkv_t* mkv,
    struct mkv_cluster_t* cluster, struct mkv_ioutil_t* io, int64_t bytes)
{
    uint32_t lengths[256] = {0};
    uint64_t start, payload, used, remaining, sum = 0, duration;
    int64_t track_id, previous = 0, timestamp_ticks;
    int16_t relative_timestamp;
    uint8_t flags, lace_count = 0;
    unsigned frame_count = 1u, i;
    unsigned lacing;
    int r, first_index, track_number;
    int f = 0;
    struct mkv_track_t* track;

    if (!mkv || !cluster || !io || !mkv->timescale ||
        bytes < 4 || bytes > (int64_t)UINT32_MAX)
        return -EINVAL;
    start = mkv_buffer_tell(io);
    if (mkv_buffer_error(io) != 0) return -EINVAL;
    track_id = mkv_buffer_read_size(io);
    relative_timestamp = (int16_t)mkv_buffer_read_int(io, 2);
    flags = (uint8_t)mkv_buffer_read_uint(io, 1);
    payload = mkv_buffer_tell(io);
    if (mkv_buffer_error(io) != 0 || payload < start ||
        payload - start > (uint64_t)bytes ||
        track_id < 1 || track_id > INT_MAX)
        return -EINVAL;

    track_number = (int)track_id;
    track = mkv_track_find(mkv, (unsigned)track_number);
    if (!track) return -EINVAL;
    if (cluster->timestamp > INT64_MAX) return -ERANGE;
    timestamp_ticks = (int64_t)cluster->timestamp + relative_timestamp;
    if (timestamp_ticks < 0) return -EINVAL;
    duration = track->duration / mkv->timescale;
    lacing = (flags >> 1) & 0x03u;

    if (lacing != MKV_LACING_NONE) {
        if (payload - start >= (uint64_t)bytes)
            return -EINVAL; /* missing lace-count byte */
        lace_count = mkv_buffer_r8(io);
        if (mkv_buffer_error(io) != 0) return -EINVAL;
        frame_count = (unsigned)lace_count + 1u; /* 1..256, never uint8 */
    }

    if (lacing == MKV_LACING_XIPH) {
        for (i = 0; i < (unsigned)lace_count; ++i) {
            uint32_t frame_length = 0;
            uint8_t chunk;
            do {
                uint64_t at = mkv_buffer_tell(io);
                if (mkv_buffer_error(io) != 0 || at < start ||
                    at - start >= (uint64_t)bytes)
                    return -EINVAL;
                chunk = mkv_buffer_r8(io);
                if (mkv_buffer_error(io) != 0 ||
                    frame_length > UINT32_MAX - chunk)
                    return -EINVAL;
                frame_length += chunk;
            } while (chunk == 255u);
            if (!frame_length || sum > UINT32_MAX - frame_length)
                return -EINVAL;
            lengths[i] = frame_length;
            sum += frame_length;
        }
    } else if (lacing == MKV_LACING_EBML) {
        for (i = 0; i < (unsigned)lace_count; ++i) {
            uint64_t at = mkv_buffer_tell(io);
            int64_t delta;
            if (mkv_buffer_error(io) != 0 || at < start ||
                at - start >= (uint64_t)bytes)
                return -EINVAL;
            delta = i == 0 ? mkv_buffer_read_size(io) :
                             mkv_buffer_read_signed_size(io);
            if (mkv_buffer_error(io) != 0 ||
                delta <= -previous ||
                delta > (int64_t)UINT32_MAX - previous)
                return -EINVAL;
            previous += delta;
            if (sum > UINT32_MAX - (uint64_t)previous)
                return -EINVAL;
            lengths[i] = (uint32_t)previous;
            sum += (uint64_t)previous;
        }
    }

    payload = mkv_buffer_tell(io);
    if (mkv_buffer_error(io) != 0 || payload < start ||
        payload - start > (uint64_t)bytes)
        return -EINVAL;
    used = payload - start;
    remaining = (uint64_t)bytes - used;
    if (lacing == MKV_LACING_FIXED_SIZE) {
        if (frame_count == 0u || !remaining ||
            remaining % frame_count != 0u)
            return -EINVAL;
        for (i = 0; i < frame_count; ++i)
            lengths[i] = (uint32_t)(remaining / frame_count);
    } else {
        if (sum >= remaining) return -EINVAL;
        lengths[frame_count - 1u] = (uint32_t)(remaining - sum);
    }
    for (i = 0; i < frame_count; ++i)
        if (lengths[i] == 0u) return -EINVAL;
    if (duration > 0 &&
        (uint64_t)(frame_count - 1u) >
            ((uint64_t)INT64_MAX - (uint64_t)timestamp_ticks) / duration)
        return -ERANGE;

    if (flags & 0x80u) f |= MKV_FLAGS_KEYFRAME;
    if (flags & 0x08u) f |= MKV_FLAGS_INVISIBLE;
    if (flags & 0x01u) f |= MKV_FLAGS_DISCARDABLE;

    r = mkv_alloc_samples(mkv, (int)frame_count);
    if (r != 0) return r;
    first_index = mkv->count;
    for (i = 0; i < frame_count; ++i) {
        struct mkv_sample_t* sample = &mkv->samples[first_index + (int)i];
        sample->flags = i == 0 ? f : 0;
        sample->track = track_number;
        sample->pts = timestamp_ticks + (int64_t)(i * duration);
        sample->dts = sample->pts;
        sample->bytes = lengths[i];
        sample->offset = payload;
        payload += lengths[i];
    }
    mkv->count += (int)frame_count;
    if (f & MKV_FLAGS_KEYFRAME) {
        r = mkv_add_rap(mkv, first_index);
        if (r != 0) {
            mkv->count = first_index;
            return r;
        }
    }
    return 0;
}

/* Block and SimpleBlock share a four-byte payload header, but BlockGroup
 * keyframe semantics use ReferenceBlock rather than SimpleBlock flag 0x80. */
static void mkv_write_block_payload(struct mkv_sample_t* sample,
                                    struct mkv_ioutil_t* io, int simple)
{
    uint8_t flags = 0;
    if (simple && (sample->flags & MKV_FLAGS_KEYFRAME)) flags |= 0x80;
    if (sample->flags & MKV_FLAGS_INVISIBLE) flags |= 0x08;
    if (sample->flags & MKV_FLAGS_DISCARDABLE) flags |= 0x01;
    mkv_buffer_w8(io, (uint8_t)(sample->track | 0x80));
    mkv_buffer_w16(io, (uint16_t)sample->pts);
    mkv_buffer_w8(io, flags); /* no lacing */
    mkv_buffer_write(io, sample->data, sample->bytes);
}

int mkv_cluster_simple_block_write(struct mkv_t* mkv,
    struct mkv_sample_t* sample, struct mkv_ioutil_t* io)
{
    (void)mkv;
    if (!sample || !io || sample->track < 1 || sample->track >= 8)
        return -EINVAL;
    mkv_buffer_write_master(io, 0xA3, UINT64_C(4) + sample->bytes, 0);
    mkv_write_block_payload(sample, io, 1);
    return mkv_buffer_error(io);
}

int mkv_cluster_block_group_write(struct mkv_sample_t* sample,
    struct mkv_ioutil_t* io, uint64_t tail_padding_ns)
{
    uint64_t block_bytes, group_bytes;
    uint8_t signed_bytes[8];
    unsigned n = 1, i;
    if (!sample || !io || sample->track < 1 || sample->track >= 8 ||
        tail_padding_ns == 0 || tail_padding_ns > INT64_MAX)
        return -EINVAL;
    /* Signed EBML integer: positive trims the END of the Block. */
    while (n < 8 && tail_padding_ns >= (UINT64_C(1) << (n * 8 - 1)))
        ++n;
    for (i = 0; i < n; ++i)
        signed_bytes[i] = (uint8_t)(tail_padding_ns >> ((n - i - 1) * 8));
    block_bytes = UINT64_C(4) + sample->bytes;
    group_bytes = 1 + ebml_size_length(block_bytes) + block_bytes +
                  2 + ebml_size_length(n) + n;
    mkv_buffer_write_master(io, 0xA0, group_bytes, 0); /* BlockGroup */
    mkv_buffer_write_master(io, 0xA1, block_bytes, 0); /* Block */
    mkv_write_block_payload(sample, io, 0);
    mkv_buffer_write_master(io, 0x75A2, n, 0); /* DiscardPadding */
    mkv_buffer_write(io, signed_bytes, n);
    return mkv_buffer_error(io);
}
