#include "turbo_media_tenant_quota_publisher.h"

#include <stdlib.h>
#include <string.h>

typedef struct tenant_quota_publish_target_state_s {
    char target_id[TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES];
    void *target_context;
    turbo_media_tenant_quota_publisher_target_state_t state;
    turbo_media_tenant_quota_publish_transport_result_t
        last_transport_result;
    uint64_t epoch;
    uint64_t sequence;
    size_t lease_count;
    unsigned int attempts;
} tenant_quota_publish_target_state_t;

struct turbo_media_tenant_quota_publisher_s {
    turbo_media_tenant_quota_publisher_config_t config;
    tenant_quota_publish_target_state_t *targets;
    size_t target_count;
};

static int publisher_response_exact(
    const turbo_media_tenant_quota_publish_response_t *response,
    uint64_t epoch, uint64_t sequence, size_t lease_count) {
    return response && response->synchronized &&
           response->epoch == epoch &&
           response->sequence == sequence &&
           response->lease_count == lease_count;
}

static void publisher_target_failed(
    tenant_quota_publish_target_state_t *target,
    turbo_media_tenant_quota_publish_transport_result_t result,
    unsigned int attempts) {
    if (!target) {
        return;
    }
    target->state = TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_FAILED;
    target->last_transport_result = result;
    target->attempts = attempts;
}

static void publisher_target_synchronized(
    tenant_quota_publish_target_state_t *target,
    turbo_media_tenant_quota_publish_transport_result_t result,
    unsigned int attempts,
    const turbo_media_tenant_quota_publish_response_t *response) {
    if (!target || !response) {
        return;
    }
    target->state =
        TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_SYNCHRONIZED;
    target->last_transport_result = result;
    target->attempts = attempts;
    target->epoch = response->epoch;
    target->sequence = response->sequence;
    target->lease_count = response->lease_count;
}

static int publisher_send_snapshot(
    turbo_media_tenant_quota_publisher_t *publisher,
    tenant_quota_publish_target_state_t *target,
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t now_unix_ms,
    turbo_media_tenant_quota_lease_t *leases,
    size_t lease_capacity,
    uint64_t *published_epoch,
    uint64_t *published_sequence) {
    turbo_media_tenant_quota_publish_transport_result_t result =
        TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_FATAL;
    turbo_media_tenant_quota_publish_response_t response;
    size_t lease_count = 0U;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    unsigned int attempt;

    if (!publisher || !target || !allocator || !leases ||
        !published_epoch || !published_sequence ||
        turbo_media_tenant_quota_allocator_build_node_snapshot(
            allocator, target->target_id, now_unix_ms,
            leases, lease_capacity, &lease_count,
            &epoch, &sequence) != 0) {
        return -1;
    }

    for (attempt = 1U;
         attempt <= publisher->config.max_attempts; ++attempt) {
        memset(&response, 0, sizeof(response));
        result = publisher->config.send_snapshot(
            publisher->config.transport_context,
            target->target_context,
            target->target_id,
            epoch, sequence,
            leases, lease_count,
            attempt, &response);
        if ((result ==
                 TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_APPLIED ||
             result ==
                 TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_STALE) &&
            publisher_response_exact(
                &response, epoch, sequence, lease_count)) {
            publisher_target_synchronized(
                target, result, attempt, &response);
            *published_epoch = epoch;
            *published_sequence = sequence;
            return 0;
        }
        if (result !=
            TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_RETRYABLE) {
            break;
        }
    }

    publisher_target_failed(
        target, result,
        attempt > publisher->config.max_attempts
            ? publisher->config.max_attempts
            : attempt);
    *published_epoch = epoch;
    *published_sequence = sequence;
    return 1;
}

static void publisher_fill_report(
    const turbo_media_tenant_quota_publisher_t *publisher,
    uint64_t epoch, uint64_t sequence,
    turbo_media_tenant_quota_publisher_report_t *report) {
    size_t index;

    if (!report) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->epoch = epoch;
    report->sequence = sequence;
    report->target_count = publisher ? publisher->target_count : 0U;
    if (!publisher) {
        return;
    }
    for (index = 0U; index < publisher->target_count; ++index) {
        if (publisher->targets[index].state ==
            TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_SYNCHRONIZED) {
            report->synchronized_count++;
        } else if (publisher->targets[index].state ==
                   TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_FAILED) {
            report->failed_count++;
        }
    }
}

