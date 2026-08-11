#ifndef TURBO_MEDIA_IVR_THREAD_H
#define TURBO_MEDIA_IVR_THREAD_H

/**
 * @file ivr_thread.h
 * @brief Minimal internal thread/mutex/cond abstraction (not part of ABI).
 *
 * The IVR worker needs one dedicated, blocking-capable control thread per
 * active session (VoiceXML collect_input blocks). This wrapper keeps platform
 * primitives out of the public headers.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_thread {
    void *handle; /* opaque platform handle; 0 when not started */
} ivr_thread_t;

typedef struct ivr_mutex {
    void *handle;
} ivr_mutex_t;

typedef struct ivr_cond {
    void *handle;
} ivr_cond_t;

/* Thread: fn is invoked with arg on the new thread. */
int ivr_thread_create(ivr_thread_t *t, void *(*fn)(void *), void *arg);
int ivr_thread_join(ivr_thread_t *t);
/* Wait up to timeout_ms for the thread to exit. Returns 0 and closes the
   handle when joined; returns -1 with *timed_out set to 1 when the thread is
   still running (the handle is retained and must be joined later). */
int ivr_thread_timedjoin(ivr_thread_t *t, uint64_t timeout_ms, int *timed_out);
void ivr_thread_sleep_ms(uint64_t ms);

int ivr_mutex_init(ivr_mutex_t *m);
void ivr_mutex_destroy(ivr_mutex_t *m);
void ivr_mutex_lock(ivr_mutex_t *m);
void ivr_mutex_unlock(ivr_mutex_t *m);

int ivr_cond_init(ivr_cond_t *c);
void ivr_cond_destroy(ivr_cond_t *c);
void ivr_cond_wait(ivr_cond_t *c, ivr_mutex_t *m);
/* returns 1 on signal, 0 on timeout */
int ivr_cond_timedwait(ivr_cond_t *c, ivr_mutex_t *m, uint64_t timeout_ms);
void ivr_cond_signal(ivr_cond_t *c);
void ivr_cond_broadcast(ivr_cond_t *c);

/* C11 atomics are already used across the repo (MSVC /experimental:c11atomics). */
#include <stdatomic.h>
typedef atomic_int ivr_atomic_int_t;
typedef atomic_flag ivr_atomic_flag_t;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_THREAD_H */

