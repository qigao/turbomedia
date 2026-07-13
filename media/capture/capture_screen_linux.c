/**
 * Linux Screen Capture Implementation
 *
 * Uses PipeWire Portal (Wayland) with X11 XShm fallback
 */
#include "turbo_capture.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifdef TURBO_HAS_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#endif

#ifdef TURBO_HAS_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <stb_sprintf.h>
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define SCREEN_POLL_TIMEOUT     100  /* ms */

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    int use_wayland;

#ifdef TURBO_HAS_PIPEWIRE
    /* PipeWire (Wayland) */
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    int pw_width;
    int pw_height;
#endif

#ifdef TURBO_HAS_X11
    /* X11 */
    Display *display;
    Window root;
    XShmSegmentInfo shminfo;
    XImage *image;
    int shm_attached;
#endif

    pthread_t capture_thread;
    volatile int running;

    int monitor_index;
    int framerate;
    int capture_cursor;
    int width;
    int height;

    uint8_t *frame_buffer;
    size_t frame_buffer_size;

    /* Timing */
    uint64_t frame_interval_us;
    uint64_t last_frame_time;

    /* Parent capture */
    turbo_capture_t *capture;
} screen_ctx_t;

/* =============================================================================
 * Helpers
 * ============================================================================= */

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* =============================================================================
 * PipeWire Portal Implementation (Wayland)
 * ============================================================================= */

#ifdef TURBO_HAS_PIPEWIRE

static void on_stream_param_changed(void *userdata, uint32_t id,
                                     const struct spa_pod *param) {
    screen_ctx_t *ctx = (screen_ctx_t *)userdata;

    if (param == NULL || id != SPA_PARAM_Format) return;

    struct spa_video_info format;
    if (spa_format_video_raw_parse(param, &format.info.raw) < 0) return;

    ctx->pw_width = format.info.raw.size.width;
    ctx->pw_height = format.info.raw.size.height;
    ctx->width = ctx->pw_width;
    ctx->height = ctx->pw_height;

    /* Allocate frame buffer */
    ctx->frame_buffer_size = ctx->width * ctx->height * 4;  /* BGRA */
    ctx->frame_buffer = realloc(ctx->frame_buffer, ctx->frame_buffer_size);
}

static void on_stream_process(void *userdata) {
    screen_ctx_t *ctx = (screen_ctx_t *)userdata;
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

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

static void *pipewire_screen_thread(void *arg) {
    screen_ctx_t *ctx = (screen_ctx_t *)arg;
    pw_main_loop_run(ctx->loop);
    return NULL;
}

/*
 * Note: Full PipeWire Portal implementation requires D-Bus interaction
 * with xdg-desktop-portal. This is a simplified version that works
 * when a PipeWire screen capture stream is already available.
 *
 * For full implementation, you would need:
 * 1. Connect to session D-Bus
 * 2. Call org.freedesktop.portal.ScreenCast.CreateSession
 * 3. Call SelectSources and Start
 * 4. Get the PipeWire node ID from the portal response
 * 5. Connect to that node
 *
 * This simplified version attempts to connect to any available screen source.
 */
static int pipewire_screen_init(screen_ctx_t *ctx) {
    pw_init(NULL, NULL);

    ctx->loop = pw_main_loop_new(NULL);
    if (!ctx->loop) {
        return -1;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);

    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "turbo-screen-capture",
        props,
        &stream_events,
        ctx);

    if (!ctx->stream) {
        pw_main_loop_destroy(ctx->loop);
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
            &SPA_RECTANGLE(1920, 1080),
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
        return -1;
    }

    ctx->use_wayland = 1;
    return 0;
}

static int pipewire_screen_start(screen_ctx_t *ctx) {
    ctx->running = 1;
    if (pthread_create(&ctx->capture_thread, NULL,
                       pipewire_screen_thread, ctx) != 0) {
        ctx->running = 0;
        return -1;
    }
    return 0;
}

static void pipewire_screen_stop(screen_ctx_t *ctx) {
    if (ctx->running) {
        ctx->running = 0;
        if (ctx->loop) {
            pw_main_loop_quit(ctx->loop);
        }
        pthread_join(ctx->capture_thread, NULL);
    }
}

static void pipewire_screen_cleanup(screen_ctx_t *ctx) {
    if (ctx->stream) {
        pw_stream_destroy(ctx->stream);
        ctx->stream = NULL;
    }
    if (ctx->loop) {
        pw_main_loop_destroy(ctx->loop);
        ctx->loop = NULL;
    }
    pw_deinit();
}

#endif /* TURBO_HAS_PIPEWIRE */

/* =============================================================================
 * X11 XShm Implementation
 * ============================================================================= */

#ifdef TURBO_HAS_X11

