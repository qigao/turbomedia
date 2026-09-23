#include "turbo_media_revocation.h"


#include <stdlib.h>
#include <string.h>

struct turbo_media_revocation_state_s {
    uint64_t epoch;
    uint64_t sequence;
    uint64_t required_epoch;
    uint64_t required_sequence;
    size_t count;
    size_t max_entries;
    int synchronized;
    uint8_t *digests;
};

static int revocation_hex_nibble(char value, uint8_t *output) {
    if (value >= '0' && value <= '9') {
        *output = (uint8_t)(value - '0');
        return 0;
    }
    if (value >= 'a' && value <= 'f') {
        *output = (uint8_t)(value - 'a' + 10);
        return 0;
    }
    return -1;
}

static int revocation_parse_digest(
    const char *value,
    uint8_t output[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES]) {
    size_t index;

    if (!value ||
        strlen(value) != TURBO_MEDIA_REVOCATION_SHA256_HEX_BYTES) {
        return -1;
    }
    for (index = 0; index < TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES; ++index) {
        uint8_t high;
        uint8_t low;
        if (revocation_hex_nibble(value[index * 2U], &high) != 0 ||
            revocation_hex_nibble(value[index * 2U + 1U], &low) != 0) {
            return -1;
        }
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return 0;
}

static int revocation_digest_equal(const uint8_t *left,
                                   const uint8_t *right) {
    return memcmp(left, right, TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES) == 0;
}

static int revocation_find(const turbo_media_revocation_state_t *state,
                           const uint8_t *digest) {
    size_t index;

    if (!state || !digest) {
        return 0;
    }
    for (index = 0; index < state->count; ++index) {
        const uint8_t *entry =
            state->digests +
            index * TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES;
        if (revocation_digest_equal(entry, digest)) {
            return 1;
        }
    }
    return 0;
}

static int revocation_version_less(uint64_t epoch, uint64_t sequence,
                                   uint64_t other_epoch,
                                   uint64_t other_sequence) {
    return epoch < other_epoch ||
           (epoch == other_epoch && sequence < other_sequence);
}

static void revocation_require_snapshot(
    turbo_media_revocation_state_t *state,
    uint64_t epoch,
    uint64_t sequence) {
    if (!state) {
        return;
    }
    if (state->required_epoch == 0U ||
        revocation_version_less(state->required_epoch,
                                state->required_sequence,
                                epoch, sequence)) {
        state->required_epoch = epoch;
        state->required_sequence = sequence;
    }
    state->synchronized = 0;
}

turbo_media_revocation_state_t *turbo_media_revocation_state_create(
    size_t max_entries) {
    turbo_media_revocation_state_t *state;

    if (max_entries == 0U ||
        max_entries > TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS ||
        max_entries > SIZE_MAX / TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES) {
        return NULL;
    }

    state = (turbo_media_revocation_state_t *)calloc(1U, sizeof(*state));
    if (!state) {
        return NULL;
    }
    state->digests = (uint8_t *)calloc(
        max_entries, TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES);
    if (!state->digests) {
        free(state);
        return NULL;
    }
    state->max_entries = max_entries;
    return state;
}

void turbo_media_revocation_state_destroy(
    turbo_media_revocation_state_t *state) {
    if (!state) {
        return;
    }
    if (state->digests) {
        memset(state->digests, 0,
               state->max_entries *
                   TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES);
    }
    free(state->digests);
    memset(state, 0, sizeof(*state));
    free(state);
}

turbo_media_revocation_apply_result_t turbo_media_revocation_apply_snapshot(
    turbo_media_revocation_state_t *state,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count) {
    uint8_t *parsed = NULL;
    size_t index;
    size_t previous;

    if (!state || epoch == 0U ||
        (count > 0U && !sha256_hex)) {
        return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
    }

    if (state->synchronized) {
        if (epoch < state->epoch ||
            (epoch == state->epoch &&
             sequence <= state->sequence)) {
            return TURBO_MEDIA_REVOCATION_APPLY_STALE;
        }
    } else if (state->required_epoch != 0U &&
               revocation_version_less(
                   epoch, sequence,
                   state->required_epoch,
                   state->required_sequence)) {
        return TURBO_MEDIA_REVOCATION_APPLY_STALE;
    }

    if (count > state->max_entries) {
        revocation_require_snapshot(state, epoch, sequence);
        return TURBO_MEDIA_REVOCATION_APPLY_LIMIT;
    }

    if (count > 0U) {
        parsed = (uint8_t *)calloc(
            count, TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES);
        if (!parsed) {
            revocation_require_snapshot(state, epoch, sequence);
            return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
        }
    }

    for (index = 0; index < count; ++index) {
        uint8_t *entry =
            parsed + index * TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES;
        if (revocation_parse_digest(sha256_hex[index], entry) != 0) {
            free(parsed);
            revocation_require_snapshot(state, epoch, sequence);
            return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
        }
        for (previous = 0; previous < index; ++previous) {
            const uint8_t *existing =
                parsed +
                previous * TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES;
            if (revocation_digest_equal(entry, existing)) {
                free(parsed);
                revocation_require_snapshot(state, epoch, sequence);
                return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
            }
        }
    }

    memset(state->digests, 0,
           state->max_entries * TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES);
    if (count > 0U) {
        memcpy(state->digests, parsed,
               count * TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES);
    }
    free(parsed);

    state->epoch = epoch;
    state->sequence = sequence;
    state->required_epoch = 0U;
    state->required_sequence = 0U;
    state->count = count;
    state->synchronized = 1;
    return TURBO_MEDIA_REVOCATION_APPLY_APPLIED;
}

turbo_media_revocation_apply_result_t turbo_media_revocation_apply_revoke(
    turbo_media_revocation_state_t *state,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex) {
    uint8_t digest[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];

    if (!state || epoch == 0U || sequence == 0U ||
        !sha256_hex) {
        return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
    }

    if (!state->synchronized) {
        revocation_require_snapshot(state, epoch, sequence);
        if (revocation_parse_digest(sha256_hex, digest) != 0) {
            return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
        }
        return TURBO_MEDIA_REVOCATION_APPLY_GAP;
    }

    if (epoch < state->epoch ||
        (epoch == state->epoch && sequence <= state->sequence)) {
        return TURBO_MEDIA_REVOCATION_APPLY_STALE;
    }

    if (epoch != state->epoch ||
        sequence != state->sequence + 1U) {
        revocation_require_snapshot(state, epoch, sequence);
        return TURBO_MEDIA_REVOCATION_APPLY_GAP;
    }

    if (revocation_parse_digest(sha256_hex, digest) != 0) {
        revocation_require_snapshot(state, epoch, sequence);
        return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
    }

    if (!revocation_find(state, digest)) {
        uint8_t *target;
        if (state->count >= state->max_entries) {
            revocation_require_snapshot(state, epoch, sequence);
            return TURBO_MEDIA_REVOCATION_APPLY_LIMIT;
        }
        target =
            state->digests +
            state->count * TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES;
        memcpy(target, digest, TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES);
        state->count++;
    }

    state->sequence = sequence;
    return TURBO_MEDIA_REVOCATION_APPLY_APPLIED;
}

int turbo_media_revocation_is_synchronized(
    const turbo_media_revocation_state_t *state) {
    return state && state->synchronized;
}

uint64_t turbo_media_revocation_epoch(
    const turbo_media_revocation_state_t *state) {
    return state ? state->epoch : 0U;
}

uint64_t turbo_media_revocation_sequence(
    const turbo_media_revocation_state_t *state) {
    return state ? state->sequence : 0U;
}

size_t turbo_media_revocation_count(
    const turbo_media_revocation_state_t *state) {
    return state ? state->count : 0U;
}

turbo_media_auth_revocation_status_t turbo_media_revocation_check_digest(
    void *context,
    const uint8_t *sha256,
    size_t sha256_size) {
    turbo_media_revocation_state_t *state =
        (turbo_media_revocation_state_t *)context;

    if (!state || !sha256 ||
        sha256_size != TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES ||
        !state->synchronized) {
        return TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN;
    }
    return revocation_find(state, sha256)
               ? TURBO_MEDIA_AUTH_REVOCATION_REVOKED
               : TURBO_MEDIA_AUTH_REVOCATION_CLEAR;
}
