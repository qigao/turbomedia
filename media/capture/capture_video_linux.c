/**
 * Linux Video Capture Implementation
 *
 * Exact native camera modes through V4L2.
 */
#include "turbo_capture.h"
#include "capture_video_backend.h"

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

/* =============================================================================
 * Backend Type Enumeration
 * ============================================================================= */

typedef struct {
    int fd;
    char path[256];
} linux_video_device_ctx_t;

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

static int v4l2_pixfmt_to_turbo_format(uint32_t pixfmt) {
    switch (pixfmt) {
        case V4L2_PIX_FMT_YUV420:
        case V4L2_PIX_FMT_YUYV:
            return TURBO_VIDEO_CAPTURE_FORMAT_I420;
        case V4L2_PIX_FMT_NV12:
            return TURBO_VIDEO_CAPTURE_FORMAT_NV12;
        case V4L2_PIX_FMT_RGB24:
            return TURBO_VIDEO_CAPTURE_FORMAT_RGB24;
        case V4L2_PIX_FMT_BGR32:
            return TURBO_VIDEO_CAPTURE_FORMAT_BGRA;
        case V4L2_PIX_FMT_MJPEG:
        case V4L2_PIX_FMT_JPEG:
            return TURBO_VIDEO_CAPTURE_FORMAT_MJPEG;
        default:
            return -1;
    }
}

static uint64_t v4l2_mode_id(uint32_t format_index,
                             uint32_t size_index,
                             uint32_t interval_index) {
    return ((uint64_t)format_index << 32) |
           ((uint64_t)size_index << 16) |
           interval_index;
}

static int v4l2_read_mode(int fd,
                          uint64_t mode_id,
                          turbo_video_native_mode_t *mode,
                          uint32_t *out_pixfmt) {
    uint32_t format_index = (uint32_t)(mode_id >> 32);
    uint32_t size_index = (uint32_t)((mode_id >> 16) & 0xffffu);
    uint32_t interval_index = (uint32_t)(mode_id & 0xffffu);
    struct v4l2_fmtdesc format_desc = {0};
    struct v4l2_frmsizeenum frame_size = {0};
    struct v4l2_frmivalenum frame_interval = {0};
    int format;

    if (!mode || format_index > 0xffffu) return TURBO_CAPTURE_ERR_FORMAT;

    format_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format_desc.index = format_index;
    if (xioctl(fd, VIDIOC_ENUM_FMT, &format_desc) == -1) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }
    format = v4l2_pixfmt_to_turbo_format(format_desc.pixelformat);
    if (format < 0) return TURBO_CAPTURE_ERR_UNSUPPORTED;

    frame_size.index = size_index;
    frame_size.pixel_format = format_desc.pixelformat;
    if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) == -1 ||
        frame_size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    frame_interval.index = interval_index;
    frame_interval.pixel_format = format_desc.pixelformat;
    frame_interval.width = frame_size.discrete.width;
    frame_interval.height = frame_size.discrete.height;
    if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frame_interval) == -1 ||
        frame_interval.type != V4L2_FRMIVAL_TYPE_DISCRETE ||
        frame_interval.discrete.numerator == 0 ||
        frame_interval.discrete.denominator == 0) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    mode->width = (int)frame_size.discrete.width;
    mode->height = (int)frame_size.discrete.height;
    mode->framerate_numerator = frame_interval.discrete.denominator;
    mode->framerate_denominator = frame_interval.discrete.numerator;
    mode->format = format;
    mode->mode_id = mode_id;
    if (out_pixfmt) *out_pixfmt = format_desc.pixelformat;
    return TURBO_CAPTURE_OK;
}