static void *x11_screen_thread(void *arg) {
    screen_ctx_t *ctx = (screen_ctx_t *)arg;
    turbo_capture_t *capture = ctx->capture;

    while (ctx->running) {
        uint64_t now = get_timestamp_us();
        uint64_t elapsed = now - ctx->last_frame_time;

        if (elapsed < ctx->frame_interval_us) {
            usleep((ctx->frame_interval_us - elapsed));
            now = get_timestamp_us();
        }

        /* Capture screen */
        if (!XShmGetImage(ctx->display, ctx->root, ctx->image, 0, 0, AllPlanes)) {
            continue;
        }

        /* Deliver frame */
        if (capture->video_cb) {
            capture->video_cb(capture, (uint8_t *)ctx->image->data,
                              ctx->frame_buffer_size,
                              ctx->width, ctx->height,
                              now, capture->user_data);
        }

        ctx->last_frame_time = now;
    }

    return NULL;
}

static int x11_screen_init(screen_ctx_t *ctx) {
    const char *display_name = getenv("DISPLAY");
    if (!display_name) {
        return -1;
    }

    ctx->display = XOpenDisplay(display_name);
    if (!ctx->display) {
        return -1;
    }

    /* Check XShm extension */
    if (!XShmQueryExtension(ctx->display)) {
        XCloseDisplay(ctx->display);
        ctx->display = NULL;
        return -1;
    }

    int screen = DefaultScreen(ctx->display);

    /* Get screen/monitor info */
    if (ctx->monitor_index < 0) {
        /* Capture entire screen */
        ctx->root = RootWindow(ctx->display, screen);
        ctx->width = DisplayWidth(ctx->display, screen);
        ctx->height = DisplayHeight(ctx->display, screen);
    } else {
        /* TODO: Use Xinerama/XRandR for multi-monitor */
        ctx->root = RootWindow(ctx->display, screen);
        ctx->width = DisplayWidth(ctx->display, screen);
        ctx->height = DisplayHeight(ctx->display, screen);
    }

    /* Create shared memory image */
    Visual *visual = DefaultVisual(ctx->display, screen);
    int depth = DefaultDepth(ctx->display, screen);

    ctx->image = XShmCreateImage(ctx->display, visual, depth,
                                  ZPixmap, NULL, &ctx->shminfo,
                                  ctx->width, ctx->height);
    if (!ctx->image) {
        XCloseDisplay(ctx->display);
        ctx->display = NULL;
        return -1;
    }

    ctx->frame_buffer_size = ctx->image->bytes_per_line * ctx->height;

    /* Allocate shared memory */
    ctx->shminfo.shmid = shmget(IPC_PRIVATE, ctx->frame_buffer_size,
                                 IPC_CREAT | 0777);
    if (ctx->shminfo.shmid < 0) {
        XDestroyImage(ctx->image);
        XCloseDisplay(ctx->display);
        ctx->display = NULL;
        return -1;
    }

    ctx->shminfo.shmaddr = ctx->image->data = shmat(ctx->shminfo.shmid, NULL, 0);
    ctx->shminfo.readOnly = False;

    if (!XShmAttach(ctx->display, &ctx->shminfo)) {
        shmdt(ctx->shminfo.shmaddr);
        shmctl(ctx->shminfo.shmid, IPC_RMID, NULL);
        XDestroyImage(ctx->image);
        XCloseDisplay(ctx->display);
        ctx->display = NULL;
        return -1;
    }

    ctx->shm_attached = 1;

    /* Mark for deletion (will be freed when detached) */
    shmctl(ctx->shminfo.shmid, IPC_RMID, NULL);

    ctx->use_wayland = 0;
    return 0;
}

static int x11_screen_start(screen_ctx_t *ctx) {
    ctx->running = 1;
    ctx->last_frame_time = get_timestamp_us();

    if (pthread_create(&ctx->capture_thread, NULL,
                       x11_screen_thread, ctx) != 0) {
        ctx->running = 0;
        return -1;
    }
    return 0;
}

static void x11_screen_stop(screen_ctx_t *ctx) {
    if (ctx->running) {
        ctx->running = 0;
        pthread_join(ctx->capture_thread, NULL);
    }
}

static void x11_screen_cleanup(screen_ctx_t *ctx) {
    if (ctx->display) {
        if (ctx->shm_attached) {
            XShmDetach(ctx->display, &ctx->shminfo);
            shmdt(ctx->shminfo.shmaddr);
            ctx->shm_attached = 0;
        }
        if (ctx->image) {
            /* Note: XDestroyImage would free data, but we used shm */
            ctx->image->data = NULL;
            XDestroyImage(ctx->image);
            ctx->image = NULL;
        }
        XCloseDisplay(ctx->display);
        ctx->display = NULL;
    }
}

