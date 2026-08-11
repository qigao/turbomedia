#ifndef TURBO_MEDIA_IVR_MEDIA_STATE_H
#define TURBO_MEDIA_IVR_MEDIA_STATE_H

#include "ivr/ivr_worker.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IVR_MEDIA_LINK_CONNECTING = 1,
    IVR_MEDIA_LINK_CONNECTED = 2,
    IVR_MEDIA_LINK_DISCONNECTED = 3,
    IVR_MEDIA_LINK_FAILED = 4,
    IVR_MEDIA_LINK_CLOSED = 5
} ivr_media_link_state_t;

enum {
    IVR_MEDIA_ERROR_NONE = 0,
    IVR_MEDIA_ERROR_CONNECT_TIMEOUT = 1001,
    IVR_MEDIA_ERROR_INPUT_STALLED = 1002,
    IVR_MEDIA_ERROR_PEER_FAILED = 1003,
    IVR_MEDIA_ERROR_STOPPED = 1004
};

typedef void (*ivr_media_state_fn)(void *context,
                                   const ivr_call_ref_t *call,
                                   uint64_t attempt_generation,
                                   ivr_media_link_state_t state,
                                   int error_code);

#ifdef __cplusplus
}
#endif

#endif
