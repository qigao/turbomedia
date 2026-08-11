#include "ivr/ivr_worker.h"
#include "ivr_content.h"
#include "ivr_session.h"
#include "ivr_thread.h"
#include "ivr_turboxml_adapter.h"
#include <stdlib.h>
#include <string.h>

typedef struct ivr_worker_slot {
    int used;
    int cleanup_owner;
    struct ivr_worker_package *package;
    void *media_instance;
    ivr_session_t session;
} ivr_worker_slot_t;

enum {
    IVR_SLOT_CLEANUP_NONE = 0,
    IVR_SLOT_CLEANUP_SESSION = 1,
    IVR_SLOT_CLEANUP_DRAIN = 2
};

typedef struct ivr_worker_package {
    ivr_str_t name;
    ivr_content_package_t pkg;
    uint32_t active_sessions;
} ivr_worker_package_t;

struct ivr_worker_s {
    /* owned config */
    ivr_str_t worker_id;
    ivr_str_t content_root;
    uint32_t max_sessions_per_worker;
    uint32_t session_inbox_capacity;
    size_t max_event_bytes;
    size_t max_command_bytes;
    size_t max_inbox_bytes;
    uint64_t drain_deadline_ms;

    ivr_command_gateway_ops_t gateway;
    ivr_media_port_factory_ops_t media_factory;
    ivr_session_observer_ops_t observer;

    ivr_mutex_t lock;
    int lock_initialized;
    ivr_cond_t drain_cond;
    int drain_cond_initialized;
    int drain_in_progress;
    ivr_atomic_int_t started;
    ivr_atomic_int_t draining;
    uint64_t drain_timed_out; /* sessions that exceeded the drain deadline */
    uint64_t events_routed;
    uint64_t events_unrouted;

    ivr_worker_slot_t *slots;
    uint32_t slot_count;

    ivr_worker_package_t *packages;
    uint32_t package_count;
    uint32_t package_capacity;
};

static ivr_worker_package_t *worker_find_package(ivr_worker_t *w,
                                                 const char *name) {
    for (uint32_t i = 0; i < w->package_count; i++) {
        if (strcmp(w->packages[i].name.data, name) == 0) {
            return &w->packages[i];
        }
    }
    return NULL;
}

static int worker_load_package(ivr_worker_t *w, const char *name,
                               ivr_worker_package_t **out) {
    ivr_worker_package_t *found = worker_find_package(w, name);
    if (found) {
        *out = found;
        return IVR_OK;
    }
    if (w->package_count >= w->max_sessions_per_worker) {
        /* Reuse an inactive package slot instead of allowing the cache to
           grow with the number of distinct calls/packages ever observed. */
        for (uint32_t i = 0; i < w->package_count; i++) {
            if (w->packages[i].active_sessions == 0) {
                ivr_worker_package_t replacement;
                int rc;

                memset(&replacement, 0, sizeof(replacement));
                ivr_str_init(&replacement.name);
                rc = ivr_content_package_load(w->content_root.data, name,
                                              &replacement.pkg);
                if (rc != IVR_OK) return rc;
                if (ivr_str_assign(&replacement.name, name, strlen(name)) < 0) {
                    ivr_content_package_free(&replacement.pkg);
                    ivr_str_free(&replacement.name);
                    return IVR_ENOSPC;
                }

                ivr_content_package_free(&w->packages[i].pkg);
                ivr_str_free(&w->packages[i].name);
                w->packages[i] = replacement;
                *out = &w->packages[i];
                return IVR_OK;
            }
        }
        return IVR_ENOSPC;
    }
    if (w->package_count == w->package_capacity) {
        uint32_t next_cap = w->package_capacity ? w->package_capacity * 2 : 4;
        if (next_cap > w->max_sessions_per_worker) {
            next_cap = w->max_sessions_per_worker;
        }
        ivr_worker_package_t *next = (ivr_worker_package_t *)realloc(
            w->packages, next_cap * sizeof(*next));
        if (!next) {
            return IVR_ENOSPC;
        }
        w->packages = next;
        w->package_capacity = next_cap;
    }
    ivr_worker_package_t *pkg = &w->packages[w->package_count];
    memset(pkg, 0, sizeof(*pkg));
    ivr_str_init(&pkg->name);
    int rc = ivr_content_package_load(w->content_root.data, name, &pkg->pkg);
    if (rc != IVR_OK) {
        return rc;
    }
    if (ivr_str_assign(&pkg->name, name, strlen(name)) < 0) {
        ivr_content_package_free(&pkg->pkg);
        return IVR_ENOSPC;
    }
    w->package_count++;
    *out = pkg;
    return IVR_OK;
}