#endif /* TURBO_HAS_X11 */

/* =============================================================================
 * Screen Enumeration
 * ============================================================================= */

int turbo_capture_list_screens(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    int count = 0;

#ifdef TURBO_HAS_X11
    const char *display_name = getenv("DISPLAY");
    if (display_name) {
        Display *display = XOpenDisplay(display_name);
        if (display) {
            int screen_count = ScreenCount(display);

            for (int i = 0; i < screen_count && count < max_count; i++) {
                turbo_capture_device_t *dev = &devices[count];
                memset(dev, 0, sizeof(*dev));

                dev->index = count;
                dev->type = TURBO_CAPTURE_TYPE_SCREEN;
                dev->is_default = (count == 0) ? 1 : 0;

                int width = DisplayWidth(display, i);
                int height = DisplayHeight(display, i);
                snprintf(dev->name, sizeof(dev->name),
                         "Screen %d (%dx%d)", i, width, height);
                snprintf(dev->id, sizeof(dev->id), "%d", i);

                count++;
            }

            XCloseDisplay(display);
        }
    }
#endif

    /* Add default if none found */
    if (count == 0 && max_count > 0) {
        turbo_capture_device_t *dev = &devices[0];
        memset(dev, 0, sizeof(*dev));
        dev->index = 0;
        dev->type = TURBO_CAPTURE_TYPE_SCREEN;
        dev->is_default = 1;
        snprintf(dev->name, sizeof(dev->name), "Primary Screen");
        snprintf(dev->id, sizeof(dev->id), "0");
        count = 1;
    }

    return count;
}

/* =============================================================================
 * Screen Capture Implementation
 * ============================================================================= */

turbo_capture_t *turbo_screen_capture_create(const turbo_screen_capture_config_t *config) {
    turbo_capture_t *capture = calloc(1, sizeof(turbo_capture_t));
    if (!capture) return NULL;

    screen_ctx_t *ctx = calloc(1, sizeof(screen_ctx_t));
    if (!ctx) {
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_SCREEN;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = ctx;

    ctx->capture = capture;
    ctx->monitor_index = config ? config->monitor_index : -1;
    ctx->framerate = config ? config->framerate : 30;
    ctx->capture_cursor = config ? config->capture_cursor : 1;
    ctx->frame_interval_us = 1000000 / ctx->framerate;

    /* Try Wayland (PipeWire Portal) first */
#ifdef TURBO_HAS_PIPEWIRE
    if (getenv("WAYLAND_DISPLAY")) {
        if (pipewire_screen_init(ctx) == 0) {
            return capture;
        }
    }
#endif

    /* Fallback to X11 */
#ifdef TURBO_HAS_X11
    if (getenv("DISPLAY")) {
        if (x11_screen_init(ctx) == 0) {
            return capture;
        }
    }
#endif

    /* No screen capture backend available */
    free(ctx);
    free(capture);
    return NULL;
}

void turbo_screen_capture_set_callback(turbo_capture_t *capture,
                                        turbo_video_capture_cb cb,
                                        void *user_data) {
    if (!capture) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

/* =============================================================================
 * Platform Hooks
 * ============================================================================= */

int linux_screen_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;
    screen_ctx_t *ctx = (screen_ctx_t *)capture->platform_ctx;

#ifdef TURBO_HAS_PIPEWIRE
    if (ctx->use_wayland) {
        return pipewire_screen_start(ctx);
    }
#endif

#ifdef TURBO_HAS_X11
    if (!ctx->use_wayland) {
        return x11_screen_start(ctx);
    }
#endif

    return -1;
}

void linux_screen_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;
    screen_ctx_t *ctx = (screen_ctx_t *)capture->platform_ctx;

#ifdef TURBO_HAS_PIPEWIRE
    if (ctx->use_wayland) {
        pipewire_screen_stop(ctx);
        return;
    }
#endif

#ifdef TURBO_HAS_X11
    if (!ctx->use_wayland) {
        x11_screen_stop(ctx);
    }
#endif
}

void linux_screen_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    linux_screen_stop(capture);

    screen_ctx_t *ctx = (screen_ctx_t *)capture->platform_ctx;
    if (ctx) {
#ifdef TURBO_HAS_PIPEWIRE
        if (ctx->use_wayland) {
            pipewire_screen_cleanup(ctx);
        }
#endif

#ifdef TURBO_HAS_X11
        if (!ctx->use_wayland) {
            x11_screen_cleanup(ctx);
        }
#endif

        if (ctx->frame_buffer) {
            free(ctx->frame_buffer);
        }
        free(ctx);
    }

    free(capture);
}

#endif /* __linux__ && !__ANDROID__ */
