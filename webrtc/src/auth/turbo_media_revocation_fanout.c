#include "turbo_media_revocation_fanout.h"

#include <stdlib.h>
#include <string.h>

typedef struct fanout_target_state_s {
    char target_id[TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES];
    void *target_context;
    turbo_media_revocation_fanout_target_state_t state;
    turbo_media_revocation_fanout_transport_result_t last_transport_result;
    uint64_t epoch;
    uint64_t sequence;
    unsigned int attempts;
} fanout_target_state_t;

struct turbo_media_revocation_fanout_s {
    turbo_media_revocation_fanout_config_t config;
    fanout_target_state_t *targets;
    size_t target_count;
    int canonical_initialized;
    uint64_t canonical_epoch;
    uint64_t canonical_sequence;
};

static int fanout_id_valid(const char *value) {
    size_t length;

    if (!value || value[0] == '\0') {
        return 0;
    }
    length = strlen(value);
    if (length >= TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES) {
        return 0;
    }
    for (size_t index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch == '.' || ch == ':')) {
            return 0;
        }
    }
    return 1;
}

static int digest_hex_valid(const char *value) {
    if (!value || strlen(value) != 64U) {
        return 0;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int snapshot_contains(
    const char *const *sha256_hex, size_t count, const char *digest) {
    if (!digest || (count > 0U && !sha256_hex)) {
        return 0;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(sha256_hex[index], digest) == 0) {
            return 1;
        }
    }
    return 0;
}

static int snapshot_payload_valid(
    const char *const *sha256_hex, size_t count) {
    if (count > TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS ||
        (count > 0U && !sha256_hex)) {
        return 0;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (!digest_hex_valid(sha256_hex[index])) {
            return 0;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (strcmp(sha256_hex[index], sha256_hex[prior]) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int version_less(uint64_t epoch, uint64_t sequence,
                        uint64_t other_epoch, uint64_t other_sequence) {
    return epoch < other_epoch ||
           (epoch == other_epoch && sequence < other_sequence);
}

static int response_covers(
    const turbo_media_revocation_fanout_response_t *response,
    uint64_t epoch, uint64_t sequence) {
    return response && response->synchronized &&
           !version_less(response->epoch, response->sequence,
                         epoch, sequence);
}

static void target_mark_failed(
    fanout_target_state_t *target,
    turbo_media_revocation_fanout_transport_result_t result,
    unsigned int attempts) {
    if (!target) {
        return;
    }
    target->state = TURBO_MEDIA_REVOCATION_FANOUT_TARGET_FAILED;
    target->last_transport_result = result;
    target->attempts = attempts;
}

static void target_mark_synchronized(
    fanout_target_state_t *target,
    turbo_media_revocation_fanout_target_state_t state,
    turbo_media_revocation_fanout_transport_result_t result,
    unsigned int attempts,
    const turbo_media_revocation_fanout_response_t *response) {
    if (!target || !response) {
        return;
    }
    target->state = state;
    target->last_transport_result = result;
    target->attempts = attempts;
    target->epoch = response->epoch;
    target->sequence = response->sequence;
}

static int send_snapshot_bounded(
    turbo_media_revocation_fanout_t *fanout,
    fanout_target_state_t *target,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count,
    turbo_media_revocation_fanout_target_state_t success_state) {
    turbo_media_revocation_fanout_transport_result_t result =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    turbo_media_revocation_fanout_response_t response;
    unsigned int attempt;

    for (attempt = 1U; attempt <= fanout->config.max_attempts; ++attempt) {
        memset(&response, 0, sizeof(response));
        result = fanout->config.send_snapshot(
            fanout->config.transport_context, target->target_context,
            target->target_id, epoch, sequence, sha256_hex, count,
            attempt, &response);
        if ((result == TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED ||
             result == TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE) &&
            response_covers(&response, epoch, sequence)) {
            target_mark_synchronized(
                target, success_state, result, attempt, &response);
            return 0;
        }
        if (result != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE) {
            break;
        }
    }
    target_mark_failed(target, result, attempt > fanout->config.max_attempts
                                           ? fanout->config.max_attempts
                                           : attempt);
    return -1;
}

static int send_revoke_bounded(
    turbo_media_revocation_fanout_t *fanout,
    fanout_target_state_t *target,
    uint64_t epoch, uint64_t sequence,
    const char *sha256_hex) {
    turbo_media_revocation_fanout_transport_result_t result =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    turbo_media_revocation_fanout_response_t response;
    unsigned int attempt;

    for (attempt = 1U; attempt <= fanout->config.max_attempts; ++attempt) {
        memset(&response, 0, sizeof(response));
        result = fanout->config.send_revoke(
            fanout->config.transport_context, target->target_context,
            target->target_id, epoch, sequence, sha256_hex,
            attempt, &response);
        if ((result == TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED ||
             result == TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE) &&
            response_covers(&response, epoch, sequence)) {
            target_mark_synchronized(
                target, TURBO_MEDIA_REVOCATION_FANOUT_TARGET_SYNCHRONIZED,
                result, attempt, &response);
            return 0;
        }
        if (result != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE) {
            break;
        }
    }

    target->last_transport_result = result;
    target->attempts = attempt > fanout->config.max_attempts
                           ? fanout->config.max_attempts
                           : attempt;
    return -1;
}

static void fill_report(
    const turbo_media_revocation_fanout_t *fanout,
    uint64_t epoch, uint64_t sequence,
    turbo_media_revocation_fanout_report_t *report) {
    if (!report) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->epoch = epoch;
    report->sequence = sequence;
    report->target_count = fanout ? fanout->target_count : 0U;
    if (!fanout) {
        return;
    }
    for (size_t index = 0U; index < fanout->target_count; ++index) {
        const fanout_target_state_t *target = &fanout->targets[index];
        if (target->state == TURBO_MEDIA_REVOCATION_FANOUT_TARGET_SYNCHRONIZED) {
            report->synchronized_count++;
        } else if (target->state ==
                   TURBO_MEDIA_REVOCATION_FANOUT_TARGET_RECOVERED) {
            report->synchronized_count++;
            report->recovered_count++;
        } else if (target->state ==
                   TURBO_MEDIA_REVOCATION_FANOUT_TARGET_FAILED) {
            report->failed_count++;
        }
    }
}

turbo_media_revocation_fanout_t *turbo_media_revocation_fanout_create(
    const turbo_media_revocation_fanout_config_t *config,
    const turbo_media_revocation_fanout_target_t *targets,
    size_t target_count) {
    turbo_media_revocation_fanout_t *fanout;

    if (!config || !targets || target_count == 0U ||
        target_count > TURBO_MEDIA_REVOCATION_FANOUT_MAX_TARGETS ||
        config->max_attempts == 0U ||
        config->max_attempts > TURBO_MEDIA_REVOCATION_FANOUT_MAX_ATTEMPTS ||
        !config->send_snapshot || !config->send_revoke) {
        return NULL;
    }
    fanout = (turbo_media_revocation_fanout_t *)calloc(1U, sizeof(*fanout));
    if (!fanout) {
        return NULL;
    }
    fanout->targets = (fanout_target_state_t *)calloc(
        target_count, sizeof(*fanout->targets));
    if (!fanout->targets) {
        free(fanout);
        return NULL;
    }
    fanout->config = *config;
    fanout->target_count = target_count;
    for (size_t index = 0U; index < target_count; ++index) {
        if (!fanout_id_valid(targets[index].target_id)) {
            turbo_media_revocation_fanout_destroy(fanout);
            return NULL;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (strcmp(targets[index].target_id,
                       fanout->targets[prior].target_id) == 0) {
                turbo_media_revocation_fanout_destroy(fanout);
                return NULL;
            }
        }
        memcpy(fanout->targets[index].target_id, targets[index].target_id,
               strlen(targets[index].target_id) + 1U);
        fanout->targets[index].target_context =
            targets[index].target_context;
        fanout->targets[index].state =
            TURBO_MEDIA_REVOCATION_FANOUT_TARGET_UNKNOWN;
        fanout->targets[index].last_transport_result =
            TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    return fanout;
}

void turbo_media_revocation_fanout_destroy(
    turbo_media_revocation_fanout_t *fanout) {
    if (!fanout) {
        return;
    }
    if (fanout->targets) {
        memset(fanout->targets, 0,
               fanout->target_count * sizeof(*fanout->targets));
    }
    free(fanout->targets);
    memset(fanout, 0, sizeof(*fanout));
    free(fanout);
}

int turbo_media_revocation_fanout_publish_snapshot(
    turbo_media_revocation_fanout_t *fanout,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count,
    turbo_media_revocation_fanout_report_t *report) {
    size_t failed = 0U;

    if (!fanout || epoch == 0U ||
        !snapshot_payload_valid(sha256_hex, count) ||
        (fanout->canonical_initialized &&
         version_less(epoch, sequence,
                      fanout->canonical_epoch,
                      fanout->canonical_sequence))) {
        return -1;
    }
    if (!fanout->canonical_initialized ||
        version_less(fanout->canonical_epoch,
                     fanout->canonical_sequence,
                     epoch, sequence)) {
        fanout->canonical_initialized = 1;
        fanout->canonical_epoch = epoch;
        fanout->canonical_sequence = sequence;
    }

    for (size_t index = 0U; index < fanout->target_count; ++index) {
        if (send_snapshot_bounded(
                fanout, &fanout->targets[index], epoch, sequence,
                sha256_hex, count,
                TURBO_MEDIA_REVOCATION_FANOUT_TARGET_SYNCHRONIZED) != 0) {
            failed++;
        }
    }
    fill_report(fanout, epoch, sequence, report);
    return failed == 0U ? 0 : 1;
}

int turbo_media_revocation_fanout_publish_revoke(
    turbo_media_revocation_fanout_t *fanout,
    uint64_t epoch, uint64_t sequence,
    const char *sha256_hex,
    const char *const *covering_sha256_hex,
    size_t covering_count,
    turbo_media_revocation_fanout_report_t *report) {
    uint64_t previous_epoch;
    uint64_t previous_sequence;
    size_t failed = 0U;

    if (!fanout || !digest_hex_valid(sha256_hex) ||
        epoch == 0U || sequence == 0U ||
        !fanout->canonical_initialized ||
        epoch != fanout->canonical_epoch ||
        fanout->canonical_sequence == UINT64_MAX ||
        sequence != fanout->canonical_sequence + 1U ||
        !snapshot_payload_valid(
            covering_sha256_hex, covering_count) ||
        !snapshot_contains(
            covering_sha256_hex, covering_count, sha256_hex)) {
        return -1;
    }

    previous_epoch = fanout->canonical_epoch;
    previous_sequence = fanout->canonical_sequence;
    fanout->canonical_sequence = sequence;

    for (size_t index = 0U; index < fanout->target_count; ++index) {
        fanout_target_state_t *target = &fanout->targets[index];
        int event_ready =
            (target->state ==
                 TURBO_MEDIA_REVOCATION_FANOUT_TARGET_SYNCHRONIZED ||
             target->state ==
                 TURBO_MEDIA_REVOCATION_FANOUT_TARGET_RECOVERED) &&
            target->epoch == previous_epoch &&
            target->sequence == previous_sequence;

        if (event_ready &&
            send_revoke_bounded(
                fanout, target, epoch, sequence, sha256_hex) == 0) {
            continue;
        }
        if (send_snapshot_bounded(
                fanout, target, epoch, sequence,
                covering_sha256_hex, covering_count,
                TURBO_MEDIA_REVOCATION_FANOUT_TARGET_RECOVERED) != 0) {
            failed++;
        }
    }
    fill_report(fanout, epoch, sequence, report);
    return failed == 0U ? 0 : 1;
}

size_t turbo_media_revocation_fanout_target_count(
    const turbo_media_revocation_fanout_t *fanout) {
    return fanout ? fanout->target_count : 0U;
}

int turbo_media_revocation_fanout_get_target_status(
    const turbo_media_revocation_fanout_t *fanout,
    size_t index,
    turbo_media_revocation_fanout_target_status_t *status) {
    const fanout_target_state_t *target;

    if (!fanout || !status || index >= fanout->target_count) {
        return -1;
    }
    target = &fanout->targets[index];
    memset(status, 0, sizeof(*status));
    memcpy(status->target_id, target->target_id,
           strlen(target->target_id) + 1U);
    status->state = target->state;
    status->last_transport_result = target->last_transport_result;
    status->epoch = target->epoch;
    status->sequence = target->sequence;
    status->attempts = target->attempts;
    return 0;
}

int turbo_media_revocation_fanout_get_canonical_version(
    const turbo_media_revocation_fanout_t *fanout,
    int *out_initialized,
    uint64_t *out_epoch,
    uint64_t *out_sequence) {
    if (!fanout || !out_initialized || !out_epoch || !out_sequence) {
        return -1;
    }
    *out_initialized = fanout->canonical_initialized;
    *out_epoch = fanout->canonical_epoch;
    *out_sequence = fanout->canonical_sequence;
    return 0;
}
