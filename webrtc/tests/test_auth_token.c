#include "tinytest.h"
#include "turbo_media_auth.h"
#include "turbo_media_revocation.h"
#include <turbo_crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NOW_SECONDS 2000000000LL

static const turbo_media_auth_config_t active_config = {
    .issuer = "turbomedia-test",
    .active_key_id = "active-2026-07",
    .active_secret = "0123456789abcdef0123456789abcdef",
    .previous_key_id = "previous-2026-06",
    .previous_secret = "abcdef0123456789abcdef0123456789",
    .clock_skew_seconds = 30,
    .max_ttl_seconds = 300
};

static char *authorization_for(const char *token) {
    size_t length = strlen(token) + strlen("Bearer ") + 1U;
    char *authorization = (char *)malloc(length);
    check_not_null(authorization);
    snprintf(authorization, length, "Bearer %s", token);
    return authorization;
}

static char *issue_token(const turbo_media_auth_config_t *config,
                         const char *scope, const char *room_id,
                         const char *participant_id, int64_t issued_at,
                         int64_t expires_at) {
    turbo_media_auth_claims_t claims = {
        .subject = "room-service",
        .audience = "turbomedia-sfu-media",
        .scope = scope,
        .room_id = room_id,
        .participant_id = participant_id,
        .issued_at = issued_at,
        .expires_at = expires_at
    };
    return turbo_media_auth_issue(config, &claims);
}

static turbo_media_auth_result_t authorize(
    const char *authorization, const turbo_media_auth_config_t *config,
    const char *scope, const char *room_id, const char *participant_id,
    int64_t now) {
    turbo_media_auth_policy_t policy = {
        .audience = "turbomedia-sfu-media",
        .required_scope = scope,
        .room_id = room_id,
        .participant_id = participant_id,
        .now = now
    };
    return turbo_media_auth_authorize(authorization, NULL, config, &policy);
}

static char *issue_tenant_token(
    const turbo_media_auth_config_t *config, const char *tenant_id,
    const char *scope, const char *room_id, const char *participant_id,
    int64_t issued_at, int64_t expires_at) {
    turbo_media_auth_claims_t claims = {
        .subject = "room-service",
        .audience = "turbomedia-sfu-media",
        .scope = scope,
        .tenant_id = tenant_id,
        .room_id = room_id,
        .participant_id = participant_id,
        .issued_at = issued_at,
        .expires_at = expires_at
    };
    return turbo_media_auth_issue(config, &claims);
}

static turbo_media_auth_result_t authorize_tenant(
    const char *authorization, const turbo_media_auth_config_t *config,
    const char *tenant_id, const char *scope, const char *room_id,
    const char *participant_id, int64_t now) {
    turbo_media_auth_policy_t policy = {
        .audience = "turbomedia-sfu-media",
        .required_scope = scope,
        .tenant_id = tenant_id,
        .room_id = room_id,
        .participant_id = participant_id,
        .now = now
    };
    return turbo_media_auth_authorize(
        authorization, NULL, config, &policy);
}

static void token_sha256_hex(const char *token, char output[65]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];

    check_equal((int)(turbo_crypto_sha256(token, strlen(token), digest)), (int)(TURBO_CRYPTO_OK));
    for (size_t index = 0; index < sizeof(digest); ++index) {
        output[index * 2U] = hex[digest[index] >> 4U];
        output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    output[64] = '\0';
}

void test_auth_token_accepts_exact_scope_and_resource(void) {
    check_equal((int)(turbo_media_auth_config_validate(&active_config)), (int)(0));
    char *token = issue_token(
        &active_config, "sfu.media.publish sfu.media.trickle", "room-a",
        "alice", TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    check_not_null(token);
    authorization = authorization_for(token);
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_SIGNED_TOKEN));

    free(authorization);
    free(token);
}

