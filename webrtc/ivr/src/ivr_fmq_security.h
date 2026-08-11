#ifndef TURBO_MEDIA_IVR_FMQ_SECURITY_H
#define TURBO_MEDIA_IVR_FMQ_SECURITY_H

#include "ivr_certificate_identity.h"
#include "turbo_flow_fmq.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_FMQ_SECURITY_MIN_SECRET_BYTES 32u
#define IVR_FMQ_SECURITY_MAX_SECRET_BYTES 4096u

typedef struct ivr_fmq_security_owner_s ivr_fmq_security_owner_t;

/* Optional per-worker PUB/SUB topic allowlist. When absent the worker is
   granted SUB/READ on the shared pub_topic only; when present the worker is
   granted SUB/READ only on the listed topics (comma-separated). An empty or
   unknown topic subscription is denied by the default-deny realm. */
typedef struct {
    const char *worker_id;
    const char *pub_topics;
} ivr_fmq_worker_topic_acl_t;

typedef struct {
    size_t size;
    const char *shared_secret;
    const char *pub_topic;
    const ivr_certificate_identity_entry_t *identities;
    size_t identity_count;
    /* Optional finer-grained per-worker SUB/READ topic ACL. */
    const ivr_fmq_worker_topic_acl_t *worker_topics;
    size_t worker_topic_count;
    uint64_t policy_version;
    ivr_certificate_identity_clock_fn clock;
    void *clock_context;
} ivr_fmq_server_security_config_t;

#define IVR_FMQ_SERVER_SECURITY_CONFIG_INIT                                \
    {sizeof(ivr_fmq_server_security_config_t), NULL, NULL, NULL, 0u,       \
     NULL, 0u, 1u, NULL, NULL}

typedef struct {
    size_t size;
    const char *shared_secret;
} ivr_fmq_client_security_config_t;

#define IVR_FMQ_CLIENT_SECURITY_CONFIG_INIT \
    {sizeof(ivr_fmq_client_security_config_t), NULL}

int ivr_fmq_server_security_create(
    const ivr_fmq_server_security_config_t *config,
    ivr_fmq_security_owner_t **out_owner);
int ivr_fmq_client_security_create(
    const ivr_fmq_client_security_config_t *config,
    ivr_fmq_security_owner_t **out_owner);

/* Borrowed binding. The owner must outlive every FlowMQ facade using it. */
const turbo_flow_fmq_security_binding_t *ivr_fmq_security_binding(
    const ivr_fmq_security_owner_t *owner);

void ivr_fmq_security_destroy(ivr_fmq_security_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_FMQ_SECURITY_H */
