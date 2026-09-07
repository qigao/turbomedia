#include "salts_ice_owner.h"

#include <salts/thread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBO_ICE_OWNER_POLL_INTERVAL_MS = 10
};

#define TURBO_ICE_OWNER_POLL_INTERVAL_NS \
    ((uint64_t)TURBO_ICE_OWNER_POLL_INTERVAL_MS * UINT64_C(1000000))

typedef enum turbo_ice_command_e {
    TURBO_ICE_COMMAND_NONE = 0,
    TURBO_ICE_COMMAND_GET_LOCAL_CREDENTIALS,
    TURBO_ICE_COMMAND_SET_REMOTE_CREDENTIALS,
    TURBO_ICE_COMMAND_RESTART,
    TURBO_ICE_COMMAND_GATHER,
    TURBO_ICE_COMMAND_ADD_REMOTE_CANDIDATE,
    TURBO_ICE_COMMAND_END_OF_CANDIDATES,
    TURBO_ICE_COMMAND_START_CHECKS,
    TURBO_ICE_COMMAND_SEND,
    TURBO_ICE_COMMAND_SET_ALLOW_LOOPBACK,
    TURBO_ICE_COMMAND_SET_ROLE,
    TURBO_ICE_COMMAND_DETACH_CALLBACKS
} turbo_ice_command_t;

typedef enum turbo_ice_command_phase_e {
    TURBO_ICE_COMMAND_IDLE = 0,
    TURBO_ICE_COMMAND_REQUESTED,
    TURBO_ICE_COMMAND_DONE
} turbo_ice_command_phase_t;

typedef struct turbo_ice_command_args_s {
    const char *text_a;
    const char *text_b;
    const void *data;
    size_t data_len;
    char *output_a;
    size_t output_a_len;
    char *output_b;
    size_t output_b_len;
    ice_candidate_t *candidate;
    ice_restart_options_t restart_options;
    int int_value;
} turbo_ice_command_args_t;

typedef struct turbo_ice_send_slot_s {
    size_t size;
    uint8_t data[TURBO_ICE_OWNER_MAX_DATAGRAM_BYTES];
} turbo_ice_send_slot_t;

struct turbo_ice_owner_s {
    ice_config_t config;
    ice_callbacks_t callbacks;
    salts_ice_agent_t *agent;
    salts_thread_t thread;
    salts_mutex_t mutex;
    salts_cond_t cond;
    int sync_initialized;
    int thread_started;
    int ready;
    int stopping;
    int close_requested;
    int create_failed;
    turbo_ice_command_phase_t phase;
    turbo_ice_command_t command;
    turbo_ice_command_args_t args;
    int result;
    turbo_ice_send_slot_t send_slots[TURBO_ICE_OWNER_SEND_CAPACITY];
    size_t send_head;
    size_t send_tail;
    size_t send_count;
    ice_state_t cached_state;
    ice_gathering_state_t cached_gathering_state;
    ice_candidate_t local_candidates[ICE_MAX_CANDIDATES];
    int local_candidate_count;
};

static SALTS_THREAD_LOCAL turbo_ice_owner_t *turbo_current_ice_owner;

static void turbo_ice_owner_on_state_change(
    salts_ice_agent_t *agent, ice_state_t old_state, ice_state_t new_state,
    void *user_data) {
    turbo_ice_owner_t *owner = (turbo_ice_owner_t *)user_data;
    ice_state_cb callback;
    void *callback_user_data;

    if (!owner) {
        return;
    }
    salts_mutex_lock(&owner->mutex);
    owner->cached_state = new_state;
    callback = owner->callbacks.on_state_change;
    callback_user_data = owner->callbacks.user_data;
    salts_mutex_unlock(&owner->mutex);
    if (callback) {
        callback(agent, old_state, new_state, callback_user_data);
    }
}

static void turbo_ice_owner_on_gathering_change(
    salts_ice_agent_t *agent, ice_gathering_state_t state, void *user_data) {
    turbo_ice_owner_t *owner = (turbo_ice_owner_t *)user_data;
    ice_gathering_cb callback;
    void *callback_user_data;

    if (!owner) {
        return;
    }
    salts_mutex_lock(&owner->mutex);
    owner->cached_gathering_state = state;
    callback = owner->callbacks.on_gathering_change;
    callback_user_data = owner->callbacks.user_data;
    salts_mutex_unlock(&owner->mutex);
    if (callback) {
        callback(agent, state, callback_user_data);
    }
}

