#include "ivr_fmq_security.h"

#include "turbo_error.h"
#include "turbo_flow_security.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_FMQ_SECURITY_DOMAIN "turbomedia.ivr"
#define IVR_FMQ_SECURITY_ROLE "ivr-worker"
#define IVR_FMQ_SECURITY_AUTH_METHOD "token"
#define IVR_FMQ_SECURITY_REALM_CHANNEL "security.ivr"
#define IVR_FMQ_SECURITY_SECRET_REFERENCE "secret/ivr-flowmq"
#define IVR_FMQ_SECURITY_CONNECTION_RESOURCE "fmq:fmq.app.endpoint:connection"

struct ivr_fmq_security_owner_s {
    uint8_t *secret;
    size_t secret_size;
    uint64_t policy_version;
    ivr_certificate_identity_t *certificate_identity;
    turbo_flow_security_realm_t *realm;
    turbo_flow_security_auth_provider_t auth_provider;
    turbo_flow_security_key_provider_t key_provider;
    turbo_flow_fmq_security_binding_t binding;
};

static void ivr_fmq_secret_clear(void *data, size_t size) {
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    while (bytes && size > 0u) {
        *bytes++ = 0u;
        --size;
    }
}

static int ivr_fmq_secret_valid(const char *secret, size_t *out_size) {
    size_t size;
    if (!secret || !out_size) return 0;
    size = strlen(secret);
    if (size < IVR_FMQ_SECURITY_MIN_SECRET_BYTES ||
        size > IVR_FMQ_SECURITY_MAX_SECRET_BYTES) {
        return 0;
    }
    *out_size = size;
    return 1;
}

static int ivr_fmq_secret_equals(const uint8_t *left, size_t left_size,
                                 const uint8_t *right, size_t right_size) {
    volatile uint8_t difference = 0u;
    size_t i;
    if (!left || !right || left_size != right_size) return 0;
    for (i = 0u; i < left_size; ++i) difference |= left[i] ^ right[i];
    return difference == 0u;
}

static int ivr_fmq_authenticate(
    void *context, const turbo_flow_security_auth_request_t *request,
    turbo_flow_security_principal_t *principal_out) {
    ivr_fmq_security_owner_t *owner = (ivr_fmq_security_owner_t *)context;
    turbo_flow_security_principal_t principal =
        TURBO_FLOW_SECURITY_PRINCIPAL_INIT;
    if (!owner || !request || !principal_out || !request->identity ||
        request->identity[0] == '\0' ||
        strlen(request->identity) > IVR_CERTIFICATE_IDENTITY_MAX_WORKER_ID ||
        !request->method ||
        strcmp(request->method, IVR_FMQ_SECURITY_AUTH_METHOD) != 0 ||
        !ivr_fmq_secret_equals(request->secret, request->secret_size,
                               owner->secret, owner->secret_size)) {
        return TURBO_EPERM;
    }
    snprintf(principal.principal_id, sizeof(principal.principal_id), "%s",
             request->identity);
    snprintf(principal.principal_type, sizeof(principal.principal_type), "%s",
             "service");
    snprintf(principal.domain_id, sizeof(principal.domain_id), "%s",
             IVR_FMQ_SECURITY_DOMAIN);
    snprintf(principal.auth_method, sizeof(principal.auth_method), "%s",
             IVR_FMQ_SECURITY_AUTH_METHOD);
    principal.scope = TURBO_FLOW_SECURITY_SCOPE_SELF;
    principal.role_count = 1u;
    snprintf(principal.roles[0], sizeof(principal.roles[0]), "%s",
             IVR_FMQ_SECURITY_ROLE);
    principal.policy_version = owner->policy_version;
    *principal_out = principal;
    return TURBO_OK;
}

