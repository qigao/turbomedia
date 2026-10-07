#include "tinytest.h"
#include "turbo_media_revocation_https.h"

#include <stddef.h>

static const char *test_token(
    void *context, const char *target_id, unsigned int attempt) {
    (void)context;
    (void)target_id;
    (void)attempt;
    return "ephemeral-security-token";
}

void test_revocation_https_discovers_only_bounded_https_targets(void) {
    turbo_media_revocation_https_config_t config = {
        .timeout_ms = 1000U,
        .acquire_token = test_token};
    turbo_media_revocation_https_target_config_t good[2] = {
        {"signaling-a", "https://127.0.0.1:9443",
         "test-ca.pem", "signaling-a.internal"},
        {"sfu-a", "https://127.0.0.1:9444",
         "test-ca.pem", "sfu-a.internal"}};
    turbo_media_revocation_https_target_config_t bad = good[0];
    turbo_media_revocation_https_t *adapter;
    turbo_media_revocation_fanout_target_t target = {0};

    adapter = turbo_media_revocation_https_create(
        &config, good, 2U);
    check_not_null(adapter);
    check_equal((int)turbo_media_revocation_https_target_count(adapter), 2);
    check_equal(turbo_media_revocation_https_get_fanout_target(
                    adapter, 0U, &target), 0);
    check_equal(target.target_id, "signaling-a");
    check_not_null(target.target_context);
    turbo_media_revocation_https_destroy(adapter);

    bad.base_url = "http://127.0.0.1:9443";
    check_null(turbo_media_revocation_https_create(&config, &bad, 1U));
    bad = good[0];
    bad.ca_file = "";
    check_null(turbo_media_revocation_https_create(&config, &bad, 1U));
    bad = good[0];
    bad.server_name = "";
    check_null(turbo_media_revocation_https_create(&config, &bad, 1U));

    good[1].target_id = "signaling-a";
    check_null(turbo_media_revocation_https_create(&config, good, 2U));
}

void test_revocation_https_requires_ephemeral_token_provider(void) {
    turbo_media_revocation_https_target_config_t target = {
        "node-a", "https://127.0.0.1:9443",
        "test-ca.pem", "node-a.internal"};
    turbo_media_revocation_https_config_t config = {
        .timeout_ms = 1000U};

    check_null(turbo_media_revocation_https_create(
        &config, &target, 1U));
    config.acquire_token = test_token;
    config.timeout_ms = 0U;
    check_null(turbo_media_revocation_https_create(
        &config, &target, 1U));
}

spec("test_revocation_https") {
    it("test_revocation_https_discovers_only_bounded_https_targets") {
        test_revocation_https_discovers_only_bounded_https_targets();
    };
    it("test_revocation_https_requires_ephemeral_token_provider") {
        test_revocation_https_requires_ephemeral_token_provider();
    };
}