static void turbo_ice_owner_on_candidate(
    salts_ice_agent_t *agent, const ice_candidate_t *candidate,
    void *user_data) {
    turbo_ice_owner_t *owner = (turbo_ice_owner_t *)user_data;
    ice_candidate_cb callback;
    void *callback_user_data;

    if (!owner || !candidate) {
        return;
    }
    salts_mutex_lock(&owner->mutex);
    if (owner->local_candidate_count < ICE_MAX_CANDIDATES) {
        owner->local_candidates[owner->local_candidate_count++] = *candidate;
    }
    callback = owner->callbacks.on_candidate;
    callback_user_data = owner->callbacks.user_data;
    salts_mutex_unlock(&owner->mutex);
    if (callback) {
        callback(agent, candidate, callback_user_data);
    }
}

static void turbo_ice_owner_on_data(
    salts_ice_agent_t *agent, const void *data, size_t len, void *user_data) {
    turbo_ice_owner_t *owner = (turbo_ice_owner_t *)user_data;

    if (owner && owner->callbacks.on_data) {
        owner->callbacks.on_data(agent, data, len, owner->callbacks.user_data);
    }
}

static int turbo_ice_owner_execute(
    turbo_ice_owner_t *owner, turbo_ice_command_t command,
    const turbo_ice_command_args_t *args) {
    ice_callbacks_t empty_callbacks = {0};

    if (!owner || !owner->agent) {
        return ICE_AGENT_ERROR_CLOSED;
    }

    switch (command) {
        case TURBO_ICE_COMMAND_GET_LOCAL_CREDENTIALS:
            ice_agent_get_local_credentials(
                owner->agent, args->output_a, args->output_a_len,
                args->output_b, args->output_b_len);
            return 0;
        case TURBO_ICE_COMMAND_SET_REMOTE_CREDENTIALS:
            return ice_agent_set_remote_credentials(
                owner->agent, args->text_a, args->text_b);
        case TURBO_ICE_COMMAND_RESTART:
            return ice_agent_restart(owner->agent, &args->restart_options);
        case TURBO_ICE_COMMAND_GATHER:
            return ice_agent_gather_candidates(owner->agent);
        case TURBO_ICE_COMMAND_ADD_REMOTE_CANDIDATE:
            return ice_agent_add_remote_candidate(owner->agent, args->text_a);
        case TURBO_ICE_COMMAND_END_OF_CANDIDATES:
            if (ice_agent_get_state(owner->agent) == ICE_STATE_CLOSED) {
                return ICE_AGENT_ERROR_CLOSED;
            }
            ice_agent_end_of_candidates(owner->agent);
            return 0;
        case TURBO_ICE_COMMAND_START_CHECKS:
            return ice_agent_start_checks(owner->agent);
        case TURBO_ICE_COMMAND_SEND:
            return ice_agent_send(owner->agent, args->data, args->data_len);
        case TURBO_ICE_COMMAND_SET_ALLOW_LOOPBACK:
            if (ice_agent_get_state(owner->agent) == ICE_STATE_CLOSED) {
                return ICE_AGENT_ERROR_CLOSED;
            }
            ice_agent_set_allow_loopback(owner->agent, args->int_value);
            return 0;
        case TURBO_ICE_COMMAND_SET_ROLE:
            return ice_agent_set_role(owner->agent, args->int_value);
        case TURBO_ICE_COMMAND_DETACH_CALLBACKS:
            ice_agent_set_callbacks(owner->agent, &empty_callbacks);
            return 0;
        case TURBO_ICE_COMMAND_NONE:
        default:
            return ICE_AGENT_ERROR_INVALID_OPTIONS;
    }
}

