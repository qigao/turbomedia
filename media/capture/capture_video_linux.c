/**
 * Linux Video Capture Implementation
 *
 * Factory-based approach:
 * - Wayland: Uses PipeWire for camera capture
 * - X11: Uses V4L2 for camera capture
 */
#include "turbo_capture.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>

#ifdef TURBO_HAS_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <stb_sprintf.h>
#endif

/* =============================================================================
 * Backend Type Enumeration
 * ============================================================================= */

typedef enum {
    VIDEO_BACKEND_V4L2,
    VIDEO_BACKEND_PIPEWIRE
} video_backend_type_t;

/* =============================================================================
 * Common Helpers
 * ============================================================================= */

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* =============================================================================
 * V4L2 Backend Implementation
 * ============================================================================= */

#define V4L2_BUFFER_COUNT   4
#define V4L2_POLL_TIMEOUT   1000  /* ms */

typedef struct {
    void *start;
    size_t length;
} v4l2_buffer_t;

typedef struct {
    int fd;
    v4l2_buffer_t *buffers;
    int buffer_count;

    pthread_t capture_thread;
    volatile int running;

    int width;
    int height;
    int framerate;
    int format;  /* turbo format: 0=I420, 1=NV12, 2=RGB24, 3=BGRA */
    uint32_t v4l2_pixfmt;  /* V4L2 pixel format */

    /* Conversion buffer if needed */
    uint8_t *convert_buf;
    size_t convert_buf_size;

    /* Parent capture */
    turbo_capture_t *capture;
} v4l2_ctx_t;

static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static uint32_t turbo_format_to_v4l2(int format) {
    switch (format) {
        case 0:  return V4L2_PIX_FMT_YUV420;   /* I420 */
        case 1:  return V4L2_PIX_FMT_NV12;     /* NV12 */
        case 2:  return V4L2_PIX_FMT_RGB24;    /* RGB24 */
        case 3:  return V4L2_PIX_FMT_BGR32;    /* BGRA */
        default: return V4L2_PIX_FMT_YUYV;     /* Default to YUYV */
    }
}

/* Simple YUYV to I420 conversion */
static void yuyv_to_i420(const uint8_t *src, uint8_t *dst,
                         int width, int height) {
    int frame_size = width * height;
    uint8_t *y = dst;
    uint8_t *u = dst + frame_size;
    uint8_t *v = u + frame_size / 4;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i += 2) {
            int idx = (j * width + i) * 2;
            y[j * width + i] = src[idx];
            y[j * width + i + 1] = src[idx + 2];

            if (j % 2 == 0) {
                int uv_idx = (j / 2) * (width / 2) + (i / 2);
                u[uv_idx] = src[idx + 1];
                v[uv_idx] = src[idx + 3];
            }
        }
    }
}

static int v4l2_init_mmap(v4l2_ctx_t *ctx) {
    struct v4l2_requestbuffers req = {0};
    req.count = V4L2_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) == -1) {
        return -1;
    }

    if (req.count < 2) {
        return -1;
    }

    ctx->buffers = calloc(req.count, sizeof(v4l2_buffer_t));
    if (!ctx->buffers) {
        return -1;
    }
    ctx->buffer_count = req.count;

    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            return -1;
        }

        ctx->buffers[i].length = buf.length;
        ctx->buffers[i].start = mmap(NULL, buf.length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, ctx->fd, buf.m.offset);

        if (ctx->buffers[i].start == MAP_FAILED) {
            return -1;
        }
    }

    return 0;
}

static void v4l2_cleanup_mmap(v4l2_ctx_t *ctx) {
    if (ctx->buffers) {
        for (int i = 0; i < ctx->buffer_count; i++) {
            if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
                munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            }
        }
        free(ctx->buffers);
        ctx->buffers = NULL;
    }
}

static void *v4l2_capture_thread(void *arg) {
    v4l2_ctx_t *ctx = (v4l2_ctx_t *)arg;
    turbo_capture_t *capture = ctx->capture;

    struct pollfd pfd = {
        .fd = ctx->fd,
        .events = POLLIN
    };

    while (ctx->running) {
        int ret = poll(&pfd, 1, V4L2_POLL_TIMEOUT);
        if (ret <= 0) {
            if (ret == 0) continue;  /* Timeout */
            if (errno == EINTR) continue;
            break;  /* Error */
        }

        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) continue;
            break;
        }

        /* Get frame data */
        uint8_t *frame_data = ctx->buffers[buf.index].start;
        size_t frame_len = buf.bytesused;

        /* Convert YUYV to I420 if needed */
        if (ctx->v4l2_pixfmt == V4L2_PIX_FMT_YUYV && ctx->format == 0) {
            if (ctx->convert_buf) {
                yuyv_to_i420(frame_data, ctx->convert_buf,
                             ctx->width, ctx->height);
                frame_data = ctx->convert_buf;
                frame_len = ctx->convert_buf_size;
            }
        }

        uint64_t timestamp = get_timestamp_us();

        /* Deliver frame */
        if (capture->video_cb) {
            capture->video_cb(capture, frame_data, frame_len,
                              ctx->width, ctx->height,
                              timestamp, capture->user_data);
        }

        /* Re-queue buffer */
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1) {
            break;
        }
    }

    return NULL;
}