ivr_status_t ivr_worker_create(const ivr_worker_config_t *config,
                               const ivr_command_gateway_ops_t *gateway,
                               const ivr_media_port_factory_ops_t *media_factory,
                               ivr_worker_t **out_worker) {
    if (!config || !gateway || !gateway->submit_copy || !media_factory ||
        !media_factory->create || !media_factory->destroy || !out_worker) {
        return IVR_EINVAL;
    }
    if (config->abi_version != IVR_WORKER_ABI_VERSION ||
        media_factory->abi_version != IVR_WORKER_ABI_VERSION) {
        return IVR_EVERSION;
    }
    if (config->observer.on_latency &&
        config->observer.abi_version != IVR_SESSION_OBSERVER_ABI_VERSION) {
        return IVR_EVERSION;
    }
    if (config->max_sessions_per_worker == 0 ||
        config->session_inbox_capacity == 0 ||
        !config->worker_id || !config->content_root) {
        return IVR_EINVAL;
    }
    ivr_worker_t *w = (ivr_worker_t *)calloc(1, sizeof(*w));
    if (!w) {
        return IVR_ENOSPC;
    }
    ivr_str_init(&w->worker_id);
    ivr_str_init(&w->content_root);
    if (ivr_str_assign(&w->worker_id, config->worker_id,
                       strlen(config->worker_id)) < 0 ||
        ivr_str_assign(&w->content_root, config->content_root,
                       strlen(config->content_root)) < 0) {
        ivr_worker_destroy(w);
        return IVR_ENOSPC;
    }
    w->max_sessions_per_worker = config->max_sessions_per_worker;
    w->session_inbox_capacity = config->session_inbox_capacity;
    w->max_event_bytes = config->max_event_bytes
                             ? config->max_event_bytes
                             : 64 * 1024;
    w->max_command_bytes = config->max_command_bytes
                               ? config->max_command_bytes
                               : 16 * 1024;
    w->max_inbox_bytes = config->max_inbox_bytes
                             ? config->max_inbox_bytes
                             : w->max_event_bytes * w->session_inbox_capacity;
    w->drain_deadline_ms = config->drain_deadline_ms;
    w->gateway = *gateway;
    w->media_factory = *media_factory;
    w->observer = config->observer;
    w->slot_count = config->max_sessions_per_worker;
    w->slots = (ivr_worker_slot_t *)calloc(w->slot_count, sizeof(*w->slots));
    if (!w->slots) {
        ivr_worker_destroy(w);
        return IVR_ENOSPC;
    }
    if (ivr_mutex_init(&w->lock) < 0) {
        ivr_worker_destroy(w);
        return IVR_ENOSPC;
    }
    w->lock_initialized = 1;
    if (ivr_cond_init(&w->drain_cond) < 0) {
        ivr_worker_destroy(w);
        return IVR_ENOSPC;
    }
    w->drain_cond_initialized = 1;
    *out_worker = w;
    return IVR_OK;
}

ivr_status_t ivr_worker_start(ivr_worker_t *worker) {
    if (!worker) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    if (atomic_load((atomic_int *)&worker->started) ||
        atomic_load((atomic_int *)&worker->draining)) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    atomic_store((atomic_int *)&worker->started, 1);
    ivr_mutex_unlock(&worker->lock);
    return IVR_OK;
}