static int turbo_ice_owner_submit(
    turbo_ice_owner_t *owner, turbo_ice_command_t command,
    const turbo_ice_command_args_t *args) {
    int result;

    if (!owner || !owner->sync_initialized || command == TURBO_ICE_COMMAND_NONE) {
        return ICE_AGENT_ERROR_INVALID_OPTIONS;
    }
    if (turbo_current_ice_owner == owner) {
        return turbo_ice_owner_execute(owner, command, args);
    }

    salts_mutex_lock(&owner->mutex);
    while (!owner->stopping && owner->phase != TURBO_ICE_COMMAND_IDLE) {
        salts_cond_wait(&owner->cond, &owner->mutex);
    }
    if (owner->stopping || !owner->agent) {
        salts_mutex_unlock(&owner->mutex);
        return ICE_AGENT_ERROR_CLOSED;
    }

    owner->command = command;
    owner->args = *args;
    owner->phase = TURBO_ICE_COMMAND_REQUESTED;
    salts_cond_broadcast(&owner->cond);
    while (!owner->stopping && owner->phase != TURBO_ICE_COMMAND_DONE) {
        salts_cond_wait(&owner->cond, &owner->mutex);
    }
    if (owner->phase != TURBO_ICE_COMMAND_DONE) {
        salts_mutex_unlock(&owner->mutex);
        return ICE_AGENT_ERROR_CLOSED;
    }

    result = owner->result;
    owner->phase = TURBO_ICE_COMMAND_IDLE;
    owner->command = TURBO_ICE_COMMAND_NONE;
    memset(&owner->args, 0, sizeof(owner->args));
    salts_cond_broadcast(&owner->cond);
    salts_mutex_unlock(&owner->mutex);
    return result;
}

static void turbo_ice_owner_main(void *argument) {
    turbo_ice_owner_t *owner = (turbo_ice_owner_t *)argument;
    ice_callbacks_t owner_callbacks = {
        .on_state_change = turbo_ice_owner_on_state_change,
        .on_gathering_change = turbo_ice_owner_on_gathering_change,
        .on_candidate = turbo_ice_owner_on_candidate,
        .on_data = turbo_ice_owner_on_data,
        .user_data = owner
    };

    turbo_current_ice_owner = owner;
    owner->agent = ice_agent_create(&owner->config);
    if (owner->agent) {
        owner->cached_state = ice_agent_get_state(owner->agent);
        owner->cached_gathering_state =
            ice_agent_get_gathering_state(owner->agent);
        ice_agent_set_callbacks(owner->agent, &owner_callbacks);
    }

    salts_mutex_lock(&owner->mutex);
    owner->create_failed = owner->agent == NULL;
    owner->ready = 1;
    salts_cond_broadcast(&owner->cond);
    salts_mutex_unlock(&owner->mutex);

    if (!owner->agent) {
        turbo_current_ice_owner = NULL;
        return;
    }

    for (;;) {
        turbo_ice_command_t command = TURBO_ICE_COMMAND_NONE;
        turbo_ice_command_args_t args = {0};
        int async_send = 0;

        salts_mutex_lock(&owner->mutex);
        if (!owner->stopping && owner->phase != TURBO_ICE_COMMAND_REQUESTED &&
            owner->send_count == 0) {
            (void)salts_cond_timedwait(
                &owner->cond, &owner->mutex,
                TURBO_ICE_OWNER_POLL_INTERVAL_NS);
        }
        if (owner->phase == TURBO_ICE_COMMAND_REQUESTED) {
            command = owner->command;
            args = owner->args;
        } else if (owner->send_count > 0) {
            turbo_ice_send_slot_t *slot =
                &owner->send_slots[owner->send_head];
            args.data = slot->data;
            args.data_len = slot->size;
            command = TURBO_ICE_COMMAND_SEND;
            async_send = 1;
        } else if (owner->stopping) {
            salts_mutex_unlock(&owner->mutex);
            break;
        }
        salts_mutex_unlock(&owner->mutex);

        if (command != TURBO_ICE_COMMAND_NONE) {
            int result = turbo_ice_owner_execute(owner, command, &args);
            salts_mutex_lock(&owner->mutex);
            if (!async_send) {
                owner->result = result;
                owner->phase = TURBO_ICE_COMMAND_DONE;
                salts_cond_broadcast(&owner->cond);
            } else {
                owner->send_slots[owner->send_head].size = 0;
                owner->send_head =
                    (owner->send_head + 1) % TURBO_ICE_OWNER_SEND_CAPACITY;
                owner->send_count--;
                salts_cond_broadcast(&owner->cond);
            }
            salts_mutex_unlock(&owner->mutex);
        } else {
            ice_state_t state = ice_agent_get_state(owner->agent);
            if (state == ICE_STATE_CONNECTED || state == ICE_STATE_COMPLETED) {
                ice_agent_poll_selected_pair(
                    owner->agent, TURBO_ICE_OWNER_POLL_INTERVAL_MS);
            }
        }
    }

    ice_agent_destroy(owner->agent);
    owner->agent = NULL;
    turbo_current_ice_owner = NULL;
}