static int v4l2_backend_init(turbo_capture_t *capture, const char *device_id,
                             const turbo_video_capture_config_t *config) {
    const char *device = device_id ? device_id : "/dev/video0";

    int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    /* Check capabilities */
    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        close(fd);
        return -1;
    }

    if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        return -1;
    }

    if (!(cap.device_caps & V4L2_CAP_STREAMING)) {
        close(fd);
        return -1;
    }

    v4l2_ctx_t *ctx = calloc(1, sizeof(v4l2_ctx_t));
    if (!ctx) {
        close(fd);
        return -1;
    }

    capture->platform_ctx = ctx;
    ctx->capture = capture;
    ctx->fd = fd;
    ctx->width = config ? config->width : 640;
    ctx->height = config ? config->height : 480;
    ctx->framerate = config ? config->framerate : 30;
    ctx->format = config ? config->format : 0;  /* I420 */

    /* Set format */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = turbo_format_to_v4l2(ctx->format);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        /* Try YUYV as fallback (most common) */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
            goto error;
        }
    }

    ctx->v4l2_pixfmt = fmt.fmt.pix.pixelformat;
    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;

    /* Allocate conversion buffer if YUYV and we want I420 */
    if (ctx->v4l2_pixfmt == V4L2_PIX_FMT_YUYV && ctx->format == 0) {
        ctx->convert_buf_size = ctx->width * ctx->height * 3 / 2;  /* I420 size */
        ctx->convert_buf = malloc(ctx->convert_buf_size);
        if (!ctx->convert_buf) {
            goto error;
        }
    }

    /* Set framerate */
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = ctx->framerate;
    xioctl(fd, VIDIOC_S_PARM, &parm);  /* Ignore errors - not all drivers support */

    /* Initialize mmap buffers */
    if (v4l2_init_mmap(ctx) != 0) {
        goto error;
    }

    return 0;

error:
    if (ctx->convert_buf) free(ctx->convert_buf);
    free(ctx);
    close(fd);
    capture->platform_ctx = NULL;
    return -1;
}

static int v4l2_backend_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;
    v4l2_ctx_t *ctx = (v4l2_ctx_t *)capture->platform_ctx;

    /* Queue all buffers */
    for (int i = 0; i < ctx->buffer_count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1) {
            return -1;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) == -1) {
        return -1;
    }

    ctx->running = 1;

    if (pthread_create(&ctx->capture_thread, NULL,
                       v4l2_capture_thread, ctx) != 0) {
        ctx->running = 0;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        return -1;
    }

    return 0;
}

static void v4l2_backend_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;
    v4l2_ctx_t *ctx = (v4l2_ctx_t *)capture->platform_ctx;

    if (ctx->running) {
        ctx->running = 0;
        pthread_join(ctx->capture_thread, NULL);

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }
}

static void v4l2_backend_destroy(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;

    v4l2_backend_stop(capture);

    v4l2_ctx_t *ctx = (v4l2_ctx_t *)capture->platform_ctx;
    v4l2_cleanup_mmap(ctx);
    if (ctx->convert_buf) free(ctx->convert_buf);
    if (ctx->fd >= 0) close(ctx->fd);
    free(ctx);
    capture->platform_ctx = NULL;
}

/* =============================================================================
 * PipeWire Backend Implementation
 * ============================================================================= */

#ifdef TURBO_HAS_PIPEWIRE

typedef struct {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    pthread_t capture_thread;
    volatile int running;

    int width;
    int height;
    int framerate;

    uint8_t *frame_buffer;
    size_t frame_buffer_size;

    /* Parent capture */
    turbo_capture_t *capture;
} pipewire_ctx_t;

static void pw_on_param_changed(void *userdata, uint32_t id,
                                 const struct spa_pod *param) {
    pipewire_ctx_t *ctx = (pipewire_ctx_t *)userdata;

    if (param == NULL || id != SPA_PARAM_Format) return;

    struct spa_video_info format;
    if (spa_format_video_raw_parse(param, &format.info.raw) < 0) return;

    ctx->width = format.info.raw.size.width;
    ctx->height = format.info.raw.size.height;

    /* Allocate frame buffer */
    ctx->frame_buffer_size = ctx->width * ctx->height * 4;  /* BGRA */
    ctx->frame_buffer = realloc(ctx->frame_buffer, ctx->frame_buffer_size);
}