static int video_native_modes_equal(const turbo_video_native_mode_t *lhs,
                                    const turbo_video_native_mode_t *rhs) {
    return lhs->width == rhs->width && lhs->height == rhs->height &&
           lhs->framerate_numerator == rhs->framerate_numerator &&
           lhs->framerate_denominator == rhs->framerate_denominator &&
           lhs->format == rhs->format && lhs->mode_id == rhs->mode_id;
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
        if (buf.index >= (uint32_t)ctx->buffer_count ||
            buf.bytesused > ctx->buffers[buf.index].length) {
            break;
        }
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

static int v4l2_backend_init_exact(
    turbo_capture_t *capture,
    const char *device,
    const turbo_video_native_mode_t *mode) {
    turbo_video_native_mode_t actual_mode;
    uint32_t pixfmt = 0;

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

    uint32_t capabilities =
        (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? cap.device_caps
            : cap.capabilities;
    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        return -1;
    }

    if (!(capabilities & V4L2_CAP_STREAMING)) {
        close(fd);
        return -1;
    }

    if (v4l2_read_mode(fd, mode->mode_id, &actual_mode, &pixfmt) !=
            TURBO_CAPTURE_OK ||
        !video_native_modes_equal(&actual_mode, mode)) {
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
    ctx->width = mode->width;
    ctx->height = mode->height;
    ctx->framerate = (int)(((uint64_t)mode->framerate_numerator +
                            mode->framerate_denominator / 2u) /
                           mode->framerate_denominator);
    ctx->format = mode->format;

    /* Set format */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1 ||
        fmt.fmt.pix.pixelformat != pixfmt ||
        fmt.fmt.pix.width != (uint32_t)ctx->width ||
        fmt.fmt.pix.height != (uint32_t)ctx->height) {
        goto error;
    }

    ctx->v4l2_pixfmt = fmt.fmt.pix.pixelformat;
    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;

    /* Allocate conversion buffer if YUYV and we want I420 */
    if (ctx->v4l2_pixfmt == V4L2_PIX_FMT_YUYV && ctx->format == 0) {
        size_t pixels;
        if ((ctx->width & 1) != 0 || (ctx->height & 1) != 0 ||
            (size_t)ctx->width > SIZE_MAX / (size_t)ctx->height) {
            goto error;
        }
        pixels = (size_t)ctx->width * (size_t)ctx->height;
        if (pixels > SIZE_MAX - pixels / 2u) goto error;
        ctx->convert_buf_size = pixels + pixels / 2u;
        ctx->convert_buf = malloc(ctx->convert_buf_size);
        if (!ctx->convert_buf) {
            goto error;
        }
    }

    /* Set framerate */
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = mode->framerate_denominator;
    parm.parm.capture.timeperframe.denominator = mode->framerate_numerator;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) == -1 ||
        parm.parm.capture.timeperframe.numerator == 0 ||
        parm.parm.capture.timeperframe.denominator == 0 ||
        (uint64_t)parm.parm.capture.timeperframe.denominator *
                mode->framerate_denominator !=
            (uint64_t)mode->framerate_numerator *
                parm.parm.capture.timeperframe.numerator) {
        goto error;
    }

    /* Initialize mmap buffers */
    if (v4l2_init_mmap(ctx) != 0) {
        goto error;
    }

    return 0;

error:
    v4l2_cleanup_mmap(ctx);
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

static int linux_video_device_open(const char *device_id, void **backend_ctx) {
    const char *path = (device_id && device_id[0]) ? device_id : "/dev/video0";
    linux_video_device_ctx_t *ctx;
    struct v4l2_capability cap = {0};
    int fd;

    if (!backend_ctx || strlen(path) >= sizeof(ctx->path)) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }
    *backend_ctx = NULL;

    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return TURBO_CAPTURE_ERR_DEVICE;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        close(fd);
        return TURBO_CAPTURE_ERR_DEVICE;
    }
    uint32_t capabilities =
        (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? cap.device_caps
            : cap.capabilities;
    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(capabilities & V4L2_CAP_STREAMING)) {
        close(fd);
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (linux_video_device_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        close(fd);
        return TURBO_CAPTURE_ERR_NOMEM;
    }
    ctx->fd = fd;
    memcpy(ctx->path, path, strlen(path) + 1);
    *backend_ctx = ctx;
    return TURBO_CAPTURE_OK;
}