ivr_status_t ivr_worker_assign_session(ivr_worker_t *worker,
                                       const ivr_call_ref_t *call,
                                       const char *content_package,
                                       ivr_session_t **out_session) {
    if (!worker || !call || !content_package || !out_session) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    if (!atomic_load((atomic_int *)&worker->started) ||
        atomic_load((atomic_int *)&worker->draining)) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ECLOSED;
    }
    ivr_worker_slot_t *slot = NULL;
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (!worker->slots[i].used) {
            slot = &worker->slots[i];
            break;
        }
    }
    if (!slot) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ENOSPC; /* at capacity: explicit rejection */
    }
    ivr_worker_package_t *pkg = NULL;
    ivr_media_port_ops_t media;
    void *media_instance = NULL;
    int rc = worker_load_package(worker, content_package, &pkg);
    if (rc != IVR_OK) {
        ivr_mutex_unlock(&worker->lock);
        return (ivr_status_t)rc;
    }
    memset(&media, 0, sizeof(media));
    rc = worker->media_factory.create(worker->media_factory.context, call,
                                      &media, &media_instance);
    if (rc != IVR_OK) {
        ivr_mutex_unlock(&worker->lock);
        return (ivr_status_t)rc;
    }
    if (media.abi_version != IVR_WORKER_ABI_VERSION) {
        worker->media_factory.destroy(worker->media_factory.context,
                                      media_instance);
        ivr_mutex_unlock(&worker->lock);
        return IVR_EVERSION;
    }
    if (!media.start_bot || !media.play_pcm || !media.cancel_input ||
        !media.stop_bot || !media.begin_input || !media.end_input) {
        worker->media_factory.destroy(worker->media_factory.context,
                                      media_instance);
        ivr_mutex_unlock(&worker->lock);
        return IVR_EINVAL;
    }
    rc = ivr_session_create(&slot->session, call,
                            worker->session_inbox_capacity,
                            worker->max_event_bytes, worker->max_inbox_bytes,
                            &ivr_turboxml_engine_ops, &pkg->pkg,
                            &worker->gateway, &media);
    if (rc != IVR_OK) {
        worker->media_factory.destroy(worker->media_factory.context,
                                      media_instance);
        ivr_mutex_unlock(&worker->lock);
        return (ivr_status_t)rc;
    }
    slot->session.observer = worker->observer;
    slot->session.owner_worker = worker;
    rc = ivr_session_start(&slot->session);
    if (rc != IVR_OK) {
        ivr_session_free(&slot->session);
        worker->media_factory.destroy(worker->media_factory.context,
                                      media_instance);
        ivr_mutex_unlock(&worker->lock);
        return (ivr_status_t)rc;
    }
    slot->used = 1;
    slot->cleanup_owner = IVR_SLOT_CLEANUP_NONE;
    slot->package = pkg;
    slot->media_instance = media_instance;
    pkg->active_sessions++;
    *out_session = &slot->session;
    ivr_mutex_unlock(&worker->lock);
    return IVR_OK;
}

static int ivr_worker_call_matches(const ivr_session_t *session,
                                      const ivr_event_view_t *event) {
    return session->call_generation == event->call.call_generation &&
           session->room_id.size == event->call.room_id.size &&
           (session->room_id.size == 0 ||
            memcmp(session->room_id.data, event->call.room_id.data,
                   session->room_id.size) == 0) &&
           session->call_id.size == event->call.call_id.size &&
           (session->call_id.size == 0 ||
            memcmp(session->call_id.data, event->call.call_id.data,
                   session->call_id.size) == 0);
}

ivr_status_t ivr_worker_submit_event_copy(ivr_worker_t *worker,
                                          const ivr_event_view_t *event) {
    if (!worker || !event) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    ivr_session_t *match = NULL;
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (worker->slots[i].used &&
            ivr_worker_call_matches(&worker->slots[i].session, event)) {
            match = &worker->slots[i].session;
            break;
        }
    }
    if (!match) {
        worker->events_unrouted++;
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    ivr_status_t rc = ivr_session_submit_event_copy(match, event);
    if (rc == IVR_OK) {
        worker->events_routed++;
    }
    ivr_mutex_unlock(&worker->lock);
    return rc;
}

void ivr_worker_release_session(ivr_worker_t *worker, ivr_session_t *session) {
    ivr_worker_slot_t *slot = NULL;
    void *media_instance = NULL;

    if (!worker || !session || !worker->lock_initialized) {
        return;
    }

    ivr_mutex_lock(&worker->lock);
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (worker->slots[i].used && &worker->slots[i].session == session) {
            slot = &worker->slots[i];
            break;
        }
    }
    if (!slot || slot->cleanup_owner != IVR_SLOT_CLEANUP_NONE) {
        ivr_mutex_unlock(&worker->lock);
        return;
    }
    slot->cleanup_owner = IVR_SLOT_CLEANUP_SESSION;
    atomic_store((atomic_int *)&session->draining, 1);
    ivr_session_request_terminal(session);
    ivr_mutex_unlock(&worker->lock);

    ivr_session_join(session);

    ivr_mutex_lock(&worker->lock);
    if (slot->used && slot->cleanup_owner == IVR_SLOT_CLEANUP_SESSION) {
        ivr_session_free(&slot->session);
        media_instance = slot->media_instance;
        slot->media_instance = NULL;
        if (slot->package && slot->package->active_sessions > 0) {
            slot->package->active_sessions--;
        }
        slot->package = NULL;
        slot->used = 0;
        slot->cleanup_owner = IVR_SLOT_CLEANUP_NONE;
        if (worker->drain_cond_initialized) {
            ivr_cond_broadcast(&worker->drain_cond);
        }
    }
    ivr_mutex_unlock(&worker->lock);
    if (media_instance) {
        worker->media_factory.destroy(worker->media_factory.context,
                                      media_instance);
    }
}

