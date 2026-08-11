#include "ivr_dtmf_rtp.h"

#include "ivr_thread.h"

#include <stdlib.h>
#include <string.h>

#define IVR_DTMF_DEFAULT_WINDOWS 64u
#define IVR_DTMF_DEFAULT_MAX_DURATION 48000u

typedef struct {
    int valid;
    int active;
    char room_id[IVR_DTMF_ID_MAX];
    char call_id[IVR_DTMF_ID_MAX];
    char input_id[IVR_DTMF_ID_MAX];
    uint64_t call_generation;
    uint64_t input_generation;
    uint64_t last_source_generation;
    uint32_t last_rtp_timestamp;
    uint8_t last_event;
    int has_final;
} ivr_dtmf_window_t;

struct ivr_dtmf_ingress_s {
    ivr_dtmf_window_t *windows;
    uint32_t capacity;
    uint16_t max_duration;
    ivr_mutex_t lock;
    int lock_initialized;
};

static int copy_view(char *out, size_t cap, const ivr_bytes_view_t *view) {
    if (!out || !view || !view->data || view->size == 0 || view->size >= cap) {
        return -1;
    }
    memcpy(out, view->data, view->size);
    out[view->size] = '\0';
    return 0;
}

static int call_matches(const ivr_dtmf_window_t *window,
                        const ivr_call_ref_t *call) {
    return window->valid && call &&
           window->call_generation == call->call_generation &&
           strlen(window->room_id) == call->room_id.size &&
           memcmp(window->room_id, call->room_id.data, call->room_id.size) == 0 &&
           strlen(window->call_id) == call->call_id.size &&
           memcmp(window->call_id, call->call_id.data, call->call_id.size) == 0;
}

static ivr_dtmf_window_t *find_window(ivr_dtmf_ingress_t *ingress,
                                      const ivr_call_ref_t *call) {
    uint32_t i;
    for (i = 0; i < ingress->capacity; ++i) {
        if (call_matches(&ingress->windows[i], call)) {
            return &ingress->windows[i];
        }
    }
    return NULL;
}

static char event_digit(uint8_t event) {
    static const char digits[] = "0123456789*#ABCD";
    return event < sizeof(digits) - 1u ? digits[event] : '\0';
}

ivr_status_t ivr_dtmf_ingress_create(
    const ivr_dtmf_ingress_config_t *config,
    ivr_dtmf_ingress_t **out_ingress) {
    ivr_dtmf_ingress_t *ingress;
    uint32_t capacity;
    if (!out_ingress) {
        return IVR_EINVAL;
    }
    capacity = config && config->window_capacity
                   ? config->window_capacity
                   : IVR_DTMF_DEFAULT_WINDOWS;
    if (capacity == 0 || capacity > 4096u) {
        return IVR_EINVAL;
    }
    ingress = (ivr_dtmf_ingress_t *)calloc(1, sizeof(*ingress));
    if (!ingress) {
        return IVR_ENOSPC;
    }
    ingress->windows = (ivr_dtmf_window_t *)calloc(capacity,
                                                    sizeof(*ingress->windows));
    if (!ingress->windows) {
        free(ingress);
        return IVR_ENOSPC;
    }
    ingress->capacity = capacity;
    ingress->max_duration = config && config->max_duration
                                ? config->max_duration
                                : IVR_DTMF_DEFAULT_MAX_DURATION;
    if (ivr_mutex_init(&ingress->lock) != 0) {
        free(ingress->windows);
        free(ingress);
        return IVR_ESTATE;
    }
    ingress->lock_initialized = 1;
    *out_ingress = ingress;
    return IVR_OK;
}

void ivr_dtmf_ingress_destroy(ivr_dtmf_ingress_t *ingress) {
    if (!ingress) {
        return;
    }
    if (ingress->lock_initialized) {
        ivr_mutex_destroy(&ingress->lock);
        ingress->lock_initialized = 0;
    }
    free(ingress->windows);
    free(ingress);
}

