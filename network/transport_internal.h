#ifndef TURBO_MEDIA_TRANSPORT_INTERNAL_H
#define TURBO_MEDIA_TRANSPORT_INTERNAL_H

#include "turbo_transport.h"

/* Every opaque transport implementation embeds this as its first member.
 * Generic entry points inspect only this common object before dispatching to
 * the concrete implementation. */
typedef struct turbo_transport_base_s {
    turbo_transport_config_t config;
} turbo_transport_base_t;

static inline turbo_transport_base_t *turbo_transport_base(
    turbo_transport_t *transport) {
    return (turbo_transport_base_t *)(void *)transport;
}

#endif
