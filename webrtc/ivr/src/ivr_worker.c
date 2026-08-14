#include "ivr_internal.h"
#include "ivr_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    IVR_MEDIA_SLOT_FREE = 0,
    IVR_MEDIA_SLOT_OPENING,
    IVR_MEDIA_SLOT_ACTIVE,
    IVR_MEDIA_SLOT_CLOSING
} ivr_media_slot_state_t;

typedef struct {
    ivr_media_slot_state_t state;
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t expected_room_version;
    ivr_media_port_ops_t media;
    void *media_instance;
    uint32_t borrowed_calls;
    int action_in_flight;
    uint64_t last_operation_generation;
    ivr_status_t last_operation_status;
    char input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t input_generation;
    int input_active;
} ivr_media_slot_t;

struct ivr_worker_s {
    char worker_id[IVR_MEDIA_ID_CAPACITY];
    char worker_instance_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t worker_epoch;
    uint64_t inventory_revision;
    uint32_t slot_count;
    uint64_t (*now_ms)(void *context);
    void *now_context;
    ivr_media_event_sink_ops_t event_sink;
    ivr_media_port_factory_ops_t media_factory;
    ivr_media_slot_t *slots;
    ivr_mutex_t lock;
    ivr_cond_t state_changed;
    int lock_initialized;
    int cond_initialized;
    int started;
    int draining;
};

typedef enum {
    IVR_MEDIA_ACTION_PLAY,
    IVR_MEDIA_ACTION_BEGIN_INPUT,
    IVR_MEDIA_ACTION_END_INPUT,
    IVR_MEDIA_ACTION_CANCEL_INPUT
} ivr_media_action_t;

static int view_valid(const ivr_bytes_view_t *view) {
    return view && view->data && view->size > 0;
}

static int copy_id(char *out, const ivr_bytes_view_t *view) {
    if (!out || !view_valid(view) || view->size >= IVR_MEDIA_ID_CAPACITY) {
        return 0;
    }
    memcpy(out, view->data, view->size);
    out[view->size] = '\0';
    return 1;
}

static int inventory_revision_advance_locked(ivr_worker_t *worker) {
    if (worker->inventory_revision == UINT64_MAX) {
        return 0;
    }
    ++worker->inventory_revision;
    return 1;
}

static int call_valid(const ivr_call_ref_t *call) {
    return call && call->call_generation > 0 &&
           ((call->tenant_id.size == 0u && !call->tenant_id.data) ||
            (view_valid(&call->tenant_id) &&
             call->tenant_id.size < IVR_MEDIA_ID_CAPACITY)) &&
           view_valid(&call->provider_session_id) &&
           call->provider_session_id.size < IVR_MEDIA_ID_CAPACITY &&
           view_valid(&call->dialog_id) &&
           call->dialog_id.size < IVR_MEDIA_ID_CAPACITY &&
           view_valid(&call->room_id) &&
           call->room_id.size < IVR_MEDIA_ID_CAPACITY &&
           view_valid(&call->call_id) &&
           call->call_id.size < IVR_MEDIA_ID_CAPACITY;
}

static int slot_matches(const ivr_media_slot_t *slot,
                        const ivr_call_ref_t *call) {
    return slot->state != IVR_MEDIA_SLOT_FREE && call_valid(call) &&
           strlen(slot->tenant_id) == call->tenant_id.size &&
           (call->tenant_id.size == 0u ||
            memcmp(slot->tenant_id, call->tenant_id.data,
                   call->tenant_id.size) == 0) &&
           strlen(slot->provider_session_id) ==
               call->provider_session_id.size &&
           memcmp(slot->provider_session_id, call->provider_session_id.data,
                  call->provider_session_id.size) == 0 &&
           strlen(slot->dialog_id) == call->dialog_id.size &&
           memcmp(slot->dialog_id, call->dialog_id.data,
                  call->dialog_id.size) == 0 &&
           slot->call_generation == call->call_generation &&
           strlen(slot->room_id) == call->room_id.size &&
           memcmp(slot->room_id, call->room_id.data, call->room_id.size) == 0 &&
           strlen(slot->call_id) == call->call_id.size &&
           memcmp(slot->call_id, call->call_id.data, call->call_id.size) == 0;
}

