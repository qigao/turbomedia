#include "turbo_frame_store.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FS_FREE=0, FS_READY=1, FS_RETIRING=2 } fs_state;
typedef struct {
    uint8_t *memory;
    size_t length;
    uint64_t frame_id;
    uint32_t slot_generation;
    uint64_t session_generation;
    uint64_t display_generation;
    uint32_t pins;
    uint32_t leases;
    fs_state state;
} fs_slot;
typedef struct { uint64_t token; uint32_t slot; uint32_t generation; } fs_credit;

struct tm_fs_store {
    tm_fs_config_v1 config;
    uint64_t generation;
    uint64_t next_frame_id;
    uint64_t next_ticket_id;
    fs_slot *slots;
    fs_credit *pins;
    fs_credit *leases;
    uint32_t cursor;
    int sealed;
};

static _Atomic uint64_t fs_next_generation = 1u;

static tm_fs_status ref_index(tm_fs_store *s, const tm_fs_frame_ref_v1 *ref, uint32_t *idx) {
    fs_slot *slot;
    if (!s || !ref || !idx) return TM_FS_EINVAL;
    if (ref->version != TM_FS_ABI_V1) return TM_FS_EVERSION;
    if (ref->size != sizeof(*ref)) return TM_FS_EINVAL;
    if (ref->store_id != s->config.store_id || ref->store_generation != s->generation ||
        ref->slot >= s->config.slot_capacity) return TM_FS_ESTALE;
    slot = &s->slots[ref->slot];
    if (slot->state == FS_FREE || slot->slot_generation != ref->slot_generation ||
        slot->frame_id != ref->frame_id ||
        slot->session_generation != ref->source_session_generation ||
        slot->display_generation != ref->display_generation) return TM_FS_ESTALE;
    *idx = ref->slot;
    return TM_FS_OK;
}

static tm_fs_status credit_lookup(tm_fs_store *s, uint64_t sid, uint64_t generation,
                                  uint64_t token, fs_credit *rows, uint32_t capacity,
                                  uint32_t *index) {
    uint32_t i;
    if (!s || !token || !index) return TM_FS_EINVAL;
    if (sid != s->config.store_id || generation != s->generation) return TM_FS_ESTALE;
    for (i = 0u; i < capacity; ++i)
        if (rows[i].token == token) { *index = i; return TM_FS_OK; }
    return TM_FS_ESTALE;
}

static void maybe_retire(fs_slot *slot) {
    if (slot->state == FS_RETIRING && !slot->pins && !slot->leases) {
        slot->state = FS_FREE;
        slot->length = 0u;
    }
}

static tm_fs_status credit_alloc(tm_fs_store *s, fs_credit *rows, uint32_t capacity,
                                 uint32_t slot, uint32_t generation, uint64_t *out_ticket) {
    uint32_t i;
    for (i = 0u; i < capacity; ++i) {
        if (!rows[i].token) {
            if (s->next_ticket_id == UINT64_MAX) return TM_FS_EEXHAUSTED;
            rows[i].token = ++s->next_ticket_id;
            rows[i].slot = slot;
            rows[i].generation = generation;
            *out_ticket = rows[i].token;
            return TM_FS_OK;
        }
    }
    return TM_FS_EFULL;
}

tm_fs_status tm_fs_create(const tm_fs_config_v1 *c, tm_fs_store **out) {
    tm_fs_store *s;
    uint32_t i;
    uint64_t gen;
    if (!out || *out != NULL || !c) return TM_FS_EINVAL;
    if (c->version != TM_FS_ABI_V1) return TM_FS_EVERSION;
    if (c->size != sizeof(*c) || !c->store_id || !c->slot_capacity ||
        c->slot_capacity > TM_FS_MAX_SLOTS || !c->max_frame_bytes ||
        c->max_frame_bytes > TM_FS_MAX_FRAME_BYTES || !c->max_pins ||
        c->max_pins > TM_FS_MAX_CREDITS || !c->max_leases ||
        c->max_leases > TM_FS_MAX_CREDITS ||
        (uint64_t)c->slot_capacity * c->max_frame_bytes > c->max_retained_bytes)
        return TM_FS_EINVAL;
    gen = atomic_fetch_add_explicit(&fs_next_generation, 1u, memory_order_relaxed);
    if (!gen || gen == UINT64_MAX) return TM_FS_EEXHAUSTED;
    s = (tm_fs_store *)calloc(1u, sizeof(*s));
    if (!s) return TM_FS_ENOMEM;
    s->config = *c;
    s->generation = gen;
    s->slots = (fs_slot *)calloc(c->slot_capacity, sizeof(fs_slot));
    s->pins = (fs_credit *)calloc(c->max_pins, sizeof(fs_credit));
    s->leases = (fs_credit *)calloc(c->max_leases, sizeof(fs_credit));
    if (!s->slots || !s->pins || !s->leases) goto fail;
    for (i = 0u; i < c->slot_capacity; ++i) {
        s->slots[i].memory = (uint8_t *)malloc(c->max_frame_bytes);
        if (!s->slots[i].memory) goto fail;
    }
    *out = s;
    return TM_FS_OK;
fail:
    if (s->slots) for (i = 0u; i < c->slot_capacity; ++i) free(s->slots[i].memory);
    free(s->slots); free(s->pins); free(s->leases); free(s);
    return TM_FS_ENOMEM;
}

