#include "turbo_pipeline.h"

#include <salts_fs.h>
#include <salts_thread.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t NANOSECONDS_PER_MILLISECOND = 1000000u;
static const uint64_t MILLISECONDS_PER_SECOND = 1000u;
static const uint64_t MILLISECONDS_PER_MINUTE = 60000u;
static const uint64_t MAX_STOP_AFTER_MILLISECONDS = UINT32_MAX - 1u;

typedef struct stop_timer {
    turbo_pipeline_t *pipeline;
    salts_mutex_t mutex;
    salts_cond_t condition;
    salts_thread_t thread;
    uint64_t duration_ms;
    int cancelled;
    int fired;
    turbo_pipeline_status_t request_status;
} stop_timer_t;

static void print_pipeline_error(const char *operation,
                                 const turbo_pipeline_error_t *error) {
    if (error->node_id[0] != '\0')
        fprintf(stderr, "%s failed at node '%s': %s (status=%d)\n", operation,
                error->node_id, error->message, (int)error->code);
    else
        fprintf(stderr, "%s failed: %s (status=%d)\n", operation, error->message,
                (int)error->code);
}

static int parse_duration_ms(const char *text, uint64_t *duration_ms) {
    unsigned long long value;
    uint64_t multiplier;
    char *suffix;

    if (!text || !duration_ms || text[0] < '0' || text[0] > '9') return 0;
    errno = 0;
    value = strtoull(text, &suffix, 10);
    if (errno == ERANGE || suffix == text || value == 0) return 0;
    if (strcmp(suffix, "ms") == 0)
        multiplier = 1;
    else if (strcmp(suffix, "s") == 0)
        multiplier = MILLISECONDS_PER_SECOND;
    else if (strcmp(suffix, "m") == 0)
        multiplier = MILLISECONDS_PER_MINUTE;
    else
        return 0;
    if (value > MAX_STOP_AFTER_MILLISECONDS / multiplier) return 0;
    *duration_ms = (uint64_t)value * multiplier;
    return 1;
}

static int stop_timer_init(stop_timer_t *timer, turbo_pipeline_t *pipeline,
                           uint64_t duration_ms) {
    memset(timer, 0, sizeof(*timer));
    timer->pipeline = pipeline;
    timer->duration_ms = duration_ms;
    timer->request_status = TURBO_PIPELINE_OK;
    salts_mutex_init(&timer->mutex);
    salts_cond_init(&timer->condition);
    if (!timer->mutex || !timer->condition) {
        salts_cond_destroy(&timer->condition);
        salts_mutex_destroy(&timer->mutex);
        return 0;
    }
    return 1;
}

static void stop_timer_entry(void *argument) {
    stop_timer_t *timer = (stop_timer_t *)argument;
    uint64_t started_ms = salts_monotonic_ms();
    uint64_t deadline_ms =
        timer->duration_ms > UINT64_MAX - started_ms
            ? UINT64_MAX
            : started_ms + timer->duration_ms;

    salts_mutex_lock(&timer->mutex);
    while (!timer->cancelled) {
        uint64_t now_ms = salts_monotonic_ms();
        uint64_t remaining_ms;
        if (now_ms >= deadline_ms) {
            timer->fired = 1;
            break;
        }
        remaining_ms = deadline_ms - now_ms;
        salts_cond_timedwait(&timer->condition, &timer->mutex,
                             remaining_ms * NANOSECONDS_PER_MILLISECOND);
    }
    salts_mutex_unlock(&timer->mutex);
    if (timer->fired)
        timer->request_status = turbo_pipeline_request_stop(timer->pipeline);
}

static int stop_timer_start(stop_timer_t *timer) {
    return salts_thread_create(&timer->thread, stop_timer_entry, timer);
}

static int stop_timer_cancel_and_join(stop_timer_t *timer) {
    int join_status;
    salts_mutex_lock(&timer->mutex);
    timer->cancelled = 1;
    salts_cond_signal(&timer->condition);
    salts_mutex_unlock(&timer->mutex);
    join_status = salts_thread_join(&timer->thread);
    salts_cond_destroy(&timer->condition);
    salts_mutex_destroy(&timer->mutex);
    return join_status;
}