static ivr_media_slot_t *find_slot(ivr_worker_t *worker,
                                   const ivr_call_ref_t *call) {
    uint32_t index;
    for (index = 0; index < worker->slot_count; ++index) {
        if (slot_matches(&worker->slots[index], call)) {
            return &worker->slots[index];
        }
    }
    return NULL;
}

static void slot_call(const ivr_media_slot_t *slot, ivr_call_ref_t *out) {
    memset(out, 0, sizeof(*out));
    out->tenant_id.data = slot->tenant_id;
    out->tenant_id.size = strlen(slot->tenant_id);
    out->provider_session_id.data = slot->provider_session_id;
    out->provider_session_id.size = strlen(slot->provider_session_id);
    out->dialog_id.data = slot->dialog_id;
    out->dialog_id.size = strlen(slot->dialog_id);
    out->room_id.data = slot->room_id;
    out->room_id.size = strlen(slot->room_id);
    out->call_id.data = slot->call_id;
    out->call_id.size = strlen(slot->call_id);
    out->call_generation = slot->call_generation;
    out->expected_room_version = slot->expected_room_version;
}

static int media_ops_valid(const ivr_media_port_ops_t *media) {
    return media && media->abi_version == IVR_WORKER_ABI_VERSION &&
           media->start_bot && media->play_pcm && media->cancel_input &&
           media->stop_bot && media->begin_input && media->end_input;
}

ivr_status_t ivr_worker_create(
    const ivr_worker_config_t *config,
    const ivr_media_event_sink_ops_t *event_sink,
    const ivr_media_port_factory_ops_t *media_factory,
    ivr_worker_t **out_worker) {
    ivr_worker_t *worker;
    size_t worker_id_size;
    size_t worker_instance_id_size;

    if (out_worker) {
        *out_worker = NULL;
    }
    if (!config || !event_sink || !media_factory || !out_worker) {
        return IVR_EINVAL;
    }
    if (config->abi_version != IVR_WORKER_ABI_VERSION ||
        event_sink->abi_version != IVR_WORKER_ABI_VERSION ||
        media_factory->abi_version != IVR_WORKER_ABI_VERSION) {
        return IVR_EVERSION;
    }
    if (!event_sink->publish_copy || !media_factory->create ||
        !media_factory->destroy || !config->worker_id ||
        !config->worker_instance_id || config->worker_epoch == 0 ||
        config->max_sessions_per_worker == 0) {
        return IVR_EINVAL;
    }
    worker_id_size = strlen(config->worker_id);
    worker_instance_id_size = strlen(config->worker_instance_id);
    if (worker_id_size == 0 || worker_id_size >= IVR_MEDIA_ID_CAPACITY ||
        worker_instance_id_size == 0 ||
        worker_instance_id_size >= IVR_MEDIA_ID_CAPACITY ||
        config->max_sessions_per_worker >
            SIZE_MAX / sizeof(ivr_media_slot_t)) {
        return IVR_EINVAL;
    }

    worker = (ivr_worker_t *)calloc(1, sizeof(*worker));
    if (!worker) {
        return IVR_ENOSPC;
    }
    memcpy(worker->worker_id, config->worker_id, worker_id_size + 1);
    memcpy(worker->worker_instance_id, config->worker_instance_id,
           worker_instance_id_size + 1);
    worker->worker_epoch = config->worker_epoch;
    worker->inventory_revision = 1;
    worker->slot_count = config->max_sessions_per_worker;
    worker->now_ms = config->now_ms;
    worker->now_context = config->now_context;
    worker->event_sink = *event_sink;
    worker->media_factory = *media_factory;
    worker->slots = (ivr_media_slot_t *)calloc(worker->slot_count,
                                                sizeof(*worker->slots));
    if (!worker->slots || ivr_mutex_init(&worker->lock) != 0) {
        ivr_worker_destroy(worker);
        return IVR_ENOSPC;
    }
    worker->lock_initialized = 1;
    if (ivr_cond_init(&worker->state_changed) != 0) {
        ivr_worker_destroy(worker);
        return IVR_ENOSPC;
    }
    worker->cond_initialized = 1;
    *out_worker = worker;
    return IVR_OK;
}