static void pw_on_process(void *userdata) {
    pipewire_ctx_t *ctx = (pipewire_ctx_t *)userdata;
    turbo_capture_t *capture = ctx->capture;
    struct pw_buffer *b;

    if ((b = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
        return;
    }

    struct spa_buffer *buf = b->buffer;
    if (buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(ctx->stream, b);
        return;
    }

    uint8_t *src = buf->datas[0].data;
    size_t len = buf->datas[0].chunk->size;
    uint64_t timestamp = get_timestamp_us();

    /* Copy to frame buffer (handle stride if needed) */
    uint32_t stride = buf->datas[0].chunk->stride;
    uint32_t expected_stride = ctx->width * 4;

    if (stride == expected_stride || stride == 0) {
        /* Direct copy */
        if (capture->video_cb) {
            capture->video_cb(capture, src, len,
                              ctx->width, ctx->height,
                              timestamp, capture->user_data);
        }
    } else {
        /* Handle stride mismatch */
        if (ctx->frame_buffer) {
            for (int y = 0; y < ctx->height; y++) {
                memcpy(ctx->frame_buffer + y * expected_stride,
                       src + y * stride,
                       expected_stride);
            }
            if (capture->video_cb) {
                capture->video_cb(capture, ctx->frame_buffer,
                                  ctx->frame_buffer_size,
                                  ctx->width, ctx->height,
                                  timestamp, capture->user_data);
            }
        }
    }

    pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events pw_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_on_param_changed,
    .process = pw_on_process,
};

static void *pipewire_capture_thread(void *arg) {
    pipewire_ctx_t *ctx = (pipewire_ctx_t *)arg;
    pw_main_loop_run(ctx->loop);
    return NULL;
}

static int pipewire_backend_init(turbo_capture_t *capture, const char *device_id,
                                  const turbo_video_capture_config_t *config) {
    pw_init(NULL, NULL);

    pipewire_ctx_t *ctx = calloc(1, sizeof(pipewire_ctx_t));
    if (!ctx) {
        return -1;
    }

    capture->platform_ctx = ctx;
    ctx->capture = capture;
    ctx->width = config ? config->width : 640;
    ctx->height = config ? config->height : 480;
    ctx->framerate = config ? config->framerate : 30;

    ctx->loop = pw_main_loop_new(NULL);
    if (!ctx->loop) {
        free(ctx);
        capture->platform_ctx = NULL;
        return -1;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Camera",
        NULL);

    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "turbo-video-capture",
        props,
        &pw_stream_events,
        ctx);

    if (!ctx->stream) {
        pw_main_loop_destroy(ctx->loop);
        free(ctx);
        capture->platform_ctx = NULL;
        return -1;
    }

    /* Request BGRA format */
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(ctx->width, ctx->height),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(4096, 4096)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(ctx->framerate, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(60, 1)));

    int res = pw_stream_connect(ctx->stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);

    if (res < 0) {
        pw_stream_destroy(ctx->stream);
        pw_main_loop_destroy(ctx->loop);
        free(ctx);
        capture->platform_ctx = NULL;
        return -1;
    }

    return 0;
}

static int pipewire_backend_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;
    pipewire_ctx_t *ctx = (pipewire_ctx_t *)capture->platform_ctx;

    ctx->running = 1;
    if (pthread_create(&ctx->capture_thread, NULL,
                       pipewire_capture_thread, ctx) != 0) {
        ctx->running = 0;
        return -1;
    }
    return 0;
}

static void pipewire_backend_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;
    pipewire_ctx_t *ctx = (pipewire_ctx_t *)capture->platform_ctx;

    if (ctx->running) {
        ctx->running = 0;
        if (ctx->loop) {
            pw_main_loop_quit(ctx->loop);
        }
        pthread_join(ctx->capture_thread, NULL);
    }
}

static void pipewire_backend_destroy(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;

    pipewire_backend_stop(capture);

    pipewire_ctx_t *ctx = (pipewire_ctx_t *)capture->platform_ctx;
    if (ctx->stream) {
        pw_stream_destroy(ctx->stream);
    }
    if (ctx->loop) {
        pw_main_loop_destroy(ctx->loop);
    }
    if (ctx->frame_buffer) {
        free(ctx->frame_buffer);
    }
    pw_deinit();
    free(ctx);
    capture->platform_ctx = NULL;
}

#endif /* TURBO_HAS_PIPEWIRE */

/* =============================================================================
 * Device Enumeration (V4L2-based for compatibility)
 * ============================================================================= */