void test_auth_token_tenant_binding_is_exact_and_unbound_is_not_global(void) {
    char *tenant_token = issue_tenant_token(
        &active_config, "tenant-a", "sfu.media.publish",
        "tenant-a/room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *legacy_unbound = issue_token(
        &active_config, "sfu.media.publish",
        "tenant-a/room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    check_not_null(tenant_token);
    check_not_null(legacy_unbound);

    authorization = authorization_for(tenant_token);
    check_equal(
        (int)authorize_tenant(
            authorization, &active_config, "tenant-a",
            "sfu.media.publish", "tenant-a/room-a", "alice",
            TEST_NOW_SECONDS + 1),
        (int)TURBO_MEDIA_AUTH_SIGNED_TOKEN);
    check_equal(
        (int)authorize_tenant(
            authorization, &active_config, "tenant-b",
            "sfu.media.publish", "tenant-a/room-a", "alice",
            TEST_NOW_SECONDS + 1),
        (int)TURBO_MEDIA_AUTH_DENIED);
    check_equal(
        (int)authorize_tenant(
            authorization, &active_config, NULL,
            "sfu.media.publish", "tenant-a/room-a", "alice",
            TEST_NOW_SECONDS + 1),
        (int)TURBO_MEDIA_AUTH_DENIED);
    free(authorization);

    authorization = authorization_for(legacy_unbound);
    check_equal(
        (int)authorize_tenant(
            authorization, &active_config, NULL,
            "sfu.media.publish", "tenant-a/room-a", "alice",
            TEST_NOW_SECONDS + 1),
        (int)TURBO_MEDIA_AUTH_SIGNED_TOKEN);
    check_equal(
        (int)authorize_tenant(
            authorization, &active_config, "tenant-a",
            "sfu.media.publish", "tenant-a/room-a", "alice",
            TEST_NOW_SECONDS + 1),
        (int)TURBO_MEDIA_AUTH_DENIED);

    free(authorization);
    free(legacy_unbound);
    free(tenant_token);
}

void test_auth_token_rejects_invalid_or_inconsistent_tenant_identity(void) {
    char *token = issue_tenant_token(
        &active_config, "tenant a", "sfu.media.publish",
        "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    turbo_media_auth_policy_t policy = {
        .audience = "turbomedia-sfu-media",
        .required_scope = "sfu.media.publish",
        .room_id = "tenant-b/room-a",
        .participant_id = "alice",
        .now = TEST_NOW_SECONDS + 1,
        .tenant_id = "tenant-a"
    };

    check_null(token);

    token = issue_tenant_token(
        &active_config, "tenant/a", "sfu.media.publish",
        "tenant/a/room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    check_null(token);

    token = issue_tenant_token(
        &active_config, "tenant-a", "sfu.media.publish",
        "tenant-b/room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    check_null(token);

    token = issue_token(
        &active_config, "sfu.media.publish",
        "tenant-b/room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    check_not_null(token);
    check_equal(
        (int)turbo_media_auth_authorize_token(
            token, &active_config, &policy),
        (int)TURBO_MEDIA_AUTH_DENIED);
    free(token);
}

void test_auth_token_rejects_cross_room_and_participant_use(void) {
    char *token = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    check_not_null(token);
    authorization = authorization_for(token);
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-b", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_DENIED));
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "bob", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_DENIED));
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.subscribe",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_DENIED));

    free(authorization);
    free(token);
}

void test_auth_token_rejects_expired_and_overlong_tokens(void) {
    char *expired = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS - 120, TEST_NOW_SECONDS - 31);
    char *overlong = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 301);
    char *authorization;

    check_not_null(expired);
    check_null(overlong);
    authorization = authorization_for(expired);
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS)), (int)(TURBO_MEDIA_AUTH_DENIED));

    free(authorization);
    free(expired);
}

void test_auth_token_supports_one_previous_rotation_key(void) {
    turbo_media_auth_config_t old_issuer = active_config;
    char *token;
    char *authorization;

    old_issuer.active_key_id = active_config.previous_key_id;
    old_issuer.active_secret = active_config.previous_secret;
    old_issuer.previous_key_id = NULL;
    old_issuer.previous_secret = NULL;
    token = issue_token(
        &old_issuer, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    check_not_null(token);
    authorization = authorization_for(token);
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_SIGNED_TOKEN));

    free(authorization);
    free(token);
}

void test_auth_token_rejects_tampering_and_unknown_key(void) {
    turbo_media_auth_config_t unrelated = active_config;
    char *token = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    check_not_null(token);
    token[strlen(token) - 1U] =
        token[strlen(token) - 1U] == 'A' ? 'B' : 'A';
    authorization = authorization_for(token);
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_DENIED));
    free(authorization);
    free(token);

    unrelated.active_key_id = "unrelated";
    unrelated.previous_key_id = NULL;
    unrelated.previous_secret = NULL;
    token = issue_token(
        &unrelated, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    check_not_null(token);
    authorization = authorization_for(token);
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_DENIED));

    free(authorization);
    free(token);
}

void test_auth_token_keeps_static_bearer_compatibility_explicit(void) {
    turbo_media_auth_policy_t policy = {
        .audience = "turbomedia-sfu-media",
        .required_scope = "sfu.media.publish",
        .room_id = "room-a",
        .participant_id = "alice",
        .now = TEST_NOW_SECONDS
    };

    check_equal((int)(turbo_media_auth_authorize(
            "Bearer legacy-token", "legacy-token", &active_config, &policy)), (int)(TURBO_MEDIA_AUTH_STATIC_TOKEN));
    check_equal((int)(turbo_media_auth_authorize(
            "Bearer wrong-token", "legacy-token", &active_config, &policy)), (int)(TURBO_MEDIA_AUTH_DENIED));
}