int ivr_worker_has_session(const ivr_worker_t *worker,
                           const ivr_call_ref_t *call) {
    if (!worker || !call) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&worker->lock);
    int found = 0;
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (worker->slots[i].used &&
            worker->slots[i].session.call_generation == call->call_generation &&
            worker->slots[i].session.room_id.size == call->room_id.size &&
            (call->room_id.size == 0 ||
             memcmp(worker->slots[i].session.room_id.data, call->room_id.data,
                    call->room_id.size) == 0) &&
            worker->slots[i].session.call_id.size == call->call_id.size &&
            (call->call_id.size == 0 ||
             memcmp(worker->slots[i].session.call_id.data, call->call_id.data,
                    call->call_id.size) == 0)) {
            found = 1;
            break;
        }
    }
    ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
    return found;
}

uint32_t ivr_worker_active_sessions(const ivr_worker_t *worker) {
    uint32_t active = 0;
    if (!worker) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&worker->lock);
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (worker->slots[i].used) {
            active++;
        }
    }
    ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
    return active;
}

ivr_status_t ivr_worker_release_call(ivr_worker_t *worker,
                                     const ivr_call_ref_t *call) {
    ivr_session_t *session = NULL;
    if (!worker || !call) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (worker->slots[i].used &&
            worker->slots[i].session.call_generation ==
                call->call_generation &&
            worker->slots[i].session.room_id.size == call->room_id.size &&
            (call->room_id.size == 0 ||
             memcmp(worker->slots[i].session.room_id.data, call->room_id.data,
                    call->room_id.size) == 0) &&
            worker->slots[i].session.call_id.size == call->call_id.size &&
            (call->call_id.size == 0 ||
             memcmp(worker->slots[i].session.call_id.data, call->call_id.data,
                    call->call_id.size) == 0)) {
            session = &worker->slots[i].session;
            break;
        }
    }
    ivr_mutex_unlock(&worker->lock);
    if (!session) {
        return IVR_OK;
    }
    ivr_session_destroy(session);
    return ivr_worker_has_session(worker, call) ? IVR_ESTATE : IVR_OK;
}