tm_fs_status tm_fs_publish_cpu(tm_fs_store *s, const void *data, size_t size_bytes,
                                uint64_t session, uint64_t display,
                                tm_fs_frame_ref_v1 *out) {
    uint32_t step;
    if (!s || !out || !data || !size_bytes || !session || !display ||
        size_bytes > s->config.max_frame_bytes) return TM_FS_EINVAL;
    memset(out, 0, sizeof(*out));
    if (s->sealed) return TM_FS_ESHUTDOWN;
    for (step = 0u; step < s->config.slot_capacity; ++step) {
        uint32_t index = (s->cursor + step) % s->config.slot_capacity;
        fs_slot *slot = &s->slots[index];
        if (slot->state != FS_FREE || slot->slot_generation == UINT32_MAX) continue;
        if (s->next_frame_id == UINT64_MAX) return TM_FS_EEXHAUSTED;
        memcpy(slot->memory, data, size_bytes);
        slot->length = size_bytes;
        slot->slot_generation++;
        slot->frame_id = ++s->next_frame_id;
        slot->session_generation = session;
        slot->display_generation = display;
        slot->state = FS_READY;
        s->cursor = (index + 1u) % s->config.slot_capacity;
        out->size = sizeof(*out);
        out->version = TM_FS_ABI_V1;
        out->store_id = s->config.store_id;
        out->store_generation = s->generation;
        out->slot = index;
        out->slot_generation = slot->slot_generation;
        out->frame_id = slot->frame_id;
        out->source_session_generation = session;
        out->display_generation = display;
        return TM_FS_OK;
    }
    return TM_FS_EFULL;
}

tm_fs_status tm_fs_reserve_pin(tm_fs_store *s, const tm_fs_frame_ref_v1 *ref,
                                tm_fs_pin_v1 *out) {
    uint32_t index;
    tm_fs_status rc;
    if (!s || !out) return TM_FS_EINVAL;
    memset(out, 0, sizeof(*out));
    if (s->sealed) return TM_FS_ESHUTDOWN;
    rc = ref_index(s, ref, &index);
    if (rc != TM_FS_OK) return rc;
    if (s->slots[index].state != FS_READY) return TM_FS_EBUSY;
    rc = credit_alloc(s, s->pins, s->config.max_pins, index,
                      s->slots[index].slot_generation, &out->ticket_id);
    if (rc != TM_FS_OK) return rc;
    s->slots[index].pins++;
    out->store_id = s->config.store_id;
    out->store_generation = s->generation;
    return TM_FS_OK;
}

static tm_fs_status acquire_slot(tm_fs_store *s, uint32_t slot, tm_fs_lease_v1 *out) {
    uint64_t token = 0;
    tm_fs_status rc;
    rc = credit_alloc(s, s->leases, s->config.max_leases, slot,
                      s->slots[slot].slot_generation, &token);
    if (rc != TM_FS_OK) return rc;
    s->slots[slot].leases++;
    out->store_id = s->config.store_id;
    out->store_generation = s->generation;
    out->ticket_id = token;
    return TM_FS_OK;
}