int turbo_capture_list_video_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    int count = 0;
    char path[64];

    for (int i = 0; i < 64 && count < max_count; i++) {
        snprintf(path, sizeof(path), "/dev/video%d", i);

        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            /* Check if it's a video capture device */
            if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                turbo_capture_device_t *dev = &devices[count];
                memset(dev, 0, sizeof(*dev));

                dev->index = count;
                dev->type = TURBO_CAPTURE_TYPE_VIDEO;
                dev->is_default = (count == 0) ? 1 : 0;

                snprintf(dev->name, sizeof(dev->name), "%s", cap.card);
                snprintf(dev->id, sizeof(dev->id), "/dev/video%d", i);

                count++;
            }
        }

        close(fd);
    }

    return count;
}

int turbo_capture_list_video_modes(const char *device_id,
                                   turbo_video_capture_mode_t *modes,
                                   int max_count) {
    (void)device_id;
    (void)modes;
    (void)max_count;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

/* =============================================================================
 * Factory Method - Backend Selection
 * ============================================================================= */

turbo_capture_t *turbo_video_capture_create(const char *device_id,
                                             const turbo_video_capture_config_t *config) {
    turbo_capture_t *capture = calloc(1, sizeof(turbo_capture_t));
    if (!capture) return NULL;

    capture->type = TURBO_CAPTURE_TYPE_VIDEO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;

    /* Determine backend based on display server */
    video_backend_type_t backend = VIDEO_BACKEND_V4L2;

#ifdef TURBO_HAS_PIPEWIRE
    if (getenv("WAYLAND_DISPLAY")) {
        backend = VIDEO_BACKEND_PIPEWIRE;
    }
#endif

    int result = -1;

    /* Initialize selected backend */
    switch (backend) {
#ifdef TURBO_HAS_PIPEWIRE
        case VIDEO_BACKEND_PIPEWIRE:
            result = pipewire_backend_init(capture, device_id, config);
            break;
#endif
        case VIDEO_BACKEND_V4L2:
        default:
            result = v4l2_backend_init(capture, device_id, config);
            break;
    }

    if (result != 0) {
        free(capture);
        return NULL;
    }

    return capture;
}

void turbo_video_capture_set_callback(turbo_capture_t *capture,
                                       turbo_video_capture_cb cb,
                                       void *user_data) {
    if (!capture) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

int turbo_video_capture_get_control_range(turbo_capture_t *capture,
                                           turbo_camera_control_t control,
                                           turbo_camera_control_range_t *range) {
    (void)capture;
    (void)control;
    if (!range) return TURBO_CAPTURE_ERR_DEVICE;
    memset(range, 0, sizeof(*range));
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_set_control(turbo_capture_t *capture,
                                     turbo_camera_control_t control,
                                     int value) {
    (void)capture;
    (void)control;
    (void)value;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_get_control(turbo_capture_t *capture,
                                     turbo_camera_control_t control,
                                     int *value) {
    (void)capture;
    (void)control;
    if (value) *value = 0;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_set_crop(turbo_capture_t *capture,
                                  const turbo_video_crop_t *crop) {
    (void)capture;
    (void)crop;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_get_crop(turbo_capture_t *capture,
                                  turbo_video_crop_t *crop) {
    (void)capture;
    if (!crop) return TURBO_CAPTURE_ERR_DEVICE;
    memset(crop, 0, sizeof(*crop));
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

/* =============================================================================
 * Platform Hooks - Dispatcher
 * ============================================================================= */

int v4l2_video_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;

    /* Detect backend type by checking context structure */
    v4l2_ctx_t *v4l2_ctx = (v4l2_ctx_t *)capture->platform_ctx;
    
    /* Simple heuristic: V4L2 context has an fd field */
    if (v4l2_ctx->fd >= 0) {
        return v4l2_backend_start(capture);
    }

#ifdef TURBO_HAS_PIPEWIRE
    return pipewire_backend_start(capture);
#else
    return -1;
#endif
}

void v4l2_video_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;

    v4l2_ctx_t *v4l2_ctx = (v4l2_ctx_t *)capture->platform_ctx;
    
    if (v4l2_ctx->fd >= 0) {
        v4l2_backend_stop(capture);
        return;
    }

#ifdef TURBO_HAS_PIPEWIRE
    pipewire_backend_stop(capture);
#endif
}

void v4l2_video_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    if (!capture->platform_ctx) {
        free(capture);
        return;
    }

    v4l2_ctx_t *v4l2_ctx = (v4l2_ctx_t *)capture->platform_ctx;
    
    if (v4l2_ctx->fd >= 0) {
        v4l2_backend_destroy(capture);
    }
#ifdef TURBO_HAS_PIPEWIRE
    else {
        pipewire_backend_destroy(capture);
    }
#endif

    free(capture);
}

#endif /* __linux__ && !__ANDROID__ */
