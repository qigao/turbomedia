#include "ivr_certificate_identity.h"

#include "platform.h"
#include "salts_error.h"
#include <salts/clock.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char worker_id[IVR_CERTIFICATE_IDENTITY_MAX_WORKER_ID + 1u];
    char active_certificate_sha256[IVR_CERTIFICATE_SHA256_TEXT_SIZE];
    char previous_certificate_sha256[IVR_CERTIFICATE_SHA256_TEXT_SIZE];
    uint64_t previous_expires_at_ms;
    uint64_t generation;
} ivr_certificate_identity_entry_owned_t;

struct ivr_certificate_identity_s {
    ivr_certificate_identity_entry_owned_t *entries;
    size_t entry_count;
    ivr_certificate_identity_clock_fn clock;
    void *clock_context;
};

static uint64_t identity_now_ms(const ivr_certificate_identity_t *identity) {
    return identity->clock ? identity->clock(identity->clock_context)
                           : salts_realtime_ms();
}

static int fingerprint_valid(const char *value) {
    size_t i;
    if (!value || strlen(value) != IVR_CERTIFICATE_SHA256_TEXT_SIZE - 1u ||
        memcmp(value, "sha256:", 7u) != 0) {
        return 0;
    }
    for (i = 7u; i < IVR_CERTIFICATE_SHA256_TEXT_SIZE - 1u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int entry_valid(const ivr_certificate_identity_entry_t *entry,
                       uint64_t now_ms) {
    if (!entry || !entry->worker_id || entry->worker_id[0] == '\0' ||
        strlen(entry->worker_id) > IVR_CERTIFICATE_IDENTITY_MAX_WORKER_ID ||
        !fingerprint_valid(entry->active_certificate_sha256) ||
        entry->generation == 0u) {
        return 0;
    }
    if (entry->previous_certificate_sha256) {
        if (!fingerprint_valid(entry->previous_certificate_sha256) ||
            entry->previous_expires_at_ms <= now_ms ||
            strcmp(entry->previous_certificate_sha256,
                   entry->active_certificate_sha256) == 0) {
            return 0;
        }
    } else if (entry->previous_expires_at_ms != 0u) {
        return 0;
    }
    return 1;
}

int ivr_certificate_identity_create(
    const ivr_certificate_identity_config_t *config,
    ivr_certificate_identity_t **out_identity) {
    ivr_certificate_identity_t *identity;
    uint64_t now_ms;
    size_t i;
    if (out_identity) *out_identity = NULL;
    if (!config || config->size < sizeof(*config) || !out_identity ||
        !config->entries || config->entry_count == 0u ||
        config->entry_count > IVR_CERTIFICATE_IDENTITY_MAX_ENTRIES) {
        return SALTS_EINVAL;
    }
    identity = (ivr_certificate_identity_t *)calloc(1u, sizeof(*identity));
    if (!identity) return SALTS_ENOMEM;
    identity->entries = (ivr_certificate_identity_entry_owned_t *)calloc(
        config->entry_count, sizeof(*identity->entries));
    if (!identity->entries) {
        free(identity);
        return SALTS_ENOMEM;
    }
    identity->entry_count = config->entry_count;
    identity->clock = config->clock;
    identity->clock_context = config->clock_context;
    now_ms = identity_now_ms(identity);
    for (i = 0u; i < config->entry_count; ++i) {
        const ivr_certificate_identity_entry_t *entry = &config->entries[i];
        size_t j;
        if (!entry_valid(entry, now_ms)) goto invalid;
        for (j = 0u; j < i; ++j) {
            const ivr_certificate_identity_entry_owned_t *prior =
                &identity->entries[j];
            if (strcmp(prior->worker_id, entry->worker_id) == 0 ||
                strcmp(prior->active_certificate_sha256,
                       entry->active_certificate_sha256) == 0 ||
                (entry->previous_certificate_sha256 &&
                 strcmp(prior->active_certificate_sha256,
                        entry->previous_certificate_sha256) == 0) ||
                (prior->previous_certificate_sha256[0] != '\0' &&
                 strcmp(prior->previous_certificate_sha256,
                        entry->active_certificate_sha256) == 0) ||
                (entry->previous_certificate_sha256 &&
                 prior->previous_certificate_sha256[0] != '\0' &&
                 strcmp(prior->previous_certificate_sha256,
                        entry->previous_certificate_sha256) == 0)) {
                goto invalid;
            }
        }
        snprintf(identity->entries[i].worker_id,
                 sizeof(identity->entries[i].worker_id), "%s", entry->worker_id);
        snprintf(identity->entries[i].active_certificate_sha256,
                 sizeof(identity->entries[i].active_certificate_sha256), "%s",
                 entry->active_certificate_sha256);
        if (entry->previous_certificate_sha256) {
            snprintf(identity->entries[i].previous_certificate_sha256,
                     sizeof(identity->entries[i].previous_certificate_sha256), "%s",
                     entry->previous_certificate_sha256);
        }
        identity->entries[i].previous_expires_at_ms = entry->previous_expires_at_ms;
        identity->entries[i].generation = entry->generation;
    }
    *out_identity = identity;
    return SALTS_OK;

invalid:
    ivr_certificate_identity_destroy(identity);
    return SALTS_EINVAL;
}

void ivr_certificate_identity_destroy(ivr_certificate_identity_t *identity) {
    if (!identity) return;
    if (identity->entries) {
        memset(identity->entries, 0, identity->entry_count * sizeof(*identity->entries));
        free(identity->entries);
    }
    memset(identity, 0, sizeof(*identity));
    free(identity);
}

int ivr_certificate_identity_verify(void *context,
                                    const char *certificate_sha256,
                                    const char *claimed_identity) {
    ivr_certificate_identity_t *identity = (ivr_certificate_identity_t *)context;
    uint64_t now_ms;
    size_t i;
    if (!identity || !fingerprint_valid(certificate_sha256) || !claimed_identity ||
        claimed_identity[0] == '\0') {
        return SALTS_EINVAL;
    }
    now_ms = identity_now_ms(identity);
    for (i = 0u; i < identity->entry_count; ++i) {
        const ivr_certificate_identity_entry_owned_t *entry = &identity->entries[i];
        if (strcmp(entry->worker_id, claimed_identity) != 0) continue;
        if (strcmp(entry->active_certificate_sha256, certificate_sha256) == 0 ||
            (entry->previous_certificate_sha256[0] != '\0' &&
             now_ms < entry->previous_expires_at_ms &&
             strcmp(entry->previous_certificate_sha256, certificate_sha256) == 0)) {
            return SALTS_OK;
        }
        return SALTS_EPERM;
    }
    return SALTS_EPERM;
}