static int ivr_fmq_secret_acquire(
    void *context, const char *reference,
    turbo_flow_security_secret_lease_t *lease_out) {
    ivr_fmq_security_owner_t *owner = (ivr_fmq_security_owner_t *)context;
    if (!owner || !reference ||
        strcmp(reference, IVR_FMQ_SECURITY_SECRET_REFERENCE) != 0 ||
        !lease_out || lease_out->size < sizeof(*lease_out)) {
        return TURBO_EINVAL;
    }
    *lease_out =
        (turbo_flow_security_secret_lease_t)TURBO_FLOW_SECURITY_SECRET_LEASE_INIT;
    lease_out->bytes = owner->secret;
    lease_out->byte_count = owner->secret_size;
    lease_out->version = 1u;
    lease_out->provider_lease = owner;
    return TURBO_OK;
}

static void ivr_fmq_secret_release(
    void *context, turbo_flow_security_secret_lease_t *lease) {
    (void)context;
    if (lease) {
        *lease = (turbo_flow_security_secret_lease_t)
            TURBO_FLOW_SECURITY_SECRET_LEASE_INIT;
    }
}

static int ivr_fmq_security_owner_allocate(
    const char *shared_secret, ivr_fmq_security_owner_t **out_owner) {
    ivr_fmq_security_owner_t *owner;
    size_t secret_size;
    if (out_owner) *out_owner = NULL;
    if (!out_owner || !ivr_fmq_secret_valid(shared_secret, &secret_size)) {
        return TURBO_EINVAL;
    }
    owner = (ivr_fmq_security_owner_t *)calloc(1u, sizeof(*owner));
    if (!owner) return TURBO_ENOMEM;
    owner->secret = (uint8_t *)malloc(secret_size);
    if (!owner->secret) {
        free(owner);
        return TURBO_ENOMEM;
    }
    memcpy(owner->secret, shared_secret, secret_size);
    owner->secret_size = secret_size;
    *out_owner = owner;
    return TURBO_OK;
}

static void ivr_fmq_rule_init(turbo_flow_security_rule_t *rule,
                              const char *worker_id, uint32_t actions,
                              const char *resource) {
    *rule = (turbo_flow_security_rule_t)TURBO_FLOW_SECURITY_RULE_INIT;
    rule->effect = TURBO_FLOW_SECURITY_ALLOW;
    rule->subject_kind = TURBO_FLOW_SECURITY_SUBJECT_PRINCIPAL;
    snprintf(rule->subject, sizeof(rule->subject), "%s", worker_id);
    snprintf(rule->domain_id, sizeof(rule->domain_id), "%s",
             IVR_FMQ_SECURITY_DOMAIN);
    rule->action_mask = actions;
    rule->resource_type = TURBO_FLOW_SECURITY_RESOURCE_GENERIC;
    rule->match_kind = TURBO_FLOW_SECURITY_MATCH_EXACT;
    snprintf(rule->pattern, sizeof(rule->pattern), "%s", resource);
}

static size_t ivr_fmq_topic_count(const char *csv) {
    size_t count = 0u;
    const char *p = csv;
    if (!csv || csv[0] == '\0') {
        return 0u;
    }
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > 0u && len <= TURBO_FLOW_SECURITY_PATTERN_MAX) {
            count++;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return count;
}

/* 1 when every comma-separated token is a non-empty topic within the security
   pattern limit. */
static int ivr_fmq_topics_valid(const char *csv) {
    const char *p = csv;
    if (!csv || csv[0] == '\0') {
        return 0;
    }
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 0u || len > TURBO_FLOW_SECURITY_PATTERN_MAX) {
            return 0;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return 1;
}

static const ivr_fmq_worker_topic_acl_t *ivr_fmq_worker_topic_find(
    const ivr_fmq_server_security_config_t *config, const char *worker_id) {
    size_t i;
    for (i = 0u; i < config->worker_topic_count; ++i) {
        if (config->worker_topics[i].worker_id &&
            strcmp(config->worker_topics[i].worker_id, worker_id) == 0) {
            return &config->worker_topics[i];
        }
    }
    return NULL;
}