ivr_status_t ivr_worker_begin_drain(ivr_worker_t *worker) {
    if (!worker) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    if (atomic_load((atomic_int *)&worker->draining)) {
        while (worker->drain_in_progress && worker->drain_cond_initialized) {
            ivr_cond_wait(&worker->drain_cond, &worker->lock);
        }
        ivr_mutex_unlock(&worker->lock);
        return IVR_OK; /* idempotent */
    }
    atomic_store((atomic_int *)&worker->draining, 1);
    worker->drain_in_progress = 1;
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        if (worker->slots[i].used) {
            if (worker->slots[i].cleanup_owner == IVR_SLOT_CLEANUP_NONE) {
                worker->slots[i].cleanup_owner = IVR_SLOT_CLEANUP_DRAIN;
            }
            atomic_store((atomic_int *)&worker->slots[i].session.draining, 1);
            ivr_session_request_terminal(&worker->slots[i].session);
        }
    }
    ivr_mutex_unlock(&worker->lock);
    /* join outside the lock so terminal callbacks can progress. When a drain
       deadline is configured, sessions that exceed it are reported via
       ivr_worker_drain_timed_out() and joined again by destroy() before any
       memory is freed (memory safety is never sacrificed to the deadline). */
    for (uint32_t i = 0; i < worker->slot_count; i++) {
        void *media_instance = NULL;
        ivr_mutex_lock(&worker->lock);
        while (worker->slots[i].used &&
               worker->slots[i].cleanup_owner == IVR_SLOT_CLEANUP_SESSION &&
               worker->drain_cond_initialized) {
            ivr_cond_wait(&worker->drain_cond, &worker->lock);
        }
        int drain_owns_slot = worker->slots[i].used &&
            worker->slots[i].cleanup_owner == IVR_SLOT_CLEANUP_DRAIN;
        ivr_mutex_unlock(&worker->lock);
        if (!drain_owns_slot) {
            continue;
        }
        if (worker->drain_deadline_ms > 0) {
            int timed_out = 0;
            if (ivr_session_join_timed(&worker->slots[i].session,
                                       worker->drain_deadline_ms,
                                       &timed_out) != 0 &&
                timed_out) {
                ivr_mutex_lock(&worker->lock);
                worker->drain_timed_out++;
                ivr_mutex_unlock(&worker->lock);
                continue; /* keep the slot used; destroy() joins it later */
            }
        } else {
            ivr_session_join(&worker->slots[i].session);
        }
        ivr_mutex_lock(&worker->lock);
        if (worker->slots[i].used &&
            worker->slots[i].cleanup_owner == IVR_SLOT_CLEANUP_DRAIN) {
            ivr_session_free(&worker->slots[i].session);
            media_instance = worker->slots[i].media_instance;
            worker->slots[i].media_instance = NULL;
            if (worker->slots[i].package && worker->slots[i].package->active_sessions > 0) {
                worker->slots[i].package->active_sessions--;
            }
            worker->slots[i].package = NULL;
            worker->slots[i].used = 0;
            worker->slots[i].cleanup_owner = IVR_SLOT_CLEANUP_NONE;
        }
        ivr_mutex_unlock(&worker->lock);
        if (media_instance) {
            worker->media_factory.destroy(worker->media_factory.context,
                                          media_instance);
        }
    }
    ivr_mutex_lock(&worker->lock);
    worker->drain_in_progress = 0;
    if (worker->drain_cond_initialized) {
        ivr_cond_broadcast(&worker->drain_cond);
    }
    ivr_mutex_unlock(&worker->lock);
    return IVR_OK;
}

uint64_t ivr_worker_drain_timed_out(const ivr_worker_t *worker) {
    uint64_t count;

    if (!worker || !worker->lock_initialized) return 0;
    ivr_mutex_lock((ivr_mutex_t *)&worker->lock);
    count = worker->drain_timed_out;
    ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
    return count;
}

ivr_status_t ivr_worker_destroy(ivr_worker_t *worker) {
    if (!worker) {
        return IVR_EINVAL;
    }
    /* Destroy owns the drain transition so callers cannot accidentally free
       a live control thread. Partial construction remains safe. */
    if (worker->lock_initialized) {
        (void)ivr_worker_begin_drain(worker);
    }
    if (worker->slots) {
        for (uint32_t i = 0; i < worker->slot_count; i++) {
            if (worker->slots[i].used) {
                /* sessions that exceeded the drain deadline are joined here so
                   their control threads are gone before ivr_session_free */
                ivr_session_join(&worker->slots[i].session);
                ivr_session_free(&worker->slots[i].session);
                worker->media_factory.destroy(worker->media_factory.context,
                                              worker->slots[i].media_instance);
                worker->slots[i].media_instance = NULL;
                if (worker->slots[i].package &&
                    worker->slots[i].package->active_sessions > 0) {
                    worker->slots[i].package->active_sessions--;
                }
                worker->slots[i].package = NULL;
                worker->slots[i].used = 0;
                worker->slots[i].cleanup_owner = IVR_SLOT_CLEANUP_NONE;
            }
        }
        free(worker->slots);
        worker->slots = NULL;
    }
    for (uint32_t i = 0; i < worker->package_count; i++) {
        ivr_content_package_free(&worker->packages[i].pkg);
        ivr_str_free(&worker->packages[i].name);
    }
    free(worker->packages);
    worker->packages = NULL;
    ivr_str_free(&worker->worker_id);
    ivr_str_free(&worker->content_root);
    if (worker->drain_cond_initialized) {
        ivr_cond_destroy(&worker->drain_cond);
        worker->drain_cond_initialized = 0;
    }
    if (worker->lock_initialized) {
        ivr_mutex_destroy(&worker->lock);
        worker->lock_initialized = 0;
    }
    free(worker);
    return IVR_OK;
}