void test_auth_token_rejects_weak_or_incomplete_key_configuration(void) {
    turbo_media_auth_config_t weak = active_config;
    turbo_media_auth_config_t incomplete_rotation = active_config;

    weak.active_secret = "human-password";
    incomplete_rotation.previous_secret = NULL;
    check_equal((int)(turbo_media_auth_config_validate(&weak)), (int)(-1));
    check_equal((int)(turbo_media_auth_config_validate(&incomplete_rotation)), (int)(-1));
}

void test_auth_token_rejects_exact_revoked_fingerprint(void) {
    turbo_media_auth_config_t revoked = active_config;
    char *token = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;
    char digest[65];
    char revocation_list[130];

    check_not_null(token);
    token_sha256_hex(token, digest);
    snprintf(
        revocation_list, sizeof(revocation_list),
        "0000000000000000000000000000000000000000000000000000000000000000,%s",
        digest);
    revoked.revoked_token_sha256 = revocation_list;
    check_equal((int)(turbo_media_auth_config_validate(&revoked)), (int)(0));
    authorization = authorization_for(token);
    check_equal((int)(authorize(authorization, &revoked, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_DENIED));
    check_equal((int)(authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1)), (int)(TURBO_MEDIA_AUTH_SIGNED_TOKEN));

    free(authorization);
    free(token);
}

void test_auth_token_dynamic_revocation_is_fail_closed_and_recovers(void) {
    turbo_media_auth_config_t old_issuer = active_config;
    turbo_media_auth_config_t dynamic = active_config;
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(8);
    char *token;
    char *authorization;
    char digest[65];

    check_not_null(state);
    dynamic.revocation_check = turbo_media_revocation_check_digest;
    dynamic.revocation_context = state;
    check_equal((int)(turbo_media_auth_config_validate(&dynamic)), (int)(0));

    old_issuer.active_key_id = active_config.previous_key_id;
    old_issuer.active_secret = active_config.previous_secret;
    old_issuer.previous_key_id = NULL;
    old_issuer.previous_secret = NULL;
    token = issue_token(
        &old_issuer, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    check_not_null(token);
    authorization = authorization_for(token);

    check_equal((int)(authorize(
                    authorization, &dynamic, "sfu.media.publish",
                    "room-a", "alice", TEST_NOW_SECONDS + 1)),
                (int)(TURBO_MEDIA_AUTH_DENIED));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 0U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(authorize(
                    authorization, &dynamic, "sfu.media.publish",
                    "room-a", "alice", TEST_NOW_SECONDS + 1)),
                (int)(TURBO_MEDIA_AUTH_SIGNED_TOKEN));

    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 2U,
                    "0000000000000000000000000000000000000000000000000000000000000000")),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_GAP));
    check_equal((int)(authorize(
                    authorization, &dynamic, "sfu.media.publish",
                    "room-a", "alice", TEST_NOW_SECONDS + 1)),
                (int)(TURBO_MEDIA_AUTH_DENIED));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 2U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(authorize(
                    authorization, &dynamic, "sfu.media.publish",
                    "room-a", "alice", TEST_NOW_SECONDS + 1)),
                (int)(TURBO_MEDIA_AUTH_SIGNED_TOKEN));

    token_sha256_hex(token, digest);
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 3U, digest)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(authorize(
                    authorization, &dynamic, "sfu.media.publish",
                    "room-a", "alice", TEST_NOW_SECONDS + 1)),
                (int)(TURBO_MEDIA_AUTH_DENIED));

    free(authorization);
    free(token);
    turbo_media_revocation_state_destroy(state);
}

void test_auth_token_dynamic_revocation_disables_static_bearer_bypass(void) {
    turbo_media_auth_config_t dynamic = active_config;
    turbo_media_auth_policy_t policy = {
        .audience = "turbomedia-sfu-media",
        .required_scope = "sfu.media.publish",
        .room_id = "room-a",
        .participant_id = "alice",
        .now = TEST_NOW_SECONDS
    };
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(1);

    check_not_null(state);
    dynamic.revocation_check = turbo_media_revocation_check_digest;
    dynamic.revocation_context = state;

    check_equal((int)(turbo_media_auth_authorize(
                    "Bearer legacy-token", "legacy-token",
                    &dynamic, &policy)),
                (int)(TURBO_MEDIA_AUTH_DENIED));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 0U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_auth_authorize(
                    "Bearer legacy-token", "legacy-token",
                    &dynamic, &policy)),
                (int)(TURBO_MEDIA_AUTH_DENIED));

    turbo_media_revocation_state_destroy(state);
}