int main(int argc, char **argv) {
    salts_fs_buf_t yaml = {0};
    turbo_pipeline_error_t error;
    turbo_pipeline_stats_t stats;
    turbo_pipeline_t *pipeline = NULL;
    stop_timer_t stop_timer = {0};
    turbo_pipeline_status_t status;
    const char *graph_path;
    uint64_t stop_after_ms = 0;
    int file_status;
    int timer_started = 0;
    int timer_fired = 0;
    int stopped_by_timer = 0;
    int exit_code = 1;

    if (argc == 2) {
        graph_path = argv[1];
    } else if (argc == 4 && strcmp(argv[1], "--stop-after") == 0 &&
               parse_duration_ms(argv[2], &stop_after_ms)) {
        graph_path = argv[3];
    } else {
        fprintf(stderr,
                "usage: turbo_pipeline_run [--stop-after <Nms|Ns|Nm>] "
                "<graph.yml>\n");
        return 2;
    }

    file_status = salts_fs_read_file(graph_path, &yaml);
    if (file_status != 0) {
        fprintf(stderr, "cannot read pipeline graph '%s' (status=%d)\n", graph_path,
                file_status);
        return 1;
    }

    pipeline = turbo_pipeline_create_from_yaml(yaml.base, yaml.len, &error);
    salts_fs_buf_free(&yaml);
    if (!pipeline) {
        print_pipeline_error("create pipeline", &error);
        return 1;
    }

    status = turbo_pipeline_prepare(pipeline, &error);
    if (status != TURBO_PIPELINE_OK) {
        print_pipeline_error("prepare pipeline", &error);
        goto cleanup;
    }

    if (stop_after_ms > 0) {
        int timer_status;
        if (!stop_timer_init(&stop_timer, pipeline, stop_after_ms)) {
            fprintf(stderr, "cannot initialize stop timer\n");
            goto cleanup;
        }
        timer_status = stop_timer_start(&stop_timer);
        if (timer_status != 0) {
            fprintf(stderr, "cannot start stop timer (status=%d)\n", timer_status);
            salts_cond_destroy(&stop_timer.condition);
            salts_mutex_destroy(&stop_timer.mutex);
            goto cleanup;
        }
        timer_started = 1;
    }

    status = turbo_pipeline_run(pipeline, &error);
    if (timer_started) {
        int join_status = stop_timer_cancel_and_join(&stop_timer);
        timer_started = 0;
        timer_fired = stop_timer.fired;
        if (join_status != 0) {
            fprintf(stderr, "cannot join stop timer (status=%d)\n", join_status);
            goto cleanup;
        }
        if (timer_fired && stop_timer.request_status != TURBO_PIPELINE_OK &&
            stop_timer.request_status != TURBO_PIPELINE_ESTATE) {
            fprintf(stderr, "stop timer request failed (status=%d)\n",
                    (int)stop_timer.request_status);
            goto cleanup;
        }
    }
    stopped_by_timer = status == TURBO_PIPELINE_ESTOPPED && timer_fired;
    if (status != TURBO_PIPELINE_OK && !stopped_by_timer) {
        print_pipeline_error("run pipeline", &error);
        goto cleanup;
    }

    status = turbo_pipeline_stats(pipeline, &stats);
    if (status != TURBO_PIPELINE_OK) {
        fprintf(stderr, "cannot read pipeline statistics (status=%d)\n", (int)status);
        goto cleanup;
    }
    printf("packets: read=%" PRIu64 " written=%" PRIu64
           ", frames: decoded=%" PRIu64 " encoded=%" PRIu64
           ", bytes: read=%" PRIu64 " written=%" PRIu64 "\n",
           stats.packets_read, stats.packets_written, stats.frames_decoded,
           stats.frames_encoded, stats.bytes_read, stats.bytes_written);
    if (stopped_by_timer)
        printf("stop-after reached after %" PRIu64 " ms\n", stop_after_ms);
    exit_code = 0;

cleanup:
    if (timer_started) stop_timer_cancel_and_join(&stop_timer);
    turbo_pipeline_destroy(pipeline);
    return exit_code;
}