turbo_ice_owner_t *turbo_ice_owner_create(
    const ice_config_t *config, const ice_callbacks_t *callbacks) {
    turbo_ice_owner_t *owner;

    if (!config) {
        return NULL;
    }
    owner = (turbo_ice_owner_t *)calloc(1, sizeof(*owner));
    if (!owner) {
        return NULL;
    }
    owner->config = *config;
    if (callbacks) {
        owner->callbacks = *callbacks;
    }
    salts_mutex_init(&owner->mutex);
    salts_cond_init(&owner->cond);
    owner->sync_initialized = 1;

    if (salts_thread_create(&owner->thread, turbo_ice_owner_main, owner) != 0) {
        salts_cond_destroy(&owner->cond);
        salts_mutex_destroy(&owner->mutex);
        free(owner);
        return NULL;
    }
    owner->thread_started = 1;

    salts_mutex_lock(&owner->mutex);
    while (!owner->ready) {
        salts_cond_wait(&owner->cond, &owner->mutex);
    }
    salts_mutex_unlock(&owner->mutex);
    if (owner->create_failed) {
        turbo_ice_owner_destroy(owner);
        return NULL;
    }
    return owner;
}

void turbo_ice_owner_destroy(turbo_ice_owner_t *owner) {
    turbo_ice_command_args_t args = {0};

    if (!owner) {
        return;
    }
    turbo_ice_owner_close(owner);
    if (owner->agent && !owner->stopping) {
        (void)turbo_ice_owner_submit(
            owner, TURBO_ICE_COMMAND_DETACH_CALLBACKS, &args);
    }
    salts_mutex_lock(&owner->mutex);
    owner->stopping = 1;
    salts_cond_broadcast(&owner->cond);
    salts_mutex_unlock(&owner->mutex);
    if (owner->thread_started) {
        (void)salts_thread_join(&owner->thread);
        salts_thread_destroy(&owner->thread);
    }
    if (owner->sync_initialized) {
        salts_cond_destroy(&owner->cond);
        salts_mutex_destroy(&owner->mutex);
    }
    free(owner);
}

void turbo_ice_owner_close(turbo_ice_owner_t *owner) {
    salts_ice_agent_t *agent;

    if (!owner || !owner->sync_initialized) {
        return;
    }
    salts_mutex_lock(&owner->mutex);
    owner->close_requested = 1;
    agent = owner->agent;
    salts_mutex_unlock(&owner->mutex);
    if (agent) {
        ice_agent_close(agent);
    }
}

int turbo_ice_owner_get_local_credentials(
    turbo_ice_owner_t *owner, char *ufrag, size_t ufrag_len,
    char *pwd, size_t pwd_len) {
    turbo_ice_command_args_t args = {0};
    if (!ufrag || ufrag_len == 0 || !pwd || pwd_len == 0) return -1;
    args.output_a = ufrag;
    args.output_a_len = ufrag_len;
    args.output_b = pwd;
    args.output_b_len = pwd_len;
    return turbo_ice_owner_submit(
        owner, TURBO_ICE_COMMAND_GET_LOCAL_CREDENTIALS, &args);
}

int turbo_ice_owner_set_remote_credentials(
    turbo_ice_owner_t *owner, const char *ufrag, const char *pwd) {
    turbo_ice_command_args_t args = {0};
    if (!ufrag || !pwd) return -1;
    args.text_a = ufrag;
    args.text_b = pwd;
    return turbo_ice_owner_submit(
        owner, TURBO_ICE_COMMAND_SET_REMOTE_CREDENTIALS, &args);
}

int turbo_ice_owner_restart(
    turbo_ice_owner_t *owner, const ice_restart_options_t *options) {
    turbo_ice_command_args_t args = {0};
    if (!options) return ICE_AGENT_ERROR_INVALID_OPTIONS;
    args.restart_options = *options;
    return turbo_ice_owner_submit(owner, TURBO_ICE_COMMAND_RESTART, &args);
}

int turbo_ice_owner_gather_candidates(turbo_ice_owner_t *owner) {
    turbo_ice_command_args_t args = {0};
    return turbo_ice_owner_submit(owner, TURBO_ICE_COMMAND_GATHER, &args);
}

int turbo_ice_owner_add_remote_candidate(
    turbo_ice_owner_t *owner, const char *candidate) {
    turbo_ice_command_args_t args = {0};
    if (!candidate) return -1;
    args.text_a = candidate;
    return turbo_ice_owner_submit(
        owner, TURBO_ICE_COMMAND_ADD_REMOTE_CANDIDATE, &args);
}