void test_auth_token_requires_dynamic_revocation_callback_context_pair(void) {
    turbo_media_auth_config_t callback_only = active_config;
    turbo_media_auth_config_t context_only = active_config;
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(1);

    check_not_null(state);
    callback_only.revocation_check = turbo_media_revocation_check_digest;
    callback_only.revocation_context = NULL;
    context_only.revocation_check = NULL;
    context_only.revocation_context = state;

    check_equal((int)(turbo_media_auth_config_validate(&callback_only)), (int)(-1));
    check_equal((int)(turbo_media_auth_config_validate(&context_only)), (int)(-1));

    turbo_media_revocation_state_destroy(state);
}

void test_auth_token_rejects_malformed_revocation_lists(void) {
    turbo_media_auth_config_t malformed = active_config;

    malformed.revoked_token_sha256 = "abc";
    check_equal((int)(turbo_media_auth_config_validate(&malformed)), (int)(-1));
    malformed.revoked_token_sha256 =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    check_equal((int)(turbo_media_auth_config_validate(&malformed)), (int)(-1));
    malformed.revoked_token_sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000,";
    check_equal((int)(turbo_media_auth_config_validate(&malformed)), (int)(-1));
}

void test_auth_token_bounds_revocation_list_cardinality(void) {
    turbo_media_auth_config_t bounded = active_config;
    const size_t maximum = TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS;
    const size_t valid_length = maximum * 64U + maximum - 1U;
    char *list = (char *)malloc(valid_length + 66U);
    size_t offset = 0U;

    check_not_null(list);
    for (size_t entry = 0; entry < maximum; ++entry) {
        memset(list + offset, '0', 64U);
        offset += 64U;
        if (entry + 1U < maximum) {
            list[offset++] = ',';
        }
    }
    list[offset] = '\0';
    bounded.revoked_token_sha256 = list;
    check_equal((int)(turbo_media_auth_config_validate(&bounded)), (int)(0));

    list[offset++] = ',';
    memset(list + offset, '1', 64U);
    offset += 64U;
    list[offset] = '\0';
    check_equal((int)(turbo_media_auth_config_validate(&bounded)), (int)(-1));
    free(list);
}

spec("test_auth_token") {
    it("test_auth_token_accepts_exact_scope_and_resource") { test_auth_token_accepts_exact_scope_and_resource(); };
    it("test_auth_token_tenant_binding_is_exact_and_unbound_is_not_global") { test_auth_token_tenant_binding_is_exact_and_unbound_is_not_global(); };
    it("test_auth_token_rejects_invalid_or_inconsistent_tenant_identity") { test_auth_token_rejects_invalid_or_inconsistent_tenant_identity(); };
    it("test_auth_token_rejects_cross_room_and_participant_use") { test_auth_token_rejects_cross_room_and_participant_use(); };
    it("test_auth_token_rejects_expired_and_overlong_tokens") { test_auth_token_rejects_expired_and_overlong_tokens(); };
    it("test_auth_token_supports_one_previous_rotation_key") { test_auth_token_supports_one_previous_rotation_key(); };
    it("test_auth_token_rejects_tampering_and_unknown_key") { test_auth_token_rejects_tampering_and_unknown_key(); };
    it("test_auth_token_keeps_static_bearer_compatibility_explicit") { test_auth_token_keeps_static_bearer_compatibility_explicit(); };
    it("test_auth_token_rejects_weak_or_incomplete_key_configuration") { test_auth_token_rejects_weak_or_incomplete_key_configuration(); };
    it("test_auth_token_rejects_exact_revoked_fingerprint") { test_auth_token_rejects_exact_revoked_fingerprint(); };
    it("test_auth_token_dynamic_revocation_is_fail_closed_and_recovers") { test_auth_token_dynamic_revocation_is_fail_closed_and_recovers(); };
    it("test_auth_token_dynamic_revocation_disables_static_bearer_bypass") { test_auth_token_dynamic_revocation_disables_static_bearer_bypass(); };
    it("test_auth_token_requires_dynamic_revocation_callback_context_pair") { test_auth_token_requires_dynamic_revocation_callback_context_pair(); };
    it("test_auth_token_rejects_malformed_revocation_lists") { test_auth_token_rejects_malformed_revocation_lists(); };
    it("test_auth_token_bounds_revocation_list_cardinality") { test_auth_token_bounds_revocation_list_cardinality(); };
}
