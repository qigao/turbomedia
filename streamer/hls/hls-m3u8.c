#include "hls-m3u8.h"
#include "hls-param.h"
#include "turbo_vec.h"
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define VMAX(a, b) ((a) > (b) ? (a) : (b))

struct hls_m3u8_t
{
	int live;
	int version;
	int64_t seq; // m3u8 sequence number (base 0)
	int64_t duration;// target duration

	turbo_vec_t segments;

	char* ext_x_map;
	char* playlist_type;
};

struct hls_segment_t
{
	int64_t pts;		// present timestamp (millisecond)
	int64_t duration;	// segment duration (millisecond)
	int64_t offset;		// EXT-X-BYTERANGE offset: a byte offset from the beginning of the resource
	int64_t bytes;		// EXT-X-BYTERANGE length
	int discontinuity;	// EXT-X-DISCONTINUITY flag

	char* name;
	size_t capacity;
};

struct hls_m3u8_t* hls_m3u8_create(int live, int version)
{
	struct hls_m3u8_t* m3u8;
	m3u8 = (struct hls_m3u8_t*)calloc(1, sizeof(*m3u8));
	if (NULL == m3u8)
		return NULL;

	assert(0 == live || live >= HLS_LIVE_NUM);
	m3u8->version = version;
	m3u8->live = live;
	m3u8->seq = 0;
	m3u8->duration = 0;
	if (TURBO_OK != turbo_vec_init(&m3u8->segments, sizeof(struct hls_segment_t*)))
	{
		free(m3u8);
		return NULL;
	}
	if (live > 0 && TURBO_OK != turbo_vec_reserve(&m3u8->segments, (size_t)live))
	{
		turbo_vec_destroy(&m3u8->segments);
		free(m3u8);
		return NULL;
	}
	return m3u8;
}

void hls_m3u8_destroy(struct hls_m3u8_t* m3u8)
{
	size_t index;
	struct hls_segment_t* seg;
	for (index = 0; index < turbo_vec_size(&m3u8->segments); ++index)
	{
		seg = *(struct hls_segment_t**)turbo_vec_at(&m3u8->segments, index);
		free(seg);
	}
	turbo_vec_destroy(&m3u8->segments);

	if (m3u8->ext_x_map)
		free(m3u8->ext_x_map);
	if (m3u8->playlist_type)
		free(m3u8->playlist_type);
	free(m3u8);
}

static struct hls_segment_t* hls_segment_alloc(size_t bytes)
{
	struct hls_segment_t* seg;
	seg = (struct hls_segment_t*)malloc(sizeof(*seg) + bytes);
	if (seg)
	{
		seg->name = (char*)(seg + 1);
		seg->capacity = bytes;
		seg->offset = 0;
		seg->bytes = 0;
		seg->pts = 0;
		seg->discontinuity = 0;
	}
	return seg;
}

int hls_m3u8_add(struct hls_m3u8_t* m3u8, const char* name, int64_t pts, int64_t duration, int discontinuity)
{
	return hls_m3u8_add_with_offset(m3u8, name, pts, duration, discontinuity, 0, 0);
}

int hls_m3u8_add_with_offset(hls_m3u8_t* m3u8, const char* name, int64_t pts, int64_t duration, int discontinuity, int64_t offset, int64_t bytes)
{
	size_t r;
	size_t count;
	struct hls_segment_t* seg;
	seg = NULL;
	r = strlen(name);
	count = turbo_vec_size(&m3u8->segments);

	if (0 != m3u8->live && count >= (size_t)m3u8->live)
	{
		assert(count == (size_t)m3u8->live);

		++m3u8->seq; // update EXT-X-MEDIA-SEQUENCE

		// reuse the first segment
		if (TURBO_OK != turbo_vec_erase(&m3u8->segments, 0U, &seg))
			return -EINVAL;

		// check name length
		if (r + 1 > seg->capacity)
		{
			free(seg);
			seg = NULL;
		}
	}

	if (NULL == seg)
	{
		// reserve more space for reuse segment
		seg = hls_segment_alloc(r + (m3u8->live ? 16 : 1));
		if (!seg)
			return -ENOMEM;

	}

	// update EXT-X-TARGETDURATION
	m3u8->duration = VMAX(m3u8->duration, duration);

	// segment
	seg->pts = pts;
	seg->bytes = bytes;
	seg->offset = offset;
	seg->duration = duration;
	seg->discontinuity = discontinuity; // EXT-X-DISCONTINUITY
	memcpy(seg->name, name, r + 1); // copy last '\0'

	if (TURBO_OK != turbo_vec_push(&m3u8->segments, &seg))
	{
		free(seg);
		return -ENOMEM;
	}
	return 0;
}