tm_fs_status tm_fs_acquire_weak(tm_fs_store *s, const tm_fs_frame_ref_v1 *ref,
                                tm_fs_lease_v1 *out) {
    uint32_t index;
    tm_fs_status rc;
    if (!s || !out) return TM_FS_EINVAL;
    memset(out, 0, sizeof(*out));
    if (s->sealed) return TM_FS_ESHUTDOWN;
    rc = ref_index(s, ref, &index);
    if (rc != TM_FS_OK) return rc;
    if (s->slots[index].state != FS_READY) return TM_FS_EBUSY;
    return acquire_slot(s, index, out);
}

tm_fs_status tm_fs_acquire_pinned(tm_fs_store *s, const tm_fs_pin_v1 *pin,
                                  tm_fs_lease_v1 *out) {
    uint32_t credit;
    tm_fs_status rc;
    if (!s || !pin || !out) return TM_FS_EINVAL;
    memset(out, 0, sizeof(*out));
    rc = credit_lookup(s, pin->store_id, pin->store_generation, pin->ticket_id,
                       s->pins, s->config.max_pins, &credit);
    if (rc != TM_FS_OK) return rc;
    return acquire_slot(s, s->pins[credit].slot, out);
}

tm_fs_status tm_fs_map_cpu(tm_fs_store *s, const tm_fs_lease_v1 *lease,
                           tm_fs_cpu_view_v1 *out) {
    uint32_t credit;
    tm_fs_status rc;
    if (!s || !lease || !out) return TM_FS_EINVAL;
    if (out->version != TM_FS_ABI_V1) return TM_FS_EVERSION;
    if (out->size != sizeof(*out)) return TM_FS_EINVAL;
    out->data = NULL;
    out->size_bytes = 0u;
    rc = credit_lookup(s, lease->store_id, lease->store_generation, lease->ticket_id,
                       s->leases, s->config.max_leases, &credit);
    if (rc != TM_FS_OK) return rc;
    out->data = s->slots[s->leases[credit].slot].memory;
    out->size_bytes = s->slots[s->leases[credit].slot].length;
    return TM_FS_OK;
}

tm_fs_status tm_fs_release_lease(tm_fs_store *s, const tm_fs_lease_v1 *lease) {
    uint32_t credit, index;
    tm_fs_status rc;
    if (!s || !lease) return TM_FS_EINVAL;
    rc = credit_lookup(s, lease->store_id, lease->store_generation, lease->ticket_id,
                       s->leases, s->config.max_leases, &credit);
    if (rc != TM_FS_OK) return rc;
    index = s->leases[credit].slot;
    memset(&s->leases[credit], 0, sizeof(fs_credit));
    s->slots[index].leases--;
    maybe_retire(&s->slots[index]);
    return TM_FS_OK;
}

tm_fs_status tm_fs_release_pin(tm_fs_store *s, const tm_fs_pin_v1 *pin) {
    uint32_t credit, index;
    tm_fs_status rc;
    if (!s || !pin) return TM_FS_EINVAL;
    rc = credit_lookup(s, pin->store_id, pin->store_generation, pin->ticket_id,
                       s->pins, s->config.max_pins, &credit);
    if (rc != TM_FS_OK) return rc;
    index = s->pins[credit].slot;
    memset(&s->pins[credit], 0, sizeof(fs_credit));
    s->slots[index].pins--;
    maybe_retire(&s->slots[index]);
    return TM_FS_OK;
}

tm_fs_status tm_fs_retire(tm_fs_store *s, const tm_fs_frame_ref_v1 *ref) {
    uint32_t index;
    tm_fs_status rc = ref_index(s, ref, &index);
    if (rc != TM_FS_OK) return rc;
    s->slots[index].state = FS_RETIRING;
    maybe_retire(&s->slots[index]);
    return s->slots[index].state == FS_FREE ? TM_FS_OK : TM_FS_EBUSY;
}

tm_fs_status tm_fs_seal(tm_fs_store *s) {
    if (!s) return TM_FS_EINVAL;
    s->sealed = 1;
    return TM_FS_OK;
}

tm_fs_status tm_fs_destroy(tm_fs_store *s) {
    uint32_t i;
    if (!s) return TM_FS_EINVAL;
    if (!s->sealed) return TM_FS_EBUSY;
    for (i = 0u; i < s->config.slot_capacity; ++i)
        if (s->slots[i].pins || s->slots[i].leases) return TM_FS_EBUSY;
    for (i = 0u; i < s->config.slot_capacity; ++i) free(s->slots[i].memory);
    free(s->slots); free(s->pins); free(s->leases); free(s);
    return TM_FS_OK;
}