ivr_status_t ivr_worker_start(ivr_worker_t *worker) {
    if (!worker) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    if (worker->started || worker->draining) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    worker->started = 1;
    ivr_mutex_unlock(&worker->lock);
    return IVR_OK;
}

ivr_status_t ivr_worker_advance_epoch(ivr_worker_t *worker,
                                      uint64_t worker_epoch) {
    if (!worker || worker_epoch == 0) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    if (!worker->started || worker->draining) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ECLOSED;
    }
    if (worker_epoch <= worker->worker_epoch) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTALE;
    }
    if (!inventory_revision_advance_locked(worker)) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    worker->worker_epoch = worker_epoch;
    ivr_mutex_unlock(&worker->lock);
    return IVR_OK;
}

static ivr_status_t open_media_call(ivr_worker_t *worker,
                                    const ivr_call_ref_t *call,
                                    uint64_t operation_generation) {
    ivr_media_slot_t *slot = NULL;
    ivr_media_port_ops_t media;
    void *instance = NULL;
    ivr_status_t status;
    uint32_t index;

    if (!worker || !call_valid(call)) {
        return IVR_EINVAL;
    }
    memset(&media, 0, sizeof(media));
    ivr_mutex_lock(&worker->lock);
    if (!worker->started || worker->draining) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ECLOSED;
    }
    slot = find_slot(worker, call);
    if (slot) {
        if (slot->state != IVR_MEDIA_SLOT_ACTIVE) {
            status = IVR_EBUSY;
        } else if (operation_generation == 0u) {
            status = IVR_OK;
        } else if (operation_generation < slot->last_operation_generation) {
            status = IVR_ESTALE;
        } else if (operation_generation == slot->last_operation_generation) {
            status = slot->last_operation_status;
        } else {
            status = IVR_ESTATE;
        }
        ivr_mutex_unlock(&worker->lock);
        return status;
    }
    for (index = 0; index < worker->slot_count; ++index) {
        if (worker->slots[index].state == IVR_MEDIA_SLOT_FREE) {
            slot = &worker->slots[index];
            break;
        }
    }
    if (!slot) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ENOSPC;
    }
    /* Opening has exactly two inventory-visible transitions: FREE->OPENING
       and OPENING->ACTIVE/FREE. Reserve both revisions before external I/O. */
    if (worker->inventory_revision > UINT64_MAX - 2u ||
        !inventory_revision_advance_locked(worker)) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->state = IVR_MEDIA_SLOT_OPENING;
    if (call->tenant_id.size > 0u) {
        (void)copy_id(slot->tenant_id, &call->tenant_id);
    }
    (void)copy_id(slot->provider_session_id, &call->provider_session_id);
    (void)copy_id(slot->dialog_id, &call->dialog_id);
    (void)copy_id(slot->room_id, &call->room_id);
    (void)copy_id(slot->call_id, &call->call_id);
    slot->call_generation = call->call_generation;
    slot->expected_room_version = call->expected_room_version;
    ivr_mutex_unlock(&worker->lock);

    status = worker->media_factory.create(worker->media_factory.context, call,
                                          &media, &instance);
    if (status == IVR_OK && !media_ops_valid(&media)) {
        status = media.abi_version == IVR_WORKER_ABI_VERSION
                     ? IVR_EINVAL
                     : IVR_EVERSION;
    }
    if (status == IVR_OK) {
        status = media.start_bot(media.context, call);
    }

    ivr_mutex_lock(&worker->lock);
    if (status == IVR_OK && !worker->draining &&
        slot->state == IVR_MEDIA_SLOT_OPENING) {
        (void)inventory_revision_advance_locked(worker);
        slot->media = media;
        slot->media_instance = instance;
        slot->state = IVR_MEDIA_SLOT_ACTIVE;
        slot->last_operation_generation = operation_generation;
        slot->last_operation_status = IVR_OK;
        ivr_cond_broadcast(&worker->state_changed);
        ivr_mutex_unlock(&worker->lock);
        return IVR_OK;
    }
    if (status == IVR_OK) {
        status = IVR_ECLOSED;
    }
    (void)inventory_revision_advance_locked(worker);
    memset(slot, 0, sizeof(*slot));
    ivr_cond_broadcast(&worker->state_changed);
    ivr_mutex_unlock(&worker->lock);

    if (instance) {
        if (media_ops_valid(&media)) {
            (void)media.stop_bot(media.context, call);
        }
        worker->media_factory.destroy(worker->media_factory.context, instance);
    }
    return status;
}

