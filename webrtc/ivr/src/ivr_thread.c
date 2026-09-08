#include "ivr_thread.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <errno.h>
#include <time.h>
#endif

#include <stdlib.h>

typedef struct {
    void *(*fn)(void *);
    void *arg;
} ivr_thread_start_t;

#if defined(_WIN32)
static unsigned __stdcall ivr_thread_entry(void *opaque) {
    ivr_thread_start_t *start = (ivr_thread_start_t *)opaque;
    start->fn(start->arg);
    free(start);
    return 0;
}
#else
static void *ivr_thread_entry(void *opaque) {
    ivr_thread_start_t *start = (ivr_thread_start_t *)opaque;
    start->fn(start->arg);
    free(start);
    return NULL;
}
#endif

int ivr_thread_create(ivr_thread_t *t, void *(*fn)(void *), void *arg) {
    if (!t || !fn) {
        return -1;
    }
    ivr_thread_start_t *start = (ivr_thread_start_t *)malloc(sizeof(*start));
    if (!start) {
        return -1;
    }
    start->fn = fn;
    start->arg = arg;
#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0, ivr_thread_entry, start, 0, NULL);
    if (h == 0) {
        free(start);
        t->handle = NULL;
        return -1;
    }
    t->handle = (void *)h;
#else
    pthread_t th;
    if (pthread_create(&th, NULL, ivr_thread_entry, start) != 0) {
        free(start);
        t->handle = NULL;
        return -1;
    }
    t->handle = (void *)th;
#endif
    return 0;
}

int ivr_thread_join(ivr_thread_t *t) {
    int status;
    if (!t || !t->handle) {
        return -1;
    }
#if defined(_WIN32)
    if (WaitForSingleObject((HANDLE)t->handle, INFINITE) != WAIT_OBJECT_0 ||
        !CloseHandle((HANDLE)t->handle)) {
        return -1;
    }
    status = 0;
#else
    status = pthread_join((pthread_t)t->handle, NULL);
#endif
    if (status != 0) return -1;
    t->handle = NULL;
    return 0;
}

int ivr_thread_timedjoin(ivr_thread_t *t, uint64_t timeout_ms, int *timed_out) {
    if (!t || !t->handle || !timed_out) {
        return -1;
    }
    *timed_out = 0;
#if defined(_WIN32)
    DWORD ms = (timeout_ms > (uint64_t)INFINITE - 1) ? INFINITE - 1
                                                     : (DWORD)timeout_ms;
    DWORD rc = WaitForSingleObject((HANDLE)t->handle, ms);
    if (rc == WAIT_OBJECT_0) {
        if (!CloseHandle((HANDLE)t->handle)) return -1;
        t->handle = NULL;
        return 0;
    }
    *timed_out = 1; /* still running; handle retained */
    return -1;
#elif defined(__linux__) && defined(__GLIBC__)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_timedjoin_np((pthread_t)t->handle, NULL, &ts);
    if (rc == 0) {
        t->handle = NULL;
        return 0;
    }
    *timed_out = 1;
    return -1;
#else
    /* no portable timed join: fall back to blocking join (deadline not
       enforced on this platform) */
    return ivr_thread_join(t);
#endif
}

void ivr_thread_sleep_ms(uint64_t ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int ivr_mutex_init(ivr_mutex_t *m) {
    if (!m) {
        return -1;
    }
#if defined(_WIN32)
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) {
        return -1;
    }
    InitializeCriticalSection(cs);
    m->handle = cs;
#else
    pthread_mutex_t *mu = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!mu) {
        return -1;
    }
    if (pthread_mutex_init(mu, NULL) != 0) {
        free(mu);
        return -1;
    }
    m->handle = mu;
#endif
    return 0;
}

void ivr_mutex_destroy(ivr_mutex_t *m) {
    if (!m || !m->handle) {
        return;
    }
#if defined(_WIN32)
    DeleteCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_destroy((pthread_mutex_t *)m->handle);
#endif
    free(m->handle);
    m->handle = NULL;
}

void ivr_mutex_lock(ivr_mutex_t *m) {
#if defined(_WIN32)
    EnterCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_lock((pthread_mutex_t *)m->handle);
#endif
}

void ivr_mutex_unlock(ivr_mutex_t *m) {
#if defined(_WIN32)
    LeaveCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_unlock((pthread_mutex_t *)m->handle);
#endif
}

int ivr_cond_init(ivr_cond_t *c) {
    if (!c) {
        return -1;
    }
#if defined(_WIN32)
    CONDITION_VARIABLE *cv = (CONDITION_VARIABLE *)malloc(sizeof(CONDITION_VARIABLE));
    if (!cv) {
        return -1;
    }
    InitializeConditionVariable(cv);
    c->handle = cv;
#else
    pthread_cond_t *pc = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    if (!pc) {
        return -1;
    }
    if (pthread_cond_init(pc, NULL) != 0) {
        free(pc);
        return -1;
    }
    c->handle = pc;
#endif
    return 0;
}

void ivr_cond_destroy(ivr_cond_t *c) {
    if (!c || !c->handle) {
        return;
    }
#if defined(_WIN32)
    /* CONDITION_VARIABLE needs no teardown */
#else
    pthread_cond_destroy((pthread_cond_t *)c->handle);
#endif
    free(c->handle);
    c->handle = NULL;
}

void ivr_cond_wait(ivr_cond_t *c, ivr_mutex_t *m) {
#if defined(_WIN32)
    SleepConditionVariableCS((CONDITION_VARIABLE *)c->handle,
                             (CRITICAL_SECTION *)m->handle, INFINITE);
#else
    pthread_cond_wait((pthread_cond_t *)c->handle, (pthread_mutex_t *)m->handle);
#endif
}

int ivr_cond_timedwait(ivr_cond_t *c, ivr_mutex_t *m, uint64_t timeout_ms) {
#if defined(_WIN32)
    DWORD ms = (timeout_ms > (uint64_t)INFINITE - 1) ? INFINITE - 1 : (DWORD)timeout_ms;
    BOOL ok = SleepConditionVariableCS((CONDITION_VARIABLE *)c->handle,
                                       (CRITICAL_SECTION *)m->handle, ms);
    return ok ? 1 : 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait((pthread_cond_t *)c->handle,
                                    (pthread_mutex_t *)m->handle, &ts);
    return rc == 0 ? 1 : 0;
#endif
}

void ivr_cond_signal(ivr_cond_t *c) {
#if defined(_WIN32)
    WakeConditionVariable((CONDITION_VARIABLE *)c->handle);
#else
    pthread_cond_signal((pthread_cond_t *)c->handle);
#endif
}

void ivr_cond_broadcast(ivr_cond_t *c) {
#if defined(_WIN32)
    WakeAllConditionVariable((CONDITION_VARIABLE *)c->handle);
#else
    pthread_cond_broadcast((pthread_cond_t *)c->handle);
#endif
}
