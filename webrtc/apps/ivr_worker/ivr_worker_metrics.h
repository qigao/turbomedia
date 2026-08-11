#ifndef TURBO_MEDIA_IVR_WORKER_METRICS_H
#define TURBO_MEDIA_IVR_WORKER_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "ivr_worker_health.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IVR_WORKER_METRIC_ASSIGN_ACCEPTED = 0,
    IVR_WORKER_METRIC_ASSIGN_REJECTED,
    IVR_WORKER_METRIC_RELEASE_ACCEPTED,
    IVR_WORKER_METRIC_RELEASE_REJECTED,
    IVR_WORKER_METRIC_REPLY_INVALID,
    IVR_WORKER_METRIC_REPLY_QUEUE_FULL,
    IVR_WORKER_METRIC_COMMAND_CONNECTED,
    IVR_WORKER_METRIC_COMMAND_DISCONNECTED,
    IVR_WORKER_METRIC_EVENT_CONNECTED,
    IVR_WORKER_METRIC_EVENT_DISCONNECTED,
    IVR_WORKER_METRIC_SYNC_RETRY,
    IVR_WORKER_METRIC_HEARTBEAT_FAILURE,
    IVR_WORKER_METRIC_MEDIA_DISCONNECTED,
    IVR_WORKER_METRIC_MEDIA_RECONNECTED,
    IVR_WORKER_METRIC_MEDIA_RETRY_EXHAUSTED,
    IVR_WORKER_METRIC_MEDIA_INPUT_STALLED,
    IVR_WORKER_METRIC_PROVIDER_ERROR,
    IVR_WORKER_METRIC_DRAIN,
    IVR_WORKER_METRIC_DRAIN_TIMEOUT,
    IVR_WORKER_METRIC_COUNT
} ivr_worker_metric_kind_t;

typedef enum {
    IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS = 0,
    IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES,
    IVR_WORKER_GAUGE_REPLY_QUEUE_ITEMS_HIGH_WATER,
    IVR_WORKER_GAUGE_REPLY_QUEUE_BYTES_HIGH_WATER,
    IVR_WORKER_GAUGE_MEDIA_PEERS,
    IVR_WORKER_GAUGE_MEDIA_PEERS_HIGH_WATER,
    IVR_WORKER_GAUGE_COUNT
} ivr_worker_gauge_kind_t;

typedef enum {
    IVR_WORKER_HISTOGRAM_DISPATCH = 0,
    IVR_WORKER_HISTOGRAM_EVENT_TO_INBOX,
    IVR_WORKER_HISTOGRAM_DTMF_TO_CANCEL,
    IVR_WORKER_HISTOGRAM_ASR_TO_COMMAND,
    IVR_WORKER_HISTOGRAM_TTS_PROVIDER,
    IVR_WORKER_HISTOGRAM_ASR_PROVIDER,
    IVR_WORKER_HISTOGRAM_COUNT
} ivr_worker_histogram_kind_t;

enum { IVR_WORKER_HISTOGRAM_BUCKET_COUNT = 14 };

typedef struct {
    atomic_uint_least64_t buckets[IVR_WORKER_HISTOGRAM_BUCKET_COUNT];
    atomic_uint_least64_t count;
    atomic_uint_least64_t sum_ms;
} ivr_worker_histogram_t;

typedef struct {
    atomic_uint_least64_t counters[IVR_WORKER_METRIC_COUNT];
    atomic_uint_least64_t gauges[IVR_WORKER_GAUGE_COUNT];
    ivr_worker_histogram_t histograms[IVR_WORKER_HISTOGRAM_COUNT];
} ivr_worker_metrics_t;

void ivr_worker_metrics_init(ivr_worker_metrics_t *metrics);
void ivr_worker_metrics_inc(ivr_worker_metrics_t *metrics,
                            ivr_worker_metric_kind_t kind);
void ivr_worker_metrics_add(ivr_worker_metrics_t *metrics,
                            ivr_worker_metric_kind_t kind, uint64_t value);
uint64_t ivr_worker_metrics_get(const ivr_worker_metrics_t *metrics,
                                ivr_worker_metric_kind_t kind);
void ivr_worker_metrics_set_gauge(ivr_worker_metrics_t *metrics,
                                  ivr_worker_gauge_kind_t kind,
                                  uint64_t value);
void ivr_worker_metrics_set_gauge_max(ivr_worker_metrics_t *metrics,
                                      ivr_worker_gauge_kind_t kind,
                                      uint64_t value);
int ivr_worker_metrics_add_gauge(ivr_worker_metrics_t *metrics,
                                 ivr_worker_gauge_kind_t kind,
                                 uint64_t value, uint64_t *out_value);
int ivr_worker_metrics_sub_gauge(ivr_worker_metrics_t *metrics,
                                 ivr_worker_gauge_kind_t kind,
                                 uint64_t value, uint64_t *out_value);
uint64_t ivr_worker_metrics_get_gauge(const ivr_worker_metrics_t *metrics,
                                      ivr_worker_gauge_kind_t kind);
void ivr_worker_metrics_observe_ms(ivr_worker_metrics_t *metrics,
                                   ivr_worker_histogram_kind_t kind,
                                   uint64_t duration_ms);

/* Render bounded Prometheus text. Only the fixed Prometheus histogram `le`
   label is emitted; no dynamic labels or user identifiers are accepted.
   Health is supplied as a borrowed snapshot. */
int ivr_worker_metrics_render(const ivr_worker_metrics_t *metrics,
                              const ivr_worker_health_snapshot_t *health,
                              char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
