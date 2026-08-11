#include "ivr_worker_health.h"

#include <stdio.h>
#include <string.h>

static int readiness_conditions_met(
    const ivr_worker_health_snapshot_t *value) {
    return value->ready && !value->draining && value->content_ready &&
           value->schema_ready && value->command_channel_ready &&
           value->event_channel_ready && value->sync_ready &&
           value->speech_ready && value->sfu_ready &&
           value->active_sessions + value->reserved_sessions <
               value->max_sessions;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

int ivr_worker_health_init(ivr_worker_health_t *health,
                           uint32_t max_sessions) {
    if (!health || max_sessions == 0) {
        return -1;
    }
    memset(health, 0, sizeof(*health));
    turbo_mutex_init(&health->mutex);
    health->value.generation = 1;
    health->value.max_sessions = max_sessions;
    copy_text(health->value.reason, sizeof(health->value.reason),
              "initializing");
    return 0;
}

void ivr_worker_health_destroy(ivr_worker_health_t *health) {
    if (!health) {
        return;
    }
    turbo_mutex_destroy(&health->mutex);
    memset(health, 0, sizeof(*health));
}

int ivr_worker_health_update(ivr_worker_health_t *health,
                             const ivr_worker_health_snapshot_t *value) {
    ivr_worker_health_snapshot_t next;
    if (!health || !value || value->max_sessions == 0 ||
        value->active_sessions > value->max_sessions ||
        value->reserved_sessions > value->max_sessions - value->active_sessions ||
        value->generation == 0) {
        return -1;
    }
    turbo_mutex_lock(&health->mutex);
    if (value->generation != health->value.generation) {
        turbo_mutex_unlock(&health->mutex);
        return -1;
    }
    next = *value;
    next.ready = readiness_conditions_met(value);
    if (next.ready != health->value.ready) {
        if (health->value.generation == UINT64_MAX) {
            turbo_mutex_unlock(&health->mutex);
            return -1;
        }
        next.generation = health->value.generation + 1;
    }
    health->value = next;
    health->value.capabilities[sizeof(health->value.capabilities) - 1] = '\0';
    health->value.reason[sizeof(health->value.reason) - 1] = '\0';
    turbo_mutex_unlock(&health->mutex);
    return 0;
}

int ivr_worker_health_snapshot(const ivr_worker_health_t *health,
                               ivr_worker_health_snapshot_t *out) {
    if (!health || !out) {
        return -1;
    }
    turbo_mutex_lock((turbo_mutex_t *)&health->mutex);
    *out = health->value;
    turbo_mutex_unlock((turbo_mutex_t *)&health->mutex);
    return 0;
}

int ivr_worker_health_json(const ivr_worker_health_t *health,
                           char *out, size_t out_capacity) {
    ivr_worker_health_snapshot_t s;
    int n;
    if (!out || out_capacity == 0 || ivr_worker_health_snapshot(health, &s) != 0) {
        return -1;
    }
    n = snprintf(out, out_capacity,
                 "{\"generation\":%llu,\"ready\":%s,\"draining\":%s,"
                 "\"content\":%s,\"schema\":%s,\"command_channel\":%s,"
                 "\"event_channel\":%s,\"sync\":%s,\"speech\":%s,"
                 "\"sfu\":%s,\"active_sessions\":%u,\"reserved_sessions\":%u,"
                 "\"max_sessions\":%u,\"capabilities\":\"%s\",\"reason\":\"%s\"}",
                 (unsigned long long)s.generation, s.ready ? "true" : "false",
                 s.draining ? "true" : "false", s.content_ready ? "true" : "false",
                 s.schema_ready ? "true" : "false",
                 s.command_channel_ready ? "true" : "false",
                 s.event_channel_ready ? "true" : "false", s.sync_ready ? "true" : "false",
                 s.speech_ready ? "true" : "false", s.sfu_ready ? "true" : "false",
                 s.active_sessions, s.reserved_sessions, s.max_sessions,
                 s.capabilities, s.reason);
    return n < 0 || (size_t)n >= out_capacity ? -1 : n;
}