int turbo_ice_owner_end_of_candidates(turbo_ice_owner_t *owner) {
    turbo_ice_command_args_t args = {0};
    return turbo_ice_owner_submit(
        owner, TURBO_ICE_COMMAND_END_OF_CANDIDATES, &args);
}

int turbo_ice_owner_start_checks(turbo_ice_owner_t *owner) {
    turbo_ice_command_args_t args = {0};
    return turbo_ice_owner_submit(
        owner, TURBO_ICE_COMMAND_START_CHECKS, &args);
}

int turbo_ice_owner_send(
    turbo_ice_owner_t *owner, const void *data, size_t len) {
    turbo_ice_command_args_t args = {0};
    if (!data || len == 0) return -1;
    args.data = data;
    args.data_len = len;
    return turbo_ice_owner_submit(owner, TURBO_ICE_COMMAND_SEND, &args);
}

int turbo_ice_owner_send_async(
    turbo_ice_owner_t *owner, const void *data, size_t len) {
    turbo_ice_send_slot_t *slot;

    if (!owner || !owner->sync_initialized || !data || len == 0 ||
        len > TURBO_ICE_OWNER_MAX_DATAGRAM_BYTES) {
        return -1;
    }
    if (turbo_current_ice_owner == owner) {
        return turbo_ice_owner_send(owner, data, len);
    }

    salts_mutex_lock(&owner->mutex);
    if (owner->stopping || owner->close_requested || !owner->agent ||
        owner->send_count == TURBO_ICE_OWNER_SEND_CAPACITY) {
        salts_mutex_unlock(&owner->mutex);
        return -1;
    }
    slot = &owner->send_slots[owner->send_tail];
    memcpy(slot->data, data, len);
    slot->size = len;
    owner->send_tail =
        (owner->send_tail + 1) % TURBO_ICE_OWNER_SEND_CAPACITY;
    owner->send_count++;
    salts_cond_signal(&owner->cond);
    salts_mutex_unlock(&owner->mutex);
    return 0;
}

ice_state_t turbo_ice_owner_get_state(turbo_ice_owner_t *owner) {
    ice_state_t state;

    if (!owner || !owner->sync_initialized) {
        return ICE_STATE_CLOSED;
    }
    salts_mutex_lock(&owner->mutex);
    state = owner->cached_state;
    salts_mutex_unlock(&owner->mutex);
    return state;
}

ice_gathering_state_t turbo_ice_owner_get_gathering_state(
    turbo_ice_owner_t *owner) {
    ice_gathering_state_t state;

    if (!owner || !owner->sync_initialized) {
        return ICE_GATHERING_NEW;
    }
    salts_mutex_lock(&owner->mutex);
    state = owner->cached_gathering_state;
    salts_mutex_unlock(&owner->mutex);
    return state;
}

int turbo_ice_owner_get_local_candidate_count(turbo_ice_owner_t *owner) {
    int count;

    if (!owner || !owner->sync_initialized) {
        return -1;
    }
    salts_mutex_lock(&owner->mutex);
    count = owner->local_candidate_count;
    salts_mutex_unlock(&owner->mutex);
    return count;
}

int turbo_ice_owner_get_local_candidate(
    turbo_ice_owner_t *owner, int index, ice_candidate_t *candidate) {
    if (!owner || !owner->sync_initialized || index < 0 || !candidate) {
        return -1;
    }
    salts_mutex_lock(&owner->mutex);
    if (index >= owner->local_candidate_count) {
        salts_mutex_unlock(&owner->mutex);
        return -1;
    }
    *candidate = owner->local_candidates[index];
    salts_mutex_unlock(&owner->mutex);
    return 0;
}

int turbo_ice_owner_set_allow_loopback(turbo_ice_owner_t *owner, int allow) {
    turbo_ice_command_args_t args = {0};
    args.int_value = allow ? 1 : 0;
    return turbo_ice_owner_submit(
        owner, TURBO_ICE_COMMAND_SET_ALLOW_LOOPBACK, &args);
}

int turbo_ice_owner_set_role(turbo_ice_owner_t *owner, int is_controlling) {
    turbo_ice_command_args_t args = {0};
    args.int_value = is_controlling ? 1 : 0;
    return turbo_ice_owner_submit(owner, TURBO_ICE_COMMAND_SET_ROLE, &args);
}