ivr_status_t ivr_worker_open_media_call(ivr_worker_t *worker,
                                        const ivr_call_ref_t *call) {
    return open_media_call(worker, call, 0u);
}

ivr_status_t ivr_worker_open_media_operation(
    ivr_worker_t *worker, const ivr_media_operation_t *operation) {
    uint64_t now_ms;
    if (!worker || !operation || operation->operation_generation == 0u ||
        !call_valid(&operation->call)) {
        return IVR_EINVAL;
    }
    if (operation->deadline_ms != 0u) {
        if (!worker->now_ms) {
            return IVR_EINVAL;
        }
        now_ms = worker->now_ms(worker->now_context);
        if (now_ms > operation->deadline_ms) {
            return IVR_ESTALE;
        }
    }
    return open_media_call(worker, &operation->call,
                           operation->operation_generation);
}

static ivr_status_t execute_operation(
    ivr_worker_t *worker, const ivr_media_operation_t *operation,
    ivr_media_action_t action, const ivr_bytes_view_t *input,
    uint64_t input_generation) {
    ivr_media_slot_t *slot;
    ivr_media_port_ops_t media;
    ivr_call_ref_t call;
    ivr_status_t status;
    uint64_t now_ms = 0;

    if (!worker || !operation || !call_valid(&operation->call) ||
        operation->operation_generation == 0 ||
        ((action == IVR_MEDIA_ACTION_PLAY ||
          action == IVR_MEDIA_ACTION_BEGIN_INPUT ||
          action == IVR_MEDIA_ACTION_END_INPUT ||
          action == IVR_MEDIA_ACTION_CANCEL_INPUT) &&
         !view_valid(input)) ||
        ((action == IVR_MEDIA_ACTION_BEGIN_INPUT ||
          action == IVR_MEDIA_ACTION_END_INPUT ||
          action == IVR_MEDIA_ACTION_CANCEL_INPUT) &&
         (input->size >= IVR_MEDIA_ID_CAPACITY || input_generation == 0))) {
        return IVR_EINVAL;
    }
    if (operation->deadline_ms != 0) {
        if (!worker->now_ms) {
            return IVR_EINVAL;
        }
        now_ms = worker->now_ms(worker->now_context);
    }

    ivr_mutex_lock(&worker->lock);
    slot = find_slot(worker, &operation->call);
    if (!slot || slot->state != IVR_MEDIA_SLOT_ACTIVE || worker->draining) {
        ivr_mutex_unlock(&worker->lock);
        return worker->draining ? IVR_ECLOSED : IVR_ESTATE;
    }
    if (operation->deadline_ms != 0 && now_ms > operation->deadline_ms) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTALE;
    }
    if (operation->operation_generation < slot->last_operation_generation) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTALE;
    }
    if (operation->operation_generation == slot->last_operation_generation) {
        status = slot->last_operation_status;
        ivr_mutex_unlock(&worker->lock);
        return status;
    }
    if (slot->action_in_flight) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_EBUSY;
    }
    if (action == IVR_MEDIA_ACTION_BEGIN_INPUT && slot->input_active) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    if ((action == IVR_MEDIA_ACTION_END_INPUT ||
         action == IVR_MEDIA_ACTION_CANCEL_INPUT) &&
        (!slot->input_active || slot->input_generation != input_generation ||
         strlen(slot->input_id) != input->size ||
         memcmp(slot->input_id, input->data, input->size) != 0)) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTALE;
    }
    slot->action_in_flight = 1;
    ++slot->borrowed_calls;
    media = slot->media;
    slot_call(slot, &call);
    ivr_mutex_unlock(&worker->lock);

    switch (action) {
        case IVR_MEDIA_ACTION_PLAY:
            status = media.play_pcm(media.context, &call, input);
            break;
        case IVR_MEDIA_ACTION_BEGIN_INPUT:
            status = media.begin_input(media.context, &call, input,
                                       input_generation);
            break;
        case IVR_MEDIA_ACTION_END_INPUT:
            status = media.end_input(media.context, &call, input,
                                     input_generation);
            break;
        case IVR_MEDIA_ACTION_CANCEL_INPUT:
            status = media.cancel_input(media.context, &call);
            break;
        default:
            status = IVR_EINVAL;
            break;
    }

    ivr_mutex_lock(&worker->lock);
    slot->last_operation_generation = operation->operation_generation;
    slot->last_operation_status = status;
    if (status == IVR_OK && action == IVR_MEDIA_ACTION_BEGIN_INPUT) {
        (void)copy_id(slot->input_id, input);
        slot->input_generation = input_generation;
        slot->input_active = 1;
    } else if (status == IVR_OK &&
               (action == IVR_MEDIA_ACTION_END_INPUT ||
                action == IVR_MEDIA_ACTION_CANCEL_INPUT)) {
        slot->input_id[0] = '\0';
        slot->input_generation = 0;
        slot->input_active = 0;
    }
    slot->action_in_flight = 0;
    --slot->borrowed_calls;
    ivr_cond_broadcast(&worker->state_changed);
    ivr_mutex_unlock(&worker->lock);
    return status;
}

