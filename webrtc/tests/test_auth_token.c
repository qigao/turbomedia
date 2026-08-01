#include "tinytest_compat.h"
#include "turbo_media_auth.h"
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
    TEST_ASSERT_NOT_NULL(authorization);
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

static void token_sha256_hex(const char *token, char output[65]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];

    TEST_ASSERT_EQUAL_INT(
        TURBO_CRYPTO_OK,
        turbo_crypto_sha256(token, strlen(token), digest));
    for (size_t index = 0; index < sizeof(digest); ++index) {
        output[index * 2U] = hex[digest[index] >> 4U];
        output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    output[64] = '\0';
}

void test_auth_token_accepts_exact_scope_and_resource(void) {
    TEST_ASSERT_EQUAL_INT(0, turbo_media_auth_config_validate(&active_config));
    char *token = issue_token(
        &active_config, "sfu.media.publish sfu.media.trickle", "room-a",
        "alice", TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    TEST_ASSERT_NOT_NULL(token);
    authorization = authorization_for(token);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_SIGNED_TOKEN,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));

    free(authorization);
    free(token);
}

void test_auth_token_rejects_cross_room_and_participant_use(void) {
    char *token = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    TEST_ASSERT_NOT_NULL(token);
    authorization = authorization_for(token);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-b", "alice", TEST_NOW_SECONDS + 1));
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "bob", TEST_NOW_SECONDS + 1));
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &active_config, "sfu.media.subscribe",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));

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

    TEST_ASSERT_NOT_NULL(expired);
    TEST_ASSERT_NULL(overlong);
    authorization = authorization_for(expired);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS));

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
    TEST_ASSERT_NOT_NULL(token);
    authorization = authorization_for(token);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_SIGNED_TOKEN,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));

    free(authorization);
    free(token);
}

void test_auth_token_rejects_tampering_and_unknown_key(void) {
    turbo_media_auth_config_t unrelated = active_config;
    char *token = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;

    TEST_ASSERT_NOT_NULL(token);
    token[strlen(token) - 1U] =
        token[strlen(token) - 1U] == 'A' ? 'B' : 'A';
    authorization = authorization_for(token);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));
    free(authorization);
    free(token);

    unrelated.active_key_id = "unrelated";
    unrelated.previous_key_id = NULL;
    unrelated.previous_secret = NULL;
    token = issue_token(
        &unrelated, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    TEST_ASSERT_NOT_NULL(token);
    authorization = authorization_for(token);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));

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

    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_STATIC_TOKEN,
        turbo_media_auth_authorize(
            "Bearer legacy-token", "legacy-token", &active_config, &policy));
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        turbo_media_auth_authorize(
            "Bearer wrong-token", "legacy-token", &active_config, &policy));
}

void test_auth_token_rejects_weak_or_incomplete_key_configuration(void) {
    turbo_media_auth_config_t weak = active_config;
    turbo_media_auth_config_t incomplete_rotation = active_config;

    weak.active_secret = "human-password";
    incomplete_rotation.previous_secret = NULL;
    TEST_ASSERT_EQUAL_INT(-1, turbo_media_auth_config_validate(&weak));
    TEST_ASSERT_EQUAL_INT(
        -1, turbo_media_auth_config_validate(&incomplete_rotation));
}

void test_auth_token_rejects_exact_revoked_fingerprint(void) {
    turbo_media_auth_config_t revoked = active_config;
    char *token = issue_token(
        &active_config, "sfu.media.publish", "room-a", "alice",
        TEST_NOW_SECONDS, TEST_NOW_SECONDS + 120);
    char *authorization;
    char digest[65];
    char revocation_list[130];

    TEST_ASSERT_NOT_NULL(token);
    token_sha256_hex(token, digest);
    snprintf(
        revocation_list, sizeof(revocation_list),
        "0000000000000000000000000000000000000000000000000000000000000000,%s",
        digest);
    revoked.revoked_token_sha256 = revocation_list;
    TEST_ASSERT_EQUAL_INT(0, turbo_media_auth_config_validate(&revoked));
    authorization = authorization_for(token);
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_DENIED,
        authorize(authorization, &revoked, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));
    TEST_ASSERT_EQUAL_INT(
        TURBO_MEDIA_AUTH_SIGNED_TOKEN,
        authorize(authorization, &active_config, "sfu.media.publish",
                  "room-a", "alice", TEST_NOW_SECONDS + 1));

    free(authorization);
    free(token);
}

void test_auth_token_rejects_malformed_revocation_lists(void) {
    turbo_media_auth_config_t malformed = active_config;

    malformed.revoked_token_sha256 = "abc";
    TEST_ASSERT_EQUAL_INT(-1, turbo_media_auth_config_validate(&malformed));
    malformed.revoked_token_sha256 =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    TEST_ASSERT_EQUAL_INT(-1, turbo_media_auth_config_validate(&malformed));
    malformed.revoked_token_sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000,";
    TEST_ASSERT_EQUAL_INT(-1, turbo_media_auth_config_validate(&malformed));
}

void test_auth_token_bounds_revocation_list_cardinality(void) {
    turbo_media_auth_config_t bounded = active_config;
    const size_t maximum = TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS;
    const size_t valid_length = maximum * 64U + maximum - 1U;
    char *list = (char *)malloc(valid_length + 66U);
    size_t offset = 0U;

    TEST_ASSERT_NOT_NULL(list);
    for (size_t entry = 0; entry < maximum; ++entry) {
        memset(list + offset, '0', 64U);
        offset += 64U;
        if (entry + 1U < maximum) {
            list[offset++] = ',';
        }
    }
    list[offset] = '\0';
    bounded.revoked_token_sha256 = list;
    TEST_ASSERT_EQUAL_INT(0, turbo_media_auth_config_validate(&bounded));

    list[offset++] = ',';
    memset(list + offset, '1', 64U);
    offset += 64U;
    list[offset] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, turbo_media_auth_config_validate(&bounded));
    free(list);
}

spec("test_auth_token") {
    TT_TEST(test_auth_token_accepts_exact_scope_and_resource);
    TT_TEST(test_auth_token_rejects_cross_room_and_participant_use);
    TT_TEST(test_auth_token_rejects_expired_and_overlong_tokens);
    TT_TEST(test_auth_token_supports_one_previous_rotation_key);
    TT_TEST(test_auth_token_rejects_tampering_and_unknown_key);
    TT_TEST(test_auth_token_keeps_static_bearer_compatibility_explicit);
    TT_TEST(test_auth_token_rejects_weak_or_incomplete_key_configuration);
    TT_TEST(test_auth_token_rejects_exact_revoked_fingerprint);
    TT_TEST(test_auth_token_rejects_malformed_revocation_lists);
    TT_TEST(test_auth_token_bounds_revocation_list_cardinality);
}
