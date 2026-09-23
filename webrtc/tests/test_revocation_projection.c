#include "tinytest.h"
#include "turbo_media_revocation_projection.h"

#include <stdint.h>
#include <string.h>

static const char DIGEST_ZERO[] =
    "0000000000000000000000000000000000000000000000000000000000000000";
static const char DIGEST_ONE[] =
    "1111111111111111111111111111111111111111111111111111111111111111";

static uint8_t hex_nibble(unsigned char value) {
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        return (uint8_t)(value - (unsigned char)'0');
    }
    return (uint8_t)(value - (unsigned char)'a' + 10U);
}

static void digest_from_hex(
    const char *value,
    uint8_t output[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES]) {
    for (size_t index = 0U;
         index < TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES; ++index) {
        uint8_t high = hex_nibble((unsigned char)value[index * 2U]);
        uint8_t low = hex_nibble((unsigned char)value[index * 2U + 1U]);
        output[index] = (uint8_t)((uint8_t)(high << 4U) | low);
    }
}

void test_revocation_projection_is_bounded_thread_safe_owner(void) {
    turbo_media_revocation_projection_t *projection =
        turbo_media_revocation_projection_create(2U);
    uint8_t zero[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    uint8_t one[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t count = 0U;

    check_not_null(projection);
    digest_from_hex(DIGEST_ZERO, zero);
    digest_from_hex(DIGEST_ONE, one);

    check_equal((int)turbo_media_revocation_projection_check_digest(
                    projection, zero, sizeof(zero)),
                (int)TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN);
    check_equal(turbo_media_revocation_projection_status(
                    projection, &synchronized, &epoch, &sequence, &count),
                0);
    check_false(synchronized);

    check_equal((int)turbo_media_revocation_projection_apply_snapshot(
                    projection, 1U, 0U, NULL, 0U),
                (int)TURBO_MEDIA_REVOCATION_APPLY_APPLIED);
    check_equal((int)turbo_media_revocation_projection_check_digest(
                    projection, zero, sizeof(zero)),
                (int)TURBO_MEDIA_AUTH_REVOCATION_CLEAR);

    check_equal((int)turbo_media_revocation_projection_apply_revoke(
                    projection, 1U, 1U, DIGEST_ZERO),
                (int)TURBO_MEDIA_REVOCATION_APPLY_APPLIED);
    check_equal((int)turbo_media_revocation_projection_check_digest(
                    projection, zero, sizeof(zero)),
                (int)TURBO_MEDIA_AUTH_REVOCATION_REVOKED);

    check_equal((int)turbo_media_revocation_projection_apply_revoke(
                    projection, 1U, 3U, DIGEST_ONE),
                (int)TURBO_MEDIA_REVOCATION_APPLY_GAP);
    check_equal((int)turbo_media_revocation_projection_check_digest(
                    projection, one, sizeof(one)),
                (int)TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN);

    check_equal((int)turbo_media_revocation_projection_apply_snapshot(
                    projection, 1U, 3U, NULL, 0U),
                (int)TURBO_MEDIA_REVOCATION_APPLY_APPLIED);
    check_equal(turbo_media_revocation_projection_status(
                    projection, &synchronized, &epoch, &sequence, &count),
                0);
    check_true(synchronized);
    check_equal((int)epoch, 1);
    check_equal((int)sequence, 3);
    check_equal((int)count, 0);

    check_equal((int)turbo_media_revocation_projection_apply_revoke(
                    projection, 1U, 4U,
                    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
                (int)TURBO_MEDIA_REVOCATION_APPLY_ERROR);
    check_equal((int)turbo_media_revocation_projection_check_digest(
                    projection, zero, sizeof(zero)),
                (int)TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN);

    turbo_media_revocation_projection_destroy(projection);
}

spec("test_revocation_projection") {
    it("test_revocation_projection_is_bounded_thread_safe_owner") {
        test_revocation_projection_is_bounded_thread_safe_owner();
    };
}