ivr_status_t ivr_worker_play(ivr_worker_t *worker,
                             const ivr_media_operation_t *operation,
                             const ivr_bytes_view_t *text) {
    return execute_operation(worker, operation, IVR_MEDIA_ACTION_PLAY, text, 0);
}

ivr_status_t ivr_worker_begin_input(ivr_worker_t *worker,
                                    const ivr_media_operation_t *operation,
                                    const ivr_bytes_view_t *input_id,
                                    uint64_t input_generation) {
    return execute_operation(worker, operation, IVR_MEDIA_ACTION_BEGIN_INPUT,
                             input_id, input_generation);
}

ivr_status_t ivr_worker_end_input(ivr_worker_t *worker,
                                  const ivr_media_operation_t *operation,
                                  const ivr_bytes_view_t *input_id,
                                  uint64_t input_generation) {
    return execute_operation(worker, operation, IVR_MEDIA_ACTION_END_INPUT,
                             input_id, input_generation);
}

ivr_status_t ivr_worker_cancel_input(ivr_worker_t *worker,
                                     const ivr_media_operation_t *operation,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    return execute_operation(worker, operation, IVR_MEDIA_ACTION_CANCEL_INPUT,
                             input_id, input_generation);
}

ivr_status_t ivr_worker_close_media_call(ivr_worker_t *worker,
                                         const ivr_call_ref_t *call) {
    ivr_media_slot_t *slot;
    ivr_media_port_ops_t media;
    ivr_call_ref_t owned_call;
    void *instance;
    ivr_status_t stop_status;

    if (!worker || !call_valid(call)) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    slot = find_slot(worker, call);
    if (!slot) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_OK;
    }
    while (slot->state == IVR_MEDIA_SLOT_OPENING) {
        ivr_cond_wait(&worker->state_changed, &worker->lock);
        slot = find_slot(worker, call);
        if (!slot) {
            ivr_mutex_unlock(&worker->lock);
            return IVR_OK;
        }
    }
    if (slot->state == IVR_MEDIA_SLOT_CLOSING) {
        while ((slot = find_slot(worker, call)) != NULL) {
            ivr_cond_wait(&worker->state_changed, &worker->lock);
        }
        ivr_mutex_unlock(&worker->lock);
        return IVR_OK;
    }
    /* Closing also reserves CLOSING and FREE revisions before calling the
       media port outside the lock. */
    if (worker->inventory_revision > UINT64_MAX - 2u ||
        !inventory_revision_advance_locked(worker)) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    slot->state = IVR_MEDIA_SLOT_CLOSING;
    while (slot->borrowed_calls > 0) {
        ivr_cond_wait(&worker->state_changed, &worker->lock);
    }
    media = slot->media;
    instance = slot->media_instance;
    slot_call(slot, &owned_call);
    ivr_mutex_unlock(&worker->lock);

    stop_status = media.stop_bot(media.context, &owned_call);
    worker->media_factory.destroy(worker->media_factory.context, instance);

    ivr_mutex_lock(&worker->lock);
    (void)inventory_revision_advance_locked(worker);
    memset(slot, 0, sizeof(*slot));
    ivr_cond_broadcast(&worker->state_changed);
    ivr_mutex_unlock(&worker->lock);
    return stop_status;
}

