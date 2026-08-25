#include "ivr_worker_metrics.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *const kMetricNames[IVR_WORKER_METRIC_COUNT] = {
    "assign_accepted_total",       "assign_rejected_total",
    "release_accepted_total",      "release_rejected_total",
    "reply_invalid_total",         "reply_queue_full_total",
    "control_queue_full_total",
    "command_connected_total",    "command_disconnected_total",
    "event_connected_total",       "event_disconnected_total",
    "sync_retry_total",            "heartbeat_failure_total",
    "media_disconnected_total",    "media_reconnected_total",
    "media_retry_exhausted_total", "media_input_stalled_total",
    "provider_error_total",        "drain_total",
    "drain_timeout_total"};

static const char *const kGaugeNames[IVR_WORKER_GAUGE_COUNT] = {
    "reply_queue_items", "reply_queue_bytes",
    "reply_queue_items_high_water", "reply_queue_bytes_high_water",
    "media_peers", "media_peers_high_water", "media_links_connected",
    "media_links_connected_high_water"};

static const char *const kHistogramNames[IVR_WORKER_HISTOGRAM_COUNT] = {
    "dispatch_duration_seconds",
    "event_to_inbox_duration_seconds",
    "dtmf_to_cancel_duration_seconds",
    "asr_to_command_duration_seconds",
    "tts_provider_duration_seconds",
    "asr_provider_duration_seconds"};

static const uint64_t kHistogramUpperBoundsMs[
    IVR_WORKER_HISTOGRAM_BUCKET_COUNT - 1] = {
    1u,   5u,   10u,   25u,   50u,    100u,   250u,
    500u, 1000u, 2500u, 5000u, 10000u, 30000u};

static const char *const kHistogramUpperBoundsSeconds[
    IVR_WORKER_HISTOGRAM_BUCKET_COUNT] = {
    "0.001", "0.005", "0.010", "0.025", "0.050", "0.100", "0.250",
    "0.500", "1.000", "2.500", "5.000", "10.000", "30.000", "+Inf"};

void ivr_worker_metrics_init(ivr_worker_metrics_t *metrics) {
    size_t i;
    size_t j;
    if (!metrics) {
        return;
    }
    for (i = 0; i < IVR_WORKER_METRIC_COUNT; ++i) {
        atomic_init(&metrics->counters[i], 0);
    }
    for (i = 0; i < IVR_WORKER_GAUGE_COUNT; ++i) {
        atomic_init(&metrics->gauges[i], 0);
    }
    for (i = 0; i < IVR_WORKER_HISTOGRAM_COUNT; ++i) {
        for (j = 0; j < IVR_WORKER_HISTOGRAM_BUCKET_COUNT; ++j) {
            atomic_init(&metrics->histograms[i].buckets[j], 0);
        }
        atomic_init(&metrics->histograms[i].count, 0);
        atomic_init(&metrics->histograms[i].sum_ms, 0);
    }
}

void ivr_worker_metrics_observe_ms(ivr_worker_metrics_t *metrics,
                                   ivr_worker_histogram_kind_t kind,
                                   uint64_t duration_ms) {
    size_t bucket = IVR_WORKER_HISTOGRAM_BUCKET_COUNT - 1u;
    size_t i;
    if (!metrics || (unsigned)kind >= IVR_WORKER_HISTOGRAM_COUNT) {
        return;
    }
    for (i = 0; i < IVR_WORKER_HISTOGRAM_BUCKET_COUNT - 1u; ++i) {
        if (duration_ms <= kHistogramUpperBoundsMs[i]) {
            bucket = i;
            break;
        }
    }
    (void)atomic_fetch_add_explicit(
        &metrics->histograms[kind].buckets[bucket], 1, memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&metrics->histograms[kind].count, 1,
                                    memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&metrics->histograms[kind].sum_ms,
                                    duration_ms, memory_order_relaxed);
}

void ivr_worker_metrics_inc(ivr_worker_metrics_t *metrics,
                            ivr_worker_metric_kind_t kind) {
    ivr_worker_metrics_add(metrics, kind, 1);
}

void ivr_worker_metrics_add(ivr_worker_metrics_t *metrics,
                            ivr_worker_metric_kind_t kind, uint64_t value) {
    if (!metrics || (unsigned)kind >= IVR_WORKER_METRIC_COUNT) {
        return;
    }
    (void)atomic_fetch_add_explicit(&metrics->counters[kind], value,
                                    memory_order_relaxed);
}