static void linux_video_device_close(void *backend_ctx) {
    linux_video_device_ctx_t *ctx = (linux_video_device_ctx_t *)backend_ctx;
    if (!ctx) return;
    if (ctx->fd >= 0) close(ctx->fd);
    free(ctx);
}

static int linux_video_device_list_modes(
    void *backend_ctx,
    turbo_video_native_mode_t *modes,
    size_t capacity,
    size_t *out_count) {
    linux_video_device_ctx_t *ctx = (linux_video_device_ctx_t *)backend_ctx;
    size_t count = 0;

    if (!ctx || !modes || capacity == 0 || !out_count) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }
    memset(modes, 0, sizeof(*modes) * capacity);

    for (uint32_t format_index = 0; format_index < 0x10000u; ++format_index) {
        struct v4l2_fmtdesc format_desc = {0};
        int format;

        format_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format_desc.index = format_index;
        if (xioctl(ctx->fd, VIDIOC_ENUM_FMT, &format_desc) == -1) break;
        format = v4l2_pixfmt_to_turbo_format(format_desc.pixelformat);
        if (format < 0) continue;

        for (uint32_t size_index = 0; size_index < 0x10000u; ++size_index) {
            struct v4l2_frmsizeenum frame_size = {0};

            frame_size.index = size_index;
            frame_size.pixel_format = format_desc.pixelformat;
            if (xioctl(ctx->fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) == -1) break;
            if (frame_size.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;

            for (uint32_t interval_index = 0;
                 interval_index < 0x10000u;
                 ++interval_index) {
                struct v4l2_frmivalenum interval = {0};
                turbo_video_native_mode_t *mode;

                interval.index = interval_index;
                interval.pixel_format = format_desc.pixelformat;
                interval.width = frame_size.discrete.width;
                interval.height = frame_size.discrete.height;
                if (xioctl(ctx->fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1) {
                    break;
                }
                if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE ||
                    interval.discrete.numerator == 0 ||
                    interval.discrete.denominator == 0) {
                    continue;
                }

                mode = &modes[count++];
                mode->width = (int)frame_size.discrete.width;
                mode->height = (int)frame_size.discrete.height;
                mode->framerate_numerator = interval.discrete.denominator;
                mode->framerate_denominator = interval.discrete.numerator;
                mode->format = format;
                mode->mode_id = v4l2_mode_id(format_index, size_index,
                                             interval_index);
                if (count == capacity) {
                    *out_count = count;
                    return TURBO_CAPTURE_OK;
                }
            }
        }
    }

    *out_count = count;
    return TURBO_CAPTURE_OK;
}

static int linux_video_device_create_capture(
    void *backend_ctx,
    const turbo_video_native_mode_t *mode,
    turbo_capture_t **out_capture) {
    linux_video_device_ctx_t *device =
        (linux_video_device_ctx_t *)backend_ctx;
    turbo_capture_t *capture;

    if (!device || !mode || !out_capture) return TURBO_CAPTURE_ERR_FORMAT;
    *out_capture = NULL;
    capture = (turbo_capture_t *)calloc(1, sizeof(*capture));
    if (!capture) return TURBO_CAPTURE_ERR_NOMEM;
    capture->type = TURBO_CAPTURE_TYPE_VIDEO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;

    if (v4l2_backend_init_exact(capture, device->path, mode) != 0) {
        free(capture);
        return TURBO_CAPTURE_ERR_DEVICE;
    }
    *out_capture = capture;
    return TURBO_CAPTURE_OK;
}

const turbo_video_backend_ops_t *turbo_video_platform_backend(void) {
    static const turbo_video_backend_ops_t ops = {
        linux_video_device_open,
        linux_video_device_close,
        linux_video_device_list_modes,
        linux_video_device_create_capture
    };
    return &ops;
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
    return v4l2_backend_start(capture);
}

void v4l2_video_stop(turbo_capture_t *capture) {
    v4l2_backend_stop(capture);
}

void v4l2_video_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    if (!capture->platform_ctx) {
        free(capture);
        return;
    }

    v4l2_backend_destroy(capture);
    free(capture);
}

#endif /* __linux__ && !__ANDROID__ */