ivr_status_t ivr_worker_publish_event_copy(ivr_worker_t *worker,
                                           const ivr_event_view_t *event) {
    ivr_media_event_sink_ops_t sink;
    ivr_media_slot_t *slot;
    ivr_status_t status;

    if (!worker || !event || !call_valid(&event->call) ||
        !view_valid(&event->event_type)) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    slot = find_slot(worker, &event->call);
    if (!slot || slot->state != IVR_MEDIA_SLOT_ACTIVE) {
        ivr_mutex_unlock(&worker->lock);
        return IVR_ESTATE;
    }
    ++slot->borrowed_calls;
    sink = worker->event_sink;
    ivr_mutex_unlock(&worker->lock);

    status = sink.publish_copy(sink.context, event);

    ivr_mutex_lock(&worker->lock);
    --slot->borrowed_calls;
    ivr_cond_broadcast(&worker->state_changed);
    ivr_mutex_unlock(&worker->lock);
    return status;
}

int ivr_worker_has_media_call(const ivr_worker_t *worker,
                              const ivr_call_ref_t *call) {
    int found;
    if (!worker || !call_valid(call)) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&worker->lock);
    found = find_slot((ivr_worker_t *)worker, call) != NULL;
    ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
    return found;
}

uint32_t ivr_worker_active_sessions(const ivr_worker_t *worker) {
    uint32_t active = 0;
    uint32_t index;
    if (!worker) {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&worker->lock);
    for (index = 0; index < worker->slot_count; ++index) {
        if (worker->slots[index].state != IVR_MEDIA_SLOT_FREE) {
            ++active;
        }
    }
    ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
    return active;
}

static ivr_worker_resource_state_t inventory_state(
    ivr_media_slot_state_t state) {
    switch (state) {
        case IVR_MEDIA_SLOT_OPENING:
            return IVR_WORKER_RESOURCE_OPENING;
        case IVR_MEDIA_SLOT_ACTIVE:
            return IVR_WORKER_RESOURCE_ACTIVE;
        case IVR_MEDIA_SLOT_CLOSING:
            return IVR_WORKER_RESOURCE_CLOSING;
        default:
            return 0;
    }
}

ivr_status_t ivr_worker_query_inventory(
    const ivr_worker_t *worker, const ivr_worker_inventory_query_t *query,
    ivr_worker_inventory_page_t *out_page) {
    uint32_t index;
    uint32_t last_returned_cursor = 0;
    int limit_reached = 0;

    if (out_page) {
        memset(out_page, 0, sizeof(*out_page));
    }
    if (!worker || !query || !out_page) {
        return IVR_EINVAL;
    }
    if (query->inventory_version != IVR_WORKER_INVENTORY_VERSION) {
        return IVR_EVERSION;
    }
    if (query->limit == 0 ||
        query->limit > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE) {
        return IVR_EINVAL;
    }

    ivr_mutex_lock((ivr_mutex_t *)&worker->lock);
    if (query->cursor > worker->slot_count) {
        ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
        return IVR_EINVAL;
    }
    if (query->expected_revision != 0 &&
        query->expected_revision != worker->inventory_revision) {
        ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
        return IVR_ESTALE;
    }

    out_page->inventory_version = IVR_WORKER_INVENTORY_VERSION;
    out_page->revision = worker->inventory_revision;
    out_page->cursor = query->cursor;
    for (index = 0; index < worker->slot_count; ++index) {
        if (worker->slots[index].state != IVR_MEDIA_SLOT_FREE) {
            ++out_page->total_active;
        }
    }
    for (index = query->cursor; index < worker->slot_count; ++index) {
        const ivr_media_slot_t *slot = &worker->slots[index];
        ivr_worker_inventory_record_t *record;
        if (slot->state == IVR_MEDIA_SLOT_FREE) {
            continue;
        }
        if (out_page->count == query->limit) {
            limit_reached = 1;
            break;
        }
        record = &out_page->records[out_page->count++];
        snprintf(record->worker_id, sizeof(record->worker_id), "%s",
                 worker->worker_id);
        snprintf(record->worker_instance_id,
                 sizeof(record->worker_instance_id), "%s",
                 worker->worker_instance_id);
        record->worker_epoch = worker->worker_epoch;
        snprintf(record->tenant_id, sizeof(record->tenant_id), "%s",
                 slot->tenant_id);
        snprintf(record->provider_session_id,
                 sizeof(record->provider_session_id), "%s",
                 slot->provider_session_id);
        snprintf(record->dialog_id, sizeof(record->dialog_id), "%s",
                 slot->dialog_id);
        snprintf(record->room_id, sizeof(record->room_id), "%s",
                 slot->room_id);
        snprintf(record->call_id, sizeof(record->call_id), "%s",
                 slot->call_id);
        record->call_generation = slot->call_generation;
        record->operation_generation = slot->last_operation_generation;
        snprintf(record->input_id, sizeof(record->input_id), "%s",
                 slot->input_id);
        record->input_generation = slot->input_generation;
        record->input_active = slot->input_active;
        record->state = inventory_state(slot->state);
        record->rebindable = slot->state == IVR_MEDIA_SLOT_ACTIVE;
        last_returned_cursor = index + 1u;
    }
    if (limit_reached) {
        out_page->has_more = 1;
        out_page->next_cursor = last_returned_cursor;
    }
    ivr_mutex_unlock((ivr_mutex_t *)&worker->lock);
    return IVR_OK;
}

