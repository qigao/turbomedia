#include "ivr_fmq_security.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_flow_security.h"

#include <string.h>

static const char kSecret[] =
    "0123456789abcdef0123456789abcdef";
static const char kFingerprint[] =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char kFingerprintB[] =
    "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

static uint64_t test_clock(void *context) {
    return *(const uint64_t *)context;
}

spec("IVR FlowMQ security owner") {
  it("creates a default-deny server binding scoped to configured workers") {
    uint64_t now_ms = 1000u;
    ivr_certificate_identity_entry_t identity = {
        "worker-a", kFingerprint, NULL, 0u, 1u};
    ivr_fmq_server_security_config_t config =
        IVR_FMQ_SERVER_SECURITY_CONFIG_INIT;
    ivr_fmq_security_owner_t *owner = NULL;
    const turbo_flow_fmq_security_binding_t *binding;
    turbo_flow_security_auth_request_t auth =
        TURBO_FLOW_SECURITY_AUTH_REQUEST_INIT;
    turbo_flow_security_principal_t principal =
        TURBO_FLOW_SECURITY_PRINCIPAL_INIT;
    turbo_flow_security_request_t request =
        TURBO_FLOW_SECURITY_REQUEST_INIT;
    turbo_flow_security_decision_t decision =
        TURBO_FLOW_SECURITY_DECISION_INIT;

    config.shared_secret = kSecret;
    config.pub_topic = "room.events";
    config.identities = &identity;
    config.identity_count = 1u;
    config.clock = test_clock;
    config.clock_context = &now_ms;
    check_int_eq(ivr_fmq_server_security_create(&config, &owner), TURBO_OK);
    binding = ivr_fmq_security_binding(owner);
    check_not_null(binding);
    check_not_null(binding->realm);
    check_int_eq(binding->verify_peer_certificate_identity(
                     binding->peer_certificate_identity_ctx, kFingerprint,
                     "worker-a"),
                 TURBO_OK);
    check_int_eq(binding->verify_peer_certificate_identity(
                     binding->peer_certificate_identity_ctx, kFingerprint,
                     "worker-b"),
                 TURBO_EPERM);

    auth.identity = "worker-a";
    auth.method = "token";
    auth.secret = (const uint8_t *)kSecret;
    auth.secret_size = sizeof(kSecret) - 1u;
    check_int_eq(turbo_flow_security_authenticate(binding->auth_provider,
                                                  &auth, &principal),
                 TURBO_OK);
    request.principal = &principal;
    request.domain_id = principal.domain_id;
    request.action = TURBO_FLOW_SECURITY_ACTION_WRITE;
    request.resource_type = TURBO_FLOW_SECURITY_RESOURCE_GENERIC;
    request.resource = "fmq:fmq.app.endpoint:connection";
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_OK);
    request.resource = "other.topic";
    decision = (turbo_flow_security_decision_t)
        TURBO_FLOW_SECURITY_DECISION_INIT;
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_EPERM);
    ivr_fmq_security_destroy(owner);
  }

  it("rejects weak secrets and wrong credentials") {
    ivr_certificate_identity_entry_t identity = {
        "worker-a", kFingerprint, NULL, 0u, 1u};
    ivr_fmq_server_security_config_t server =
        IVR_FMQ_SERVER_SECURITY_CONFIG_INIT;
    ivr_fmq_security_owner_t *owner = NULL;
    turbo_flow_security_auth_request_t auth =
        TURBO_FLOW_SECURITY_AUTH_REQUEST_INIT;
    turbo_flow_security_principal_t principal =
        TURBO_FLOW_SECURITY_PRINCIPAL_INIT;
    const turbo_flow_fmq_security_binding_t *binding;
    static const char wrong[] =
        "ffffffffffffffffffffffffffffffff";

    server.shared_secret = "short";
    server.pub_topic = "room.events";
    server.identities = &identity;
    server.identity_count = 1u;
    check_int_eq(ivr_fmq_server_security_create(&server, &owner),
                 TURBO_EINVAL);
    check_null(owner);
    server.shared_secret = kSecret;
    check_int_eq(ivr_fmq_server_security_create(&server, &owner), TURBO_OK);
    binding = ivr_fmq_security_binding(owner);
    auth.identity = "worker-a";
    auth.method = "token";
    auth.secret = (const uint8_t *)wrong;
    auth.secret_size = sizeof(wrong) - 1u;
    check_int_eq(turbo_flow_security_authenticate(binding->auth_provider,
                                                  &auth, &principal),
                 TURBO_EPERM);
    ivr_fmq_security_destroy(owner);
  }

  it("creates a client binding whose secret lease is bounded and borrowed") {
    ivr_fmq_client_security_config_t config =
        IVR_FMQ_CLIENT_SECURITY_CONFIG_INIT;
    ivr_fmq_security_owner_t *owner = NULL;
    const turbo_flow_fmq_security_binding_t *binding;
    turbo_flow_security_secret_lease_t lease =
        TURBO_FLOW_SECURITY_SECRET_LEASE_INIT;

    config.shared_secret = kSecret;
    check_int_eq(ivr_fmq_client_security_create(&config, &owner), TURBO_OK);
    binding = ivr_fmq_security_binding(owner);
    check_not_null(binding);
    check_null(binding->realm);
    check_int_eq(turbo_flow_security_secret_acquire(
                     binding->key_provider, binding->secret_reference, &lease),
                 TURBO_OK);
    check_size_eq(lease.byte_count, sizeof(kSecret) - 1u);
    check_mem_eq(lease.bytes, kSecret, sizeof(kSecret) - 1u);
    turbo_flow_security_secret_release(binding->key_provider, &lease);
    check_null(lease.bytes);
    ivr_fmq_security_destroy(owner);
  }

  it("grants per-worker PUB/SUB topic ACL and denies unknown topics") {
    uint64_t now_ms = 1000u;
    ivr_certificate_identity_entry_t identities[2] = {
        {"worker-a", kFingerprint, NULL, 0u, 1u},
        {"worker-b", kFingerprintB, NULL, 0u, 1u}};
    ivr_fmq_worker_topic_acl_t topics[2] = {
        {"worker-a", "room.events.tenant-a"},
        {"worker-b", "room.events.tenant-b,room.events.shared"}};
    ivr_fmq_server_security_config_t config =
        IVR_FMQ_SERVER_SECURITY_CONFIG_INIT;
    ivr_fmq_security_owner_t *owner = NULL;
    const turbo_flow_fmq_security_binding_t *binding;
    turbo_flow_security_auth_request_t auth =
        TURBO_FLOW_SECURITY_AUTH_REQUEST_INIT;
    turbo_flow_security_principal_t principal =
        TURBO_FLOW_SECURITY_PRINCIPAL_INIT;
    turbo_flow_security_request_t request =
        TURBO_FLOW_SECURITY_REQUEST_INIT;
    turbo_flow_security_decision_t decision =
        TURBO_FLOW_SECURITY_DECISION_INIT;

    config.shared_secret = kSecret;
    config.pub_topic = "room.events";
    config.identities = identities;
    config.identity_count = 2u;
    config.worker_topics = topics;
    config.worker_topic_count = 2u;
    config.clock = test_clock;
    config.clock_context = &now_ms;
    check_int_eq(ivr_fmq_server_security_create(&config, &owner), TURBO_OK);
    binding = ivr_fmq_security_binding(owner);
    check_not_null(binding);

    auth.identity = "worker-a";
    auth.method = "token";
    auth.secret = (const uint8_t *)kSecret;
    auth.secret_size = sizeof(kSecret) - 1u;
    check_int_eq(turbo_flow_security_authenticate(binding->auth_provider,
                                                  &auth, &principal),
                 TURBO_OK);
    request.principal = &principal;
    request.domain_id = principal.domain_id;
    request.action = TURBO_FLOW_SECURITY_ACTION_SUBSCRIBE;
    request.resource_type = TURBO_FLOW_SECURITY_RESOURCE_GENERIC;
    request.resource = "room.events.tenant-a";
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_OK);
    /* worker-a is not granted the shared topic or tenant-b topic */
    request.resource = "room.events";
    decision = (turbo_flow_security_decision_t)
        TURBO_FLOW_SECURITY_DECISION_INIT;
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_EPERM);
    request.resource = "room.events.tenant-b";
    decision = (turbo_flow_security_decision_t)
        TURBO_FLOW_SECURITY_DECISION_INIT;
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_EPERM);

    /* worker-b is granted both of its configured topics */
    auth.identity = "worker-b";
    check_int_eq(turbo_flow_security_authenticate(binding->auth_provider,
                                                  &auth, &principal),
                 TURBO_OK);
    request.principal = &principal;
    request.domain_id = principal.domain_id;
    request.resource = "room.events.tenant-b";
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_OK);
    request.resource = "room.events.shared";
    decision = (turbo_flow_security_decision_t)
        TURBO_FLOW_SECURITY_DECISION_INIT;
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_OK);
    request.resource = "room.events.tenant-a";
    decision = (turbo_flow_security_decision_t)
        TURBO_FLOW_SECURITY_DECISION_INIT;
    check_int_eq(turbo_flow_security_realm_authorize(
                     binding->realm, &request, 1u, &decision),
                 TURBO_EPERM);
    ivr_fmq_security_destroy(owner);
  }

  it("rejects a worker topic ACL referencing an unknown worker") {
    uint64_t now_ms = 1000u;
    ivr_certificate_identity_entry_t identity = {
        "worker-a", kFingerprint, NULL, 0u, 1u};
    ivr_fmq_worker_topic_acl_t topics[1] = {
        {"worker-unknown", "room.events"}};
    ivr_fmq_server_security_config_t config =
        IVR_FMQ_SERVER_SECURITY_CONFIG_INIT;
    ivr_fmq_security_owner_t *owner = NULL;

    config.shared_secret = kSecret;
    config.pub_topic = "room.events";
    config.identities = &identity;
    config.identity_count = 1u;
    config.worker_topics = topics;
    config.worker_topic_count = 1u;
    config.clock = test_clock;
    config.clock_context = &now_ms;
    check_int_eq(ivr_fmq_server_security_create(&config, &owner),
                 TURBO_EINVAL);
    check_null(owner);
  }
}