ivr_status_t ivr_dtmf_ingress_begin_input(
    ivr_dtmf_ingress_t *ingress, const ivr_call_ref_t *call,
    const char *input_id, uint64_t input_generation) {
    ivr_dtmf_window_t *window;
    uint32_t i;
    if (!ingress || !call || !input_id || !input_id[0] ||
        strlen(input_id) >= IVR_DTMF_ID_MAX || input_generation == 0) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&ingress->lock);
    window = find_window(ingress, call);
    if (!window) {
        for (i = 0; i < ingress->capacity; ++i) {
            if (!ingress->windows[i].valid || !ingress->windows[i].active) {
                window = &ingress->windows[i];
                break;
            }
        }
    }
    if (!window) {
        ivr_mutex_unlock(&ingress->lock);
        return IVR_ENOSPC;
    }
    memset(window, 0, sizeof(*window));
    if (copy_view(window->room_id, sizeof(window->room_id), &call->room_id) != 0 ||
        copy_view(window->call_id, sizeof(window->call_id), &call->call_id) != 0) {
        ivr_mutex_unlock(&ingress->lock);
        return IVR_EINVAL;
    }
    memcpy(window->input_id, input_id, strlen(input_id) + 1u);
    window->call_generation = call->call_generation;
    window->input_generation = input_generation;
    window->valid = 1;
    window->active = 1;
    ivr_mutex_unlock(&ingress->lock);
    return IVR_OK;
}

ivr_status_t ivr_dtmf_ingress_end_input(
    ivr_dtmf_ingress_t *ingress, const ivr_call_ref_t *call,
    uint64_t input_generation) {
    ivr_dtmf_window_t *window;
    if (!ingress || !call || input_generation == 0) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&ingress->lock);
    window = find_window(ingress, call);
    if (!window || !window->active ||
        window->input_generation != input_generation) {
        ivr_mutex_unlock(&ingress->lock);
        return IVR_ESTATE;
    }
    window->active = 0;
    ivr_mutex_unlock(&ingress->lock);
    return IVR_OK;
}

ivr_status_t ivr_dtmf_ingress_submit_rtp(
    ivr_dtmf_ingress_t *ingress, const ivr_call_ref_t *call,
    uint64_t source_generation, uint32_t rtp_timestamp,
    const uint8_t *payload, size_t payload_length,
    ivr_dtmf_input_t *out_input) {
    ivr_dtmf_window_t *window;
    uint8_t event;
    uint16_t duration;
    char digit;
    if (!ingress || !call || source_generation == 0 || !payload ||
        payload_length != 4u || !out_input) {
        return IVR_EINVAL;
    }
    event = payload[0];
    duration = (uint16_t)(((uint16_t)payload[2] << 8u) | payload[3]);
    digit = event_digit(event);
    if (!digit || (payload[1] & 0x40u) != 0 || duration == 0 ||
        duration > ingress->max_duration) {
        return IVR_EINVAL;
    }
    if ((payload[1] & 0x80u) == 0) {
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&ingress->lock);
    window = find_window(ingress, call);
    if (!window || !window->active) {
        ivr_mutex_unlock(&ingress->lock);
        return IVR_ESTATE;
    }
    if (window->has_final &&
        window->last_source_generation == source_generation &&
        window->last_rtp_timestamp == rtp_timestamp &&
        window->last_event == event) {
        ivr_mutex_unlock(&ingress->lock);
        return IVR_ESTATE;
    }
    memset(out_input, 0, sizeof(*out_input));
    memcpy(out_input->room_id, window->room_id, strlen(window->room_id) + 1u);
    memcpy(out_input->call_id, window->call_id, strlen(window->call_id) + 1u);
    memcpy(out_input->input_id, window->input_id, strlen(window->input_id) + 1u);
    out_input->call_generation = window->call_generation;
    out_input->input_generation = window->input_generation;
    out_input->source_generation = source_generation;
    out_input->rtp_timestamp = rtp_timestamp;
    out_input->digit = digit;
    out_input->duration = duration;
    out_input->ended = 1;
    window->last_source_generation = source_generation;
    window->last_rtp_timestamp = rtp_timestamp;
    window->last_event = event;
    window->has_final = 1;
    ivr_mutex_unlock(&ingress->lock);
    return IVR_OK;
}
