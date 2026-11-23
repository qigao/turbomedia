#ifndef TURBO_MONITOR_INTERNAL_H
#define TURBO_MONITOR_INTERNAL_H

#include <memory/buffer.h>
#include <threads/threads.h>
#include <core/tstr.h>
#include <stdatomic.h>
#include <stdint.h>

enum {
    MONITOR_MAX_SOURCES = 8,
    MONITOR_MAX_CLIPS = 64,
    MONITOR_PREVIEW_BYTES = 128 * 1024
};

typedef enum {
    MONITOR_CONNECTING,
    MONITOR_LIVE,
    MONITOR_OFFLINE,
    MONITOR_UNSUPPORTED
} monitor_signal_t;

typedef enum {
    MONITOR_CLIP_NONE,
    MONITOR_CLIP_PENDING,
    MONITOR_CLIP_READY,
    MONITOR_CLIP_UNAVAILABLE
} monitor_clip_status_t;

typedef struct monitor_state {
    monitor_signal_t signal;
    monitor_clip_status_t clip;
    uint64_t last_frame_ms;
    uint64_t preview_version;
    uint64_t incident;
    uint64_t opened_ms;
    uint64_t recovered_ms;
    bool acknowledged;
} monitor_state_t;

typedef struct monitor_clip monitor_clip_t;
typedef struct monitor monitor_t;

typedef struct monitor_source {
    monitor_t *monitor;
    unsigned id;
    const char *name;
    const char *url;
    sync_mutex_t mutex;
    sync_thread_t thread;
    monitor_state_t state;
    mem_buffer_t *preview;
    monitor_clip_t *job;
} monitor_source_t;

struct monitor {
    unsigned count;
    monitor_source_t sources[MONITOR_MAX_SOURCES];
    tstr archive;
    atomic_bool stop;
    sync_mutex_t archive_mutex;
    sync_cond_t archive_ready;
    sync_thread_t archive_thread;
    bool archive_closed;
    unsigned clips_submitted;
};

bool monitor_start(monitor_t *monitor);
void monitor_stop(monitor_t *monitor);
tstr monitor_clip_path(const monitor_source_t *source, uint64_t incident);

#endif