int hls_m3u8_set_x_map(hls_m3u8_t* m3u8, const char* name)
{
	if (m3u8->ext_x_map)
		free(m3u8->ext_x_map);
	m3u8->ext_x_map = name ? strdup(name) : NULL;
	return name && !m3u8->ext_x_map ? -ENOMEM : 0;
}

int hls_m3u8_set_playlist_type(hls_m3u8_t* m3u8, const char* type)
{
	char* value;
	if (!m3u8 || (type && 0 != strcmp(type, "VOD") && 0 != strcmp(type, "EVENT")))
		return -EINVAL;
	value = type ? strdup(type) : NULL;
	if (type && !value)
		return -ENOMEM;
	free(m3u8->playlist_type);
	m3u8->playlist_type = value;
	return 0;
}

size_t hls_m3u8_count(struct hls_m3u8_t* m3u8)
{
	return turbo_vec_size(&m3u8->segments);
}

int hls_m3u8_playlist(struct hls_m3u8_t* m3u8, int eof, char* playlist, size_t bytes)
{
	int r;
	size_t n, index;
	struct hls_segment_t* seg;

	r = snprintf(playlist, bytes,
		"#EXTM3U\n" // MUST
		"#EXT-X-VERSION:%d\n" // Optional
		"#EXT-X-TARGETDURATION:%" PRId64 "\n" // MUST, decimal-integer, in seconds
		"#EXT-X-MEDIA-SEQUENCE:%" PRId64 "\n"
		"%s%s%s"  // #EXT-X-PLAYLIST-TYPE
		"%s", // #EXT-X-ALLOW-CACHE:YES
		m3u8->version,
		(m3u8->duration + 999) / 1000,
		m3u8->seq,
		m3u8->playlist_type ? "#EXT-X-PLAYLIST-TYPE:" : "",
		m3u8->playlist_type ? m3u8->playlist_type : "",
		m3u8->playlist_type ? "\n" : "",
		m3u8->live ? "" : "#EXT-X-ALLOW-CACHE:YES\n");
	if (r <= 0 || (size_t)r >= bytes)
		return -ENOMEM;

	// #EXT-X-MAP:URI="main.mp4",BYTERANGE="1206@0"
	if (m3u8->ext_x_map)
		r += snprintf(playlist + r, r < bytes ? bytes - r : 0, "#EXT-X-MAP:URI=\"%s\",\n", m3u8->ext_x_map);

	n = r;
	for (index = 0; index < turbo_vec_size(&m3u8->segments); ++index)
	{
		if (bytes <= n)
			break;

		seg = *(struct hls_segment_t**)turbo_vec_at(&m3u8->segments, index);

		if (seg->discontinuity)
			n += snprintf(playlist + n, n < bytes ? bytes - n : 0, "#EXT-X-DISCONTINUITY\n");
		if (bytes > n)
		{
			if(seg->bytes > 0)
				n += snprintf(playlist + n, n < bytes ? bytes - n : 0, "#EXTINF:%.3f,\n#EXT-X-BYTERANGE:%" PRId64 "@%" PRId64 "\n%s\n", seg->duration / 1000.0, seg->bytes, seg->offset, seg->name);
			else
				n += snprintf(playlist + n, n < bytes ? bytes - n : 0, "#EXTINF:%.3f,\n%s\n", seg->duration / 1000.0, seg->name);
		}
			
	}

	if (eof && bytes > n + 15)
		n += snprintf(playlist + n, n < bytes ? bytes - n : 0, "#EXT-X-ENDLIST\n");

	return (bytes > n && n > 0) ? 0 : -ENOMEM;
}
