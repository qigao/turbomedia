#include "ivr_media_reconnect.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct ivr_media_reconnect_s {
    ivr_media_reconnect_config_t config;
    ivr_media_reconnect_snapshot_t state;
};

static uint64_t add_saturating(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t retry_backoff(const ivr_media_reconnect_t *reconnect) {
    uint64_t delay = reconnect->config.initial_backoff_ms;
    uint32_t shifts = reconnect->state.attempts_started > 1
                          ? reconnect->state.attempts_started - 1u
                          : 0u;
    while (shifts-- > 0 && delay < reconnect->config.max_backoff_ms) {
        delay = delay > UINT64_MAX / 2u ? UINT64_MAX : delay * 2u;
    }
    return delay > reconnect->config.max_backoff_ms
               ? reconnect->config.max_backoff_ms
               : delay;
}

ivr_status_t ivr_media_reconnect_create(
    const ivr_media_reconnect_config_t *config,
    ivr_media_reconnect_t **out_reconnect) {
    ivr_media_reconnect_t *reconnect;
    if (!out_reconnect) {
        return IVR_EINVAL;
    }
    reconnect = (ivr_media_reconnect_t *)calloc(1, sizeof(*reconnect));
    if (!reconnect) {
        return IVR_ENOSPC;
    }
    reconnect->config.max_attempts = config && config->max_attempts
                                         ? config->max_attempts
                                         : 3u;
    reconnect->config.initial_backoff_ms =
        config && config->initial_backoff_ms ? config->initial_backoff_ms : 100u;
    reconnect->config.max_backoff_ms =
        config && config->max_backoff_ms ? config->max_backoff_ms : 2000u;
    reconnect->config.total_deadline_ms =
        config && config->total_deadline_ms ? config->total_deadline_ms : 10000u;
    if (reconnect->config.max_attempts > 32u ||
        reconnect->config.initial_backoff_ms > reconnect->config.max_backoff_ms ||
        reconnect->config.total_deadline_ms < reconnect->config.initial_backoff_ms) {
        free(reconnect);
        return IVR_EINVAL;
    }
    *out_reconnect = reconnect;
    return IVR_OK;
}

void ivr_media_reconnect_destroy(ivr_media_reconnect_t *reconnect) {
    free(reconnect);
}

ivr_status_t ivr_media_reconnect_start(ivr_media_reconnect_t *reconnect,
                                       uint64_t now_ms,
                                       uint64_t *out_generation) {
    if (!reconnect || !out_generation) {
        return IVR_EINVAL;
    }
    memset(&reconnect->state, 0, sizeof(reconnect->state));
    reconnect->state.attempt_generation = 1;
    reconnect->state.attempts_started = 1;
    reconnect->state.deadline_at_ms =
        add_saturating(now_ms, reconnect->config.total_deadline_ms);
    *out_generation = reconnect->state.attempt_generation;
    return IVR_OK;
}

static ivr_status_t schedule_retry(ivr_media_reconnect_t *reconnect,
                                   uint64_t now_ms,
                                   ivr_media_reconnect_event_t *out_event) {
    int first_failure_in_episode = reconnect->state.attempts_started == 1u;
    if (reconnect->state.attempts_started >= reconnect->config.max_attempts ||
        now_ms >= reconnect->state.deadline_at_ms) {
        reconnect->state.exhausted = 1;
        reconnect->state.retry_pending = 0;
        *out_event = IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED;
        return IVR_OK;
    }
    reconnect->state.retry_pending = 1;
    reconnect->state.next_retry_at_ms =
        add_saturating(now_ms, retry_backoff(reconnect));
    if (reconnect->state.next_retry_at_ms > reconnect->state.deadline_at_ms) {
        reconnect->state.next_retry_at_ms = reconnect->state.deadline_at_ms;
    }
    /* A recovery episode is one business outage even when several transport
       attempts fail. Only its first failure publishes DISCONNECTED. */
    *out_event = first_failure_in_episode
                     ? IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED
                     : IVR_MEDIA_RECONNECT_EVENT_NONE;
    return IVR_OK;
}

static void begin_stable_connection_recovery(
    ivr_media_reconnect_t *reconnect, uint64_t now_ms) {
    if (!reconnect || reconnect->state.retry_pending ||
        !reconnect->state.whip_connected ||
        !reconnect->state.whep_connected) {
        return;
    }

    /* The deadline bounds one recovery episode, not the lifetime of a
       healthy dialog. A fault after a long stable period receives the same
       bounded retry budget as a fault immediately after setup. */
    reconnect->state.attempts_started = 1u;
    reconnect->state.next_retry_at_ms = 0u;
    reconnect->state.deadline_at_ms =
        add_saturating(now_ms, reconnect->config.total_deadline_ms);
}

ivr_status_t ivr_media_reconnect_on_state(
    ivr_media_reconnect_t *reconnect, ivr_media_link_kind_t link,
    uint64_t attempt_generation, ivr_media_link_state_t state,
    int error_code, uint64_t now_ms,
    ivr_media_reconnect_event_t *out_event) {
    int was_connected;
    if (!reconnect || !out_event ||
        (link != IVR_MEDIA_LINK_WHIP && link != IVR_MEDIA_LINK_WHEP)) {
        return IVR_EINVAL;
    }
    *out_event = IVR_MEDIA_RECONNECT_EVENT_NONE;
    if (attempt_generation != reconnect->state.attempt_generation) {
        reconnect->state.stale_callbacks++;
        return IVR_ESTATE;
    }
    if (reconnect->state.exhausted) {
        return IVR_ESTATE;
    }
    was_connected = reconnect->state.whip_connected &&
                    reconnect->state.whep_connected;
    if (link == IVR_MEDIA_LINK_WHEP &&
        error_code == IVR_MEDIA_ERROR_INPUT_STALLED) {
        reconnect->state.input_stalls++;
        if (was_connected) {
            begin_stable_connection_recovery(reconnect, now_ms);
        }
        if (!reconnect->state.retry_pending) {
            ivr_media_reconnect_event_t ignored;
            (void)schedule_retry(reconnect, now_ms, &ignored);
        }
        *out_event = IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED;
        return IVR_OK;
    }
    if (state == IVR_MEDIA_LINK_CONNECTED) {
        if (link == IVR_MEDIA_LINK_WHIP) {
            reconnect->state.whip_connected = 1;
        } else {
            reconnect->state.whep_connected = 1;
        }
        if (!was_connected && reconnect->state.whip_connected &&
            reconnect->state.whep_connected) {
            reconnect->state.retry_pending = 0;
            *out_event = IVR_MEDIA_RECONNECT_EVENT_RECONNECTED;
        }
        return IVR_OK;
    }
    if (state == IVR_MEDIA_LINK_DISCONNECTED ||
        state == IVR_MEDIA_LINK_FAILED || state == IVR_MEDIA_LINK_CLOSED) {
        if (was_connected) {
            begin_stable_connection_recovery(reconnect, now_ms);
        }
        if (link == IVR_MEDIA_LINK_WHIP) {
            reconnect->state.whip_connected = 0;
        } else {
            reconnect->state.whep_connected = 0;
        }
        if (reconnect->state.retry_pending) {
            return IVR_OK;
        }
        return schedule_retry(reconnect, now_ms, out_event);
    }
    return IVR_OK;
}

ivr_status_t ivr_media_reconnect_poll(
    ivr_media_reconnect_t *reconnect, uint64_t now_ms,
    uint64_t *out_generation, ivr_media_reconnect_event_t *out_event) {
    if (!reconnect || !out_generation || !out_event) {
        return IVR_EINVAL;
    }
    *out_event = IVR_MEDIA_RECONNECT_EVENT_NONE;
    *out_generation = reconnect->state.attempt_generation;
    if (reconnect->state.exhausted) {
        return IVR_ESTATE;
    }
    if (reconnect->state.retry_pending &&
        now_ms >= reconnect->state.next_retry_at_ms) {
        if (now_ms >= reconnect->state.deadline_at_ms ||
            reconnect->state.attempts_started >= reconnect->config.max_attempts) {
            reconnect->state.exhausted = 1;
            reconnect->state.retry_pending = 0;
            *out_event = IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED;
            return IVR_OK;
        }
        reconnect->state.retry_pending = 0;
        reconnect->state.whip_connected = 0;
        reconnect->state.whep_connected = 0;
        reconnect->state.attempts_started++;
        reconnect->state.attempt_generation++;
        *out_generation = reconnect->state.attempt_generation;
        *out_event = IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE;
    }
    return IVR_OK;
}

ivr_status_t ivr_media_reconnect_snapshot(
    const ivr_media_reconnect_t *reconnect,
    ivr_media_reconnect_snapshot_t *out_snapshot) {
    if (!reconnect || !out_snapshot) {
        return IVR_EINVAL;
    }
    *out_snapshot = reconnect->state;
    return IVR_OK;
}