/* Append SUBSCRIBE + READ rules for every topic in a comma-separated list. */
static void ivr_fmq_rule_for_topics(turbo_flow_security_rule_t *rules,
                                    size_t *cursor, const char *worker_id,
                                    const char *topics_csv) {
    const char *p = topics_csv;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char topic[TURBO_FLOW_SECURITY_PATTERN_MAX + 1u];
        if (len == 0u || len >= sizeof(topic)) {
            if (!end) {
                break;
            }
            p = end + 1;
            continue;
        }
        memcpy(topic, p, len);
        topic[len] = '\0';
        ivr_fmq_rule_init(&rules[(*cursor)++], worker_id,
                          TURBO_FLOW_SECURITY_ACTION_SUBSCRIBE, topic);
        ivr_fmq_rule_init(&rules[(*cursor)++], worker_id,
                          TURBO_FLOW_SECURITY_ACTION_READ, topic);
        if (!end) {
            break;
        }
        p = end + 1;
    }
}

int ivr_fmq_server_security_create(
    const ivr_fmq_server_security_config_t *config,
    ivr_fmq_security_owner_t **out_owner) {
    ivr_fmq_security_owner_t *owner = NULL;
    ivr_certificate_identity_config_t identity_config =
        IVR_CERTIFICATE_IDENTITY_CONFIG_INIT;
    turbo_flow_security_realm_config_t realm_config =
        TURBO_FLOW_SECURITY_REALM_CONFIG_INIT;
    turbo_flow_security_rule_t *rules = NULL;
    size_t rule_count = 0u;
    size_t cursor = 0u;
    size_t i;
    int rc;
    if (out_owner) *out_owner = NULL;
    if (!config || config->size < sizeof(*config) || !out_owner ||
        !config->pub_topic || config->pub_topic[0] == '\0' ||
        strlen(config->pub_topic) > TURBO_FLOW_SECURITY_PATTERN_MAX ||
        !config->identities || config->identity_count == 0u ||
        config->identity_count > IVR_CERTIFICATE_IDENTITY_MAX_ENTRIES ||
        config->policy_version == 0u ||
        (config->worker_topic_count > 0u && !config->worker_topics)) {
        return TURBO_EINVAL;
    }
    /* Validate the per-worker topic ACL before allocating anything: every
       listed worker must exist in the identity table and every topic must be
       bounded. An unknown worker reference fails closed. */
    for (i = 0u; i < config->worker_topic_count; ++i) {
        const ivr_fmq_worker_topic_acl_t *wt = &config->worker_topics[i];
        size_t j;
        int identity_found = 0;
        if (!wt->worker_id || wt->worker_id[0] == '\0' ||
            !ivr_fmq_topics_valid(wt->pub_topics)) {
            return TURBO_EINVAL;
        }
        for (j = 0u; j < config->identity_count; ++j) {
            if (strcmp(config->identities[j].worker_id, wt->worker_id) == 0) {
                identity_found = 1;
                break;
            }
        }
        if (!identity_found) {
            return TURBO_EINVAL;
        }
    }
    /* One CONNECT rule plus SUBSCRIBE/READ per allowed topic per worker. */
    for (i = 0u; i < config->identity_count; ++i) {
        const char *worker_id = config->identities[i].worker_id;
        const ivr_fmq_worker_topic_acl_t *wt =
            ivr_fmq_worker_topic_find(config, worker_id);
        size_t topic_count = wt ? ivr_fmq_topic_count(wt->pub_topics) : 1u;
        if (rule_count > SIZE_MAX - 1u - 2u * topic_count) {
            return TURBO_ERANGE;
        }
        rule_count += 1u + 2u * topic_count;
    }
    rc = ivr_fmq_security_owner_allocate(config->shared_secret, &owner);
    if (rc != TURBO_OK) return rc;
    owner->policy_version = config->policy_version;
    identity_config.entries = config->identities;
    identity_config.entry_count = config->identity_count;
    identity_config.clock = config->clock;
    identity_config.clock_context = config->clock_context;
    rc = ivr_certificate_identity_create(&identity_config,
                                         &owner->certificate_identity);
    if (rc != TURBO_OK) goto fail;

    rules = (turbo_flow_security_rule_t *)calloc(rule_count, sizeof(*rules));
    if (!rules) {
        rc = TURBO_ENOMEM;
        goto fail;
    }
    for (i = 0u; i < config->identity_count; ++i) {
        const char *worker_id = config->identities[i].worker_id;
        const ivr_fmq_worker_topic_acl_t *wt =
            ivr_fmq_worker_topic_find(config, worker_id);
        const char *topics = wt ? wt->pub_topics : config->pub_topic;
        ivr_fmq_rule_init(&rules[cursor++], worker_id,
                          TURBO_FLOW_SECURITY_ACTION_CONNECT |
                              TURBO_FLOW_SECURITY_ACTION_READ |
                              TURBO_FLOW_SECURITY_ACTION_WRITE,
                          IVR_FMQ_SECURITY_CONNECTION_RESOURCE);
        ivr_fmq_rule_for_topics(rules, &cursor, worker_id, topics);
    }
    realm_config.resource_uid = "security:turbomedia.ivr";
    realm_config.owner_name = "turbomedia.ivr";
    realm_config.policy_version = config->policy_version;
    realm_config.rules = rules;
    realm_config.rule_count = rule_count;
    rc = turbo_flow_security_realm_create(&realm_config, &owner->realm);
    free(rules);
    rules = NULL;
    if (rc != TURBO_OK) goto fail;

    owner->auth_provider = (turbo_flow_security_auth_provider_t)
        TURBO_FLOW_SECURITY_AUTH_PROVIDER_INIT;
    owner->auth_provider.ctx = owner;
    owner->auth_provider.authenticate = ivr_fmq_authenticate;
    owner->binding = (turbo_flow_fmq_security_binding_t)
        TURBO_FLOW_FMQ_SECURITY_BINDING_INIT;
    owner->binding.realm_channel = IVR_FMQ_SECURITY_REALM_CHANNEL;
    owner->binding.auth_method = IVR_FMQ_SECURITY_AUTH_METHOD;
    owner->binding.auth_provider = &owner->auth_provider;
    owner->binding.realm = owner->realm;
    owner->binding.verify_peer_certificate_identity =
        ivr_certificate_identity_verify;
    owner->binding.peer_certificate_identity_ctx =
        owner->certificate_identity;
    *out_owner = owner;
    return TURBO_OK;

fail:
    free(rules);
    ivr_fmq_security_destroy(owner);
    return rc;
}
int ivr_fmq_client_security_create(
    const ivr_fmq_client_security_config_t *config,
    ivr_fmq_security_owner_t **out_owner) {
    ivr_fmq_security_owner_t *owner = NULL;
    int rc;
    if (out_owner) *out_owner = NULL;
    if (!config || config->size < sizeof(*config) || !out_owner) {
        return TURBO_EINVAL;
    }
    rc = ivr_fmq_security_owner_allocate(config->shared_secret, &owner);
    if (rc != TURBO_OK) return rc;
    owner->key_provider = (turbo_flow_security_key_provider_t)
        TURBO_FLOW_SECURITY_KEY_PROVIDER_INIT;
    owner->key_provider.ctx = owner;
    owner->key_provider.acquire = ivr_fmq_secret_acquire;
    owner->key_provider.release = ivr_fmq_secret_release;
    owner->binding = (turbo_flow_fmq_security_binding_t)
        TURBO_FLOW_FMQ_SECURITY_BINDING_INIT;
    owner->binding.auth_method = IVR_FMQ_SECURITY_AUTH_METHOD;
    owner->binding.key_provider = &owner->key_provider;
    owner->binding.secret_reference = IVR_FMQ_SECURITY_SECRET_REFERENCE;
    *out_owner = owner;
    return TURBO_OK;
}

const turbo_flow_fmq_security_binding_t *ivr_fmq_security_binding(
    const ivr_fmq_security_owner_t *owner) {
    return owner ? &owner->binding : NULL;
}

void ivr_fmq_security_destroy(ivr_fmq_security_owner_t *owner) {
    if (!owner) return;
    turbo_flow_security_realm_destroy(owner->realm);
    owner->realm = NULL;
    ivr_certificate_identity_destroy(owner->certificate_identity);
    owner->certificate_identity = NULL;
    ivr_fmq_secret_clear(owner->secret, owner->secret_size);
    free(owner->secret);
    memset(owner, 0, sizeof(*owner));
    free(owner);
}
