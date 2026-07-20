#ifndef TURBO_MEDIA_SIP_ATOMIC_H
#define TURBO_MEDIA_SIP_ATOMIC_H

#include <stdatomic.h>
#include <stdint.h>

typedef _Atomic int32_t sip_atomic_i32_t;

static inline int32_t sip_atomic_increment(sip_atomic_i32_t *value) {
  return atomic_fetch_add_explicit(value, 1, memory_order_relaxed) + 1;
}

static inline int32_t sip_atomic_decrement(sip_atomic_i32_t *value) {
  return atomic_fetch_sub_explicit(value, 1, memory_order_acq_rel) - 1;
}

static inline int32_t sip_atomic_load(const sip_atomic_i32_t *value) {
  return atomic_load_explicit(value, memory_order_acquire);
}

#endif
