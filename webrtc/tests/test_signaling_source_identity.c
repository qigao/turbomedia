#include "tinytest.h"
#include "signaling/signaling_source_identity.h"

#include <string.h>

static const char CERT_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char CERT_B[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static cnet_stream_peer ipv4_peer(
    uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    cnet_stream_peer peer;
    memset(&peer, 0, sizeof(peer));
    peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
    peer.address[0] = a;
    peer.address[1] = b;
    peer.address[2] = c;
    peer.address[3] = d;
    return peer;
}

void test_direct_mode_ignores_spoofed_forwarded_identity(void) {
    signaling_source_identity_policy_t policy;
    signaling_source_key_t actual;
    signaling_source_key_t expected;
    cnet_stream_peer peer = ipv4_peer(198, 51, 100, 10);
    int used_proxy = 1;

    check_equal(signaling_source_identity_policy_init(&policy, NULL), 0);
    check_false(signaling_source_identity_policy_enabled(&policy));
    check_equal(
        (int)signaling_source_identity_resolve(
            &policy, &peer, CERT_A, "203.0.113.99", &actual, &used_proxy),
        (int)SIGNALING_SOURCE_IDENTITY_OK);
    check_equal(signaling_source_key_from_numeric(
                    "198.51.100.10", &expected), 0);
    check_true(signaling_source_key_equal(&actual, &expected));
    check_false(used_proxy);
}

void test_trusted_proxy_requires_socket_and_certificate_pin(void) {
    signaling_source_identity_policy_t policy;
    signaling_source_key_t actual;
    signaling_source_key_t expected;
    cnet_stream_peer trusted = ipv4_peer(10, 0, 0, 10);
    cnet_stream_peer wrong = ipv4_peer(10, 0, 0, 11);
    int used_proxy = 0;

    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        0);
    check_true(signaling_source_identity_policy_enabled(&policy));

    check_equal(
        (int)signaling_source_identity_resolve(
            &policy, &trusted, CERT_A, "203.0.113.7",
            &actual, &used_proxy),
        (int)SIGNALING_SOURCE_IDENTITY_OK);
    check_true(used_proxy);
    check_equal(signaling_source_key_from_numeric(
                    "203.0.113.7", &expected), 0);
    check_true(signaling_source_key_equal(&actual, &expected));

    check_equal(
        (int)signaling_source_identity_resolve(
            &policy, &trusted, CERT_B, "203.0.113.7",
            &actual, NULL),
        (int)SIGNALING_SOURCE_IDENTITY_UNTRUSTED_PROXY);
    check_equal(
        (int)signaling_source_identity_resolve(
            &policy, &wrong, CERT_A, "203.0.113.7",
            &actual, NULL),
        (int)SIGNALING_SOURCE_IDENTITY_UNTRUSTED_PROXY);
    check_equal(
        (int)signaling_source_identity_resolve(
            &policy, &trusted, NULL, "203.0.113.7",
            &actual, NULL),
        (int)SIGNALING_SOURCE_IDENTITY_UNTRUSTED_PROXY);
}

void test_forwarded_identity_is_strict_single_numeric_ip(void) {
    static const char *const invalid[] = {
        "",
        "203.0.113.7,198.51.100.2",
        "203.0.113.7, 198.51.100.2",
        "203.0.113.7:443",
        " 203.0.113.7",
        "203.0.113.7 ",
        "[2001:db8::7]",
        "2001:db8::7%eth0",
        "client.example.test",
    };
    signaling_source_identity_policy_t policy;
    signaling_source_key_t actual;
    signaling_source_key_t expected;
    cnet_stream_peer trusted = ipv4_peer(10, 0, 0, 10);

    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        0);

    for (size_t index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        check_equal(
            (int)signaling_source_identity_resolve(
                &policy, &trusted, CERT_A, invalid[index],
                &actual, NULL),
            (int)SIGNALING_SOURCE_IDENTITY_INVALID_FORWARDED);
    }

    check_equal(
        (int)signaling_source_identity_resolve(
            &policy, &trusted, CERT_A, "2001:db8::7",
            &actual, NULL),
        (int)SIGNALING_SOURCE_IDENTITY_OK);
    check_equal(signaling_source_key_from_numeric(
                    "2001:db8::7", &expected), 0);
    check_true(signaling_source_key_equal(&actual, &expected));

    check_equal(signaling_source_key_from_numeric(
                    "::ffff:203.0.113.7", &actual), 0);
    check_equal(signaling_source_key_from_numeric(
                    "203.0.113.7", &expected), 0);
    check_true(signaling_source_key_equal(&actual, &expected));
}

void test_proxy_map_is_bounded_canonical_and_duplicate_free(void) {
    signaling_source_identity_policy_t policy;

    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,"
            "2001:db8::10="
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        0);
    check_equal((int)policy.proxy_count, 2);

    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,"
            "10.0.0.10="
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        -1);
    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10="
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        -1);
    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10 ="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        -1);
    check_equal(
        signaling_source_identity_policy_init(
            &policy,
            "10.0.0.10="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,"),
        -1);
}

spec("test_signaling_source_identity") {
    it("test_direct_mode_ignores_spoofed_forwarded_identity") {
        test_direct_mode_ignores_spoofed_forwarded_identity();
    };
    it("test_trusted_proxy_requires_socket_and_certificate_pin") {
        test_trusted_proxy_requires_socket_and_certificate_pin();
    };
    it("test_forwarded_identity_is_strict_single_numeric_ip") {
        test_forwarded_identity_is_strict_single_numeric_ip();
    };
    it("test_proxy_map_is_bounded_canonical_and_duplicate_free") {
        test_proxy_map_is_bounded_canonical_and_duplicate_free();
    };
}