ivr_status_t ivr_worker_begin_drain(ivr_worker_t *worker) {
    ivr_call_ref_t call;
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    ivr_status_t result = IVR_OK;
    uint32_t index;

    if (!worker) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&worker->lock);
    worker->draining = 1;
    worker->started = 0;
    ivr_mutex_unlock(&worker->lock);

    for (;;) {
        int found = 0;
        ivr_mutex_lock(&worker->lock);
        for (index = 0; index < worker->slot_count; ++index) {
            if (worker->slots[index].state == IVR_MEDIA_SLOT_OPENING ||
                worker->slots[index].state == IVR_MEDIA_SLOT_CLOSING) {
                ivr_cond_wait(&worker->state_changed, &worker->lock);
                found = -1;
                break;
            }
            if (worker->slots[index].state == IVR_MEDIA_SLOT_ACTIVE) {
                snprintf(tenant_id, sizeof(tenant_id), "%s",
                         worker->slots[index].tenant_id);
                snprintf(provider_session_id, sizeof(provider_session_id),
                         "%s", worker->slots[index].provider_session_id);
                snprintf(dialog_id, sizeof(dialog_id), "%s",
                         worker->slots[index].dialog_id);
                snprintf(room_id, sizeof(room_id), "%s",
                         worker->slots[index].room_id);
                snprintf(call_id, sizeof(call_id), "%s",
                         worker->slots[index].call_id);
                memset(&call, 0, sizeof(call));
                call.tenant_id.data = tenant_id[0] ? tenant_id : NULL;
                call.tenant_id.size = strlen(tenant_id);
                call.provider_session_id.data = provider_session_id;
                call.provider_session_id.size =
                    strlen(provider_session_id);
                call.dialog_id.data = dialog_id;
                call.dialog_id.size = strlen(dialog_id);
                call.room_id.data = room_id;
                call.room_id.size = strlen(room_id);
                call.call_id.data = call_id;
                call.call_id.size = strlen(call_id);
                call.call_generation = worker->slots[index].call_generation;
                call.expected_room_version =
                    worker->slots[index].expected_room_version;
                found = 1;
                break;
            }
        }
        ivr_mutex_unlock(&worker->lock);
        if (found == 0) {
            break;
        }
        if (found > 0) {
            ivr_status_t status = ivr_worker_close_media_call(worker, &call);
            if (status != IVR_OK && result == IVR_OK) {
                result = status;
            }
        }
    }
    return result;
}

ivr_status_t ivr_worker_destroy(ivr_worker_t *worker) {
    ivr_status_t result;
    if (!worker) {
        return IVR_OK;
    }
    result = worker->lock_initialized ? ivr_worker_begin_drain(worker)
                                      : IVR_OK;
    if (worker->cond_initialized) {
        ivr_cond_destroy(&worker->state_changed);
    }
    if (worker->lock_initialized) {
        ivr_mutex_destroy(&worker->lock);
    }
    free(worker->slots);
    free(worker);
    return result;
}
