#include "ivr_certificate_identity.h"
#include "tinytest.h"

#include <stdint.h>

typedef struct {
    uint64_t now_ms;
} identity_clock_t;

static uint64_t identity_test_clock(void *context) {
    return ((identity_clock_t *)context)->now_ms;
}

static const char ACTIVE_CERT[] =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
static const char PREVIOUS_CERT[] =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

spec("ivr certificate identity") {
    it("matches the active certificate and claimed worker exactly") {
        identity_clock_t clock = {100u};
        ivr_certificate_identity_entry_t entry = {
            "worker-a", ACTIVE_CERT, NULL, 0u, 7u};
        ivr_certificate_identity_config_t config =
            IVR_CERTIFICATE_IDENTITY_CONFIG_INIT;
        ivr_certificate_identity_t *identity = NULL;
        config.entries = &entry;
        config.entry_count = 1u;
        config.clock = identity_test_clock;
        config.clock_context = &clock;
        check_equal(ivr_certificate_identity_create(&config, &identity), 0);
        check_not_null(identity);
        check_equal(ivr_certificate_identity_verify(identity, ACTIVE_CERT,
                                                     "worker-a"), 0);
        check_true(ivr_certificate_identity_verify(identity, ACTIVE_CERT,
                                                   "worker-b") != 0);
        ivr_certificate_identity_destroy(identity);
    }

    it("accepts previous certificate only before its expiry") {
        identity_clock_t clock = {199u};
        ivr_certificate_identity_entry_t entry = {
            "worker-a", ACTIVE_CERT, PREVIOUS_CERT, 200u, 8u};
        ivr_certificate_identity_config_t config =
            IVR_CERTIFICATE_IDENTITY_CONFIG_INIT;
        ivr_certificate_identity_t *identity = NULL;
        config.entries = &entry;
        config.entry_count = 1u;
        config.clock = identity_test_clock;
        config.clock_context = &clock;
        check_equal(ivr_certificate_identity_create(&config, &identity), 0);
        check_equal(ivr_certificate_identity_verify(identity, PREVIOUS_CERT,
                                                     "worker-a"), 0);
        clock.now_ms = 200u;
        check_true(ivr_certificate_identity_verify(identity, PREVIOUS_CERT,
                                                   "worker-a") != 0);
        check_equal(ivr_certificate_identity_verify(identity, ACTIVE_CERT,
                                                     "worker-a"), 0);
        ivr_certificate_identity_destroy(identity);
    }

    it("rejects malformed, duplicate, and unbounded mappings") {
        identity_clock_t clock = {100u};
        ivr_certificate_identity_entry_t invalid = {
            "worker-a", "sha256:ABC", NULL, 0u, 1u};
        ivr_certificate_identity_config_t config =
            IVR_CERTIFICATE_IDENTITY_CONFIG_INIT;
        ivr_certificate_identity_t *identity = NULL;
        config.entries = &invalid;
        config.entry_count = 1u;
        config.clock = identity_test_clock;
        config.clock_context = &clock;
        check_not_equal(ivr_certificate_identity_create(&config, &identity), 0);
        check_null(identity);

        {
            ivr_certificate_identity_entry_t duplicate[2] = {
                {"worker-a", ACTIVE_CERT, NULL, 0u, 1u},
                {"worker-b", ACTIVE_CERT, NULL, 0u, 1u}};
            config.entries = duplicate;
            config.entry_count = 2u;
            check_not_equal(ivr_certificate_identity_create(&config, &identity), 0);
            check_null(identity);
        }
    }
}