turbo_media_tenant_quota_publisher_t *
turbo_media_tenant_quota_publisher_create(
    const turbo_media_tenant_quota_publisher_config_t *config,
    const turbo_media_tenant_quota_publisher_target_t *targets,
    size_t target_count) {
    turbo_media_tenant_quota_publisher_t *publisher;
    size_t index;
    size_t prior;

    if (!config || !config->send_snapshot ||
        config->max_attempts == 0U ||
        config->max_attempts >
            TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_MAX_ATTEMPTS ||
        !targets || target_count == 0U ||
        target_count >
            TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_MAX_TARGETS) {
        return NULL;
    }

    publisher = (turbo_media_tenant_quota_publisher_t *)calloc(
        1U, sizeof(*publisher));
    if (!publisher) {
        return NULL;
    }
    publisher->targets =
        (tenant_quota_publish_target_state_t *)calloc(
            target_count, sizeof(*publisher->targets));
    if (!publisher->targets) {
        free(publisher);
        return NULL;
    }
    publisher->config = *config;
    publisher->target_count = target_count;

    for (index = 0U; index < target_count; ++index) {
        if (!turbo_media_tenant_quota_node_id_valid(
                targets[index].target_id)) {
            turbo_media_tenant_quota_publisher_destroy(publisher);
            return NULL;
        }
        for (prior = 0U; prior < index; ++prior) {
            if (strcmp(targets[index].target_id,
                       publisher->targets[prior].target_id) == 0) {
                turbo_media_tenant_quota_publisher_destroy(publisher);
                return NULL;
            }
        }
        memcpy(publisher->targets[index].target_id,
               targets[index].target_id,
               strlen(targets[index].target_id) + 1U);
        publisher->targets[index].target_context =
            targets[index].target_context;
        publisher->targets[index].state =
            TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_UNKNOWN;
        publisher->targets[index].last_transport_result =
            TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_FATAL;
    }
    return publisher;
}

void turbo_media_tenant_quota_publisher_destroy(
    turbo_media_tenant_quota_publisher_t *publisher) {
    if (!publisher) {
        return;
    }
    if (publisher->targets) {
        memset(publisher->targets, 0,
               publisher->target_count * sizeof(*publisher->targets));
    }
    free(publisher->targets);
    memset(publisher, 0, sizeof(*publisher));
    free(publisher);
}

int turbo_media_tenant_quota_publisher_publish(
    turbo_media_tenant_quota_publisher_t *publisher,
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t now_unix_ms,
    turbo_media_tenant_quota_publisher_report_t *report) {
    turbo_media_tenant_quota_lease_t *leases = NULL;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    uint64_t target_epoch = 0U;
    uint64_t target_sequence = 0U;
    size_t policy_count = 0U;
    size_t allocator_lease_count = 0U;
    size_t failed = 0U;
    int synchronized = 0;
    size_t index;

    if (!publisher || !allocator ||
        turbo_media_tenant_quota_allocator_status(
            allocator, &synchronized, &epoch, &sequence,
            &policy_count, &allocator_lease_count) != 0 ||
        !synchronized) {
        return -1;
    }
    (void)policy_count;
    (void)allocator_lease_count;

    leases = (turbo_media_tenant_quota_lease_t *)calloc(
        TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS, sizeof(*leases));
    if (!leases) {
        return -1;
    }

    for (index = 0U; index < publisher->target_count; ++index) {
        int result = publisher_send_snapshot(
            publisher, &publisher->targets[index],
            allocator, now_unix_ms, leases,
            TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS,
            &target_epoch, &target_sequence);
        memset(
            leases, 0,
            TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS * sizeof(*leases));
        if (result != 0) {
            failed++;
            continue;
        }
        if (target_epoch != epoch || target_sequence != sequence) {
            publisher_target_failed(
                &publisher->targets[index],
                TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_ERROR,
                publisher->targets[index].attempts);
            failed++;
        }
    }

    memset(
        leases, 0,
        TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS * sizeof(*leases));
    free(leases);
    publisher_fill_report(publisher, epoch, sequence, report);
    return failed == 0U ? 0 : 1;
}

size_t turbo_media_tenant_quota_publisher_target_count(
    const turbo_media_tenant_quota_publisher_t *publisher) {
    return publisher ? publisher->target_count : 0U;
}

int turbo_media_tenant_quota_publisher_get_target_status(
    const turbo_media_tenant_quota_publisher_t *publisher,
    size_t index,
    turbo_media_tenant_quota_publisher_target_status_t *status) {
    const tenant_quota_publish_target_state_t *target;

    if (!publisher || !status || index >= publisher->target_count) {
        return -1;
    }
    target = &publisher->targets[index];
    memset(status, 0, sizeof(*status));
    memcpy(status->target_id, target->target_id,
           strlen(target->target_id) + 1U);
    status->state = target->state;
    status->last_transport_result = target->last_transport_result;
    status->epoch = target->epoch;
    status->sequence = target->sequence;
    status->lease_count = target->lease_count;
    status->attempts = target->attempts;
    return 0;
}
