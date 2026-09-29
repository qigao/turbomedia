#ifndef WEBRTC_SIGNALING_MANAGEMENT_INTERNAL_H
#define WEBRTC_SIGNALING_MANAGEMENT_INTERNAL_H

/*
 * Management operations are part of the public signaling ABI. Internal
 * management users must consume the canonical exported declarations instead
 * of redeclaring them without TURBO_MEDIA_API, which produces a different
 * linkage contract on Windows.
 */
#include "webrtc_signaling.h"

#endif /* WEBRTC_SIGNALING_MANAGEMENT_INTERNAL_H */