uint64_t ivr_worker_metrics_get(const ivr_worker_metrics_t *metrics,
                                ivr_worker_metric_kind_t kind) {
    if (!metrics || (unsigned)kind >= IVR_WORKER_METRIC_COUNT) {
        return 0;
    }
    return atomic_load_explicit(&metrics->counters[kind], memory_order_relaxed);
}

void ivr_worker_metrics_set_gauge(ivr_worker_metrics_t *metrics,
                                  ivr_worker_gauge_kind_t kind,
                                  uint64_t value) {
    if (!metrics || (unsigned)kind >= IVR_WORKER_GAUGE_COUNT) {
        return;
    }
    atomic_store_explicit(&metrics->gauges[kind], value, memory_order_relaxed);
}

void ivr_worker_metrics_set_gauge_max(ivr_worker_metrics_t *metrics,
                                      ivr_worker_gauge_kind_t kind,
                                      uint64_t value) {
    uint64_t current;
    if (!metrics || (unsigned)kind >= IVR_WORKER_GAUGE_COUNT) {
        return;
    }
    current = atomic_load_explicit(&metrics->gauges[kind], memory_order_relaxed);
    while (current < value &&
           !atomic_compare_exchange_weak_explicit(
               &metrics->gauges[kind], &current, value, memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

int ivr_worker_metrics_add_gauge(ivr_worker_metrics_t *metrics,
                                 ivr_worker_gauge_kind_t kind,
                                 uint64_t value, uint64_t *out_value) {
    uint64_t current;
    if (!metrics || (unsigned)kind >= IVR_WORKER_GAUGE_COUNT || !out_value) {
        return -1;
    }
    current = atomic_load_explicit(&metrics->gauges[kind],
                                   memory_order_relaxed);
    do {
        if (UINT64_MAX - current < value) {
            return -1;
        }
    } while (!atomic_compare_exchange_weak_explicit(
        &metrics->gauges[kind], &current, current + value,
        memory_order_relaxed, memory_order_relaxed));
    *out_value = current + value;
    return 0;
}

int ivr_worker_metrics_sub_gauge(ivr_worker_metrics_t *metrics,
                                 ivr_worker_gauge_kind_t kind,
                                 uint64_t value, uint64_t *out_value) {
    uint64_t current;
    if (!metrics || (unsigned)kind >= IVR_WORKER_GAUGE_COUNT || !out_value) {
        return -1;
    }
    current = atomic_load_explicit(&metrics->gauges[kind],
                                   memory_order_relaxed);
    do {
        if (current < value) {
            return -1;
        }
    } while (!atomic_compare_exchange_weak_explicit(
        &metrics->gauges[kind], &current, current - value,
        memory_order_relaxed, memory_order_relaxed));
    *out_value = current - value;
    return 0;
}

uint64_t ivr_worker_metrics_get_gauge(const ivr_worker_metrics_t *metrics,
                                      ivr_worker_gauge_kind_t kind) {
    if (!metrics || (unsigned)kind >= IVR_WORKER_GAUGE_COUNT) {
        return 0;
    }
    return atomic_load_explicit(&metrics->gauges[kind], memory_order_relaxed);
}

static int append_counter(char *out, size_t capacity, size_t *used,
                          const char *name, uint64_t value) {
    if (*used >= capacity) {
        return -1;
    }
    int n = snprintf(out + *used, capacity - *used,
                     "# TYPE turbo_ivr_worker_%s counter\n"
                     "turbo_ivr_worker_%s %llu\n",
                     name, name,
                     (unsigned long long)value);
    if (n < 0 || (size_t)n >= capacity - *used) {
        return -1;
    }
    *used += (size_t)n;
    return 0;
}

static int append_gauge(char *out, size_t capacity, size_t *used,
                        const char *name, uint64_t value) {
    int n;
    if (*used >= capacity) {
        return -1;
    }
    n = snprintf(out + *used, capacity - *used,
                 "# TYPE turbo_ivr_worker_%s gauge\n"
                 "turbo_ivr_worker_%s %llu\n",
                 name, name, (unsigned long long)value);
    if (n < 0 || (size_t)n >= capacity - *used) {
        return -1;
    }
    *used += (size_t)n;
    return 0;
}

static int append_histogram(const ivr_worker_metrics_t *metrics,
                            ivr_worker_histogram_kind_t kind, char *out,
                            size_t capacity, size_t *used) {
    const ivr_worker_histogram_t *histogram = &metrics->histograms[kind];
    uint64_t cumulative = 0;
    uint64_t count;
    uint64_t sum_ms;
    size_t i;
    int n;
    n = snprintf(out + *used, capacity - *used,
                 "# TYPE turbo_ivr_worker_%s histogram\n",
                 kHistogramNames[kind]);
    if (n < 0 || (size_t)n >= capacity - *used) {
        return -1;
    }
    *used += (size_t)n;
    for (i = 0; i < IVR_WORKER_HISTOGRAM_BUCKET_COUNT; ++i) {
        cumulative += atomic_load_explicit(&histogram->buckets[i],
                                           memory_order_relaxed);
        n = snprintf(out + *used, capacity - *used,
                     "turbo_ivr_worker_%s_bucket{le=\"%s\"} %llu\n",
                     kHistogramNames[kind], kHistogramUpperBoundsSeconds[i],
                     (unsigned long long)cumulative);
        if (n < 0 || (size_t)n >= capacity - *used) {
            return -1;
        }
        *used += (size_t)n;
    }
    count = atomic_load_explicit(&histogram->count, memory_order_relaxed);
    sum_ms = atomic_load_explicit(&histogram->sum_ms, memory_order_relaxed);
    n = snprintf(out + *used, capacity - *used,
                 "turbo_ivr_worker_%s_sum %llu.%03llu\n"
                 "turbo_ivr_worker_%s_count %llu\n",
                 kHistogramNames[kind],
                 (unsigned long long)(sum_ms / 1000u),
                 (unsigned long long)(sum_ms % 1000u),
                 kHistogramNames[kind], (unsigned long long)count);
    if (n < 0 || (size_t)n >= capacity - *used) {
        return -1;
    }
    *used += (size_t)n;
    return 0;
}

int ivr_worker_metrics_render(const ivr_worker_metrics_t *metrics,
                              const ivr_worker_health_snapshot_t *health,
                              char *out, size_t capacity) {
    size_t used = 0;
    size_t i;
    int n;
    if (!metrics || !health || !out || capacity == 0) {
        return -1;
    }
    n = snprintf(out, capacity,
                 "# TYPE turbo_ivr_worker_ready gauge\n"
                 "turbo_ivr_worker_ready %d\n"
                 "# TYPE turbo_ivr_worker_draining gauge\n"
                 "turbo_ivr_worker_draining %d\n"
                 "# TYPE turbo_ivr_worker_active_sessions gauge\n"
                 "turbo_ivr_worker_active_sessions %u\n"
                 "# TYPE turbo_ivr_worker_reserved_sessions gauge\n"
                 "turbo_ivr_worker_reserved_sessions %u\n"
                 "# TYPE turbo_ivr_worker_max_sessions gauge\n"
                 "turbo_ivr_worker_max_sessions %u\n"
                 "# TYPE turbo_ivr_worker_health_generation gauge\n"
                 "turbo_ivr_worker_health_generation %llu\n",
                 health->ready ? 1 : 0, health->draining ? 1 : 0,
                 health->active_sessions, health->reserved_sessions,
                 health->max_sessions,
                 (unsigned long long)health->generation);
    if (n < 0 || (size_t)n >= capacity) {
        return -1;
    }
    used = (size_t)n;
    for (i = 0; i < IVR_WORKER_METRIC_COUNT; ++i) {
        if (append_counter(out, capacity, &used, kMetricNames[i],
                           ivr_worker_metrics_get(
                               metrics, (ivr_worker_metric_kind_t)i)) != 0) {
            return -1;
        }
    }
    for (i = 0; i < IVR_WORKER_GAUGE_COUNT; ++i) {
        if (append_gauge(out, capacity, &used, kGaugeNames[i],
                         ivr_worker_metrics_get_gauge(
                             metrics, (ivr_worker_gauge_kind_t)i)) != 0) {
            return -1;
        }
    }
    for (i = 0; i < IVR_WORKER_HISTOGRAM_COUNT; ++i) {
        if (append_histogram(metrics, (ivr_worker_histogram_kind_t)i, out,
                             capacity, &used) != 0) {
            return -1;
        }
    }
    return (int)used;
}
