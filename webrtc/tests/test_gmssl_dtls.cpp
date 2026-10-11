#include "gmssl_dtls.h"
#include "dc_certificate.h"
#include "dc_certificate_der.h"
#include <tinytest.hpp>
#include <cmeta_crypto.h>
#include <cstl/deque.h>
#include <openssl/err.h>
#include <openssl/srtp.h>
#include <openssl/x509.h>

#include <cstring>

namespace {
uint64_t clock_ms;
void test_clock(const SSL *, struct timeval *out) {
    out->tv_sec = static_cast<decltype(out->tv_sec)>(clock_ms / 1000);
    out->tv_usec = static_cast<decltype(out->tv_usec)>((clock_ms % 1000) * 1000);
}
struct Packets { deque_t values; size_t bytes; };
int bio_create(BIO *bio) {
    auto *queue = new Packets{};
    if (deque_init_bytes(&queue->values, sizeof(tstr), alignof(tstr), 256) != STL_OK) {
        delete queue; return 0;
    }
    BIO_set_data(bio, queue); BIO_set_init(bio, 1); return 1;
}
int bio_free(BIO *bio) {
    auto *queue = static_cast<Packets *>(BIO_get_data(bio));
    if (queue) {
        tstr packet;
        while (deque_pop_front(&queue->values, &packet) == STL_OK) tstr_free(packet);
        deque_destroy(&queue->values); delete queue;
    }
    return 1;
}
int bio_write(BIO *bio, const char *data, int size) {
    BIO_clear_retry_flags(bio);
    auto *queue = static_cast<Packets *>(BIO_get_data(bio));
    if (size <= 0 || size > 65535 || deque_size(&queue->values) == 256 ||
        static_cast<size_t>(size) > 256 * 1024 - queue->bytes) return -1;
    tstr packet = tstr_new_len(data, static_cast<size_t>(size));
    if (!packet || deque_push_back(&queue->values, &packet) != STL_OK) {
        tstr_free(packet); return -1;
    }
    queue->bytes += static_cast<size_t>(size); return size;
}
int bio_read(BIO *bio, char *data, int size) {
    BIO_clear_retry_flags(bio);
    if (size <= 0) return 0;
    auto *queue = static_cast<Packets *>(BIO_get_data(bio));
    tstr packet = nullptr;
    if (deque_pop_front(&queue->values, &packet) != STL_OK) {
        BIO_set_retry_read(bio); return -1;
    }
    size_t n = tstr_len(packet); queue->bytes -= n;
    if (n > static_cast<size_t>(size)) n = static_cast<size_t>(size);
    memcpy(data, packet, n); tstr_free(packet); return static_cast<int>(n);
}
long bio_ctrl(BIO *bio, int command, long, void *) {
    auto *queue = static_cast<Packets *>(BIO_get_data(bio));
    if (command == BIO_CTRL_FLUSH) return 1;
    if (command == BIO_CTRL_PENDING) {
        auto *packet = static_cast<tstr *>(deque_front(&queue->values));
        return packet ? static_cast<long>(tstr_len(*packet)) : 0;
    }
    return 0;
}
const BIO_METHOD datagram_method = {
    BIO_TYPE_SOURCE_SINK, "GmSSL DTLS interoperability datagrams", bio_write,
    bio_read, nullptr, nullptr, bio_ctrl, bio_create, bio_free, nullptr
};
int accept_pinned_test_peer(int, X509_STORE_CTX *) { return 1; }
bool contains_finished_record(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    while (size >= 13) {
        size_t record_size = 13 + (static_cast<size_t>(p[11]) << 8) + p[12];
        if (record_size > size) return false;
        if (p[0] == 22 && p[3] == 0 && p[4] == 1) return true;
        p += record_size; size -= record_size;
    }
    return false;
}

struct Fixture {
    SSL_CTX *reference_context = nullptr;
    SSL *reference = nullptr;
    turbo_gdtls_context *context = nullptr;
    turbo_gdtls *session = nullptr;
    tstr certificate = nullptr, private_key = nullptr, fingerprint = nullptr;
    tstr owned_packet = nullptr, held_packet = nullptr;
    bool server = false;
    bool drop_first = false, drop_final = false, reorder = false, dropped = false;
    int gm_error = 0, ssl_error = 0;
    uint8_t reference_plain[16384]{};
    size_t reference_plain_size = 0;

    void clear() {
        turbo_gdtls_destroy(session); session = nullptr;
        turbo_gdtls_context_destroy(context); context = nullptr;
        SSL_free(reference); reference = nullptr;
        SSL_CTX_free(reference_context); reference_context = nullptr;
        if (private_key) cmeta_crypto_clear(private_key, tstr_len(private_key));
        tstr_free(private_key); private_key = nullptr;
        tstr_free(certificate); certificate = nullptr;
        tstr_free(fingerprint); fingerprint = nullptr;
        tstr_free(owned_packet); owned_packet = nullptr;
        tstr_free(held_packet); held_packet = nullptr;
    }
    bool init(bool server_role, uint16_t profile, bool aes256 = false, bool wrong_pin = false) {
        server = server_role; clock_ms = 1000000;
        reference_context = SSL_CTX_new(DTLS_method());
        if (!reference_context || turbo_dc_generate_identity(reference_context, &fingerprint) != 0 ||
            turbo_dc_generate_certificate_der(&certificate, &private_key) != 0) return false;
        context = turbo_gdtls_context_create(certificate, tstr_len(certificate),
                                             private_key, tstr_len(private_key));
        if (!context) return false;
        uint8_t expected[32]; unsigned int size = 0;
        if (X509_digest(SSL_CTX_get0_certificate(reference_context), EVP_sha256(), expected, &size) != 1 || size != 32)
            return false;
        if (wrong_pin) expected[0] ^= 1;
        session = turbo_gdtls_create(context, server ? 1 : 0, 300, expected, &profile, 1);
        if (!session) return false;
        const char *profile_name = profile == 1 ? "SRTP_AES128_CM_SHA1_80" :
            profile == 2 ? "SRTP_AES128_CM_SHA1_32" :
            profile == 7 ? "SRTP_AEAD_AES_128_GCM" : "SRTP_AEAD_AES_256_GCM";
        if (SSL_CTX_set_tlsext_use_srtp(reference_context, profile_name) != 0 ||
            SSL_CTX_set_cipher_list(reference_context, aes256 ? "ECDHE-ECDSA-AES256-GCM-SHA384" :
                                                               "ECDHE-ECDSA-AES128-GCM-SHA256") != 1 ||
            SSL_CTX_set_min_proto_version(reference_context, DTLS1_2_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(reference_context, DTLS1_2_VERSION) != 1) return false;
        SSL_CTX_set_current_time_cb(reference_context, test_clock);
        SSL_CTX_set_verify(reference_context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           accept_pinned_test_peer);
        reference = SSL_new(reference_context);
        if (!reference) return false;
        BIO *input = BIO_new(&datagram_method), *output = BIO_new(&datagram_method);
        if (!input || !output) { BIO_free(input); BIO_free(output); return false; }
        SSL_set_bio(reference, input, output); SSL_set_mtu(reference, 300);
        if (server) SSL_set_connect_state(reference); else SSL_set_accept_state(reference);
        return turbo_gdtls_start(session, clock_ms) == 0;
    }
    bool transfer_to_reference(tstr packet) {
        return BIO_write(SSL_get_rbio(reference), packet, static_cast<int>(tstr_len(packet))) ==
               static_cast<int>(tstr_len(packet));
    }
    bool step() {
        clock_ms += 20;
        gm_error = turbo_gdtls_poll(session, clock_ms);
        if (gm_error) return false;
        while ((owned_packet = turbo_gdtls_take_datagram(session)) != nullptr) {
            bool encrypted_handshake = contains_finished_record(owned_packet, tstr_len(owned_packet));
            if (!dropped && (drop_first || (drop_final && server && encrypted_handshake))) dropped = true;
            else if (reorder && !held_packet) { held_packet = owned_packet; owned_packet = nullptr; }
            else {
                if (!transfer_to_reference(owned_packet)) return false;
                if (held_packet) {
                    if (!transfer_to_reference(held_packet)) return false;
                    tstr_free(held_packet); held_packet = nullptr;
                }
            }
            tstr_free(owned_packet); owned_packet = nullptr;
        }
        // Do not indefinitely hold an odd flight's final packet.
        if (held_packet) {
            if (!transfer_to_reference(held_packet)) return false;
            tstr_free(held_packet); held_packet = nullptr;
        }
        int result;
        if (SSL_is_init_finished(reference)) {
            result = SSL_read(reference, reference_plain, sizeof(reference_plain));
            if (result > 0) reference_plain_size = static_cast<size_t>(result);
        } else result = SSL_do_handshake(reference);
        if (result <= 0) {
            ssl_error = SSL_get_error(reference, result);
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) return false;
        }
        if (DTLSv1_handle_timeout(reference) < 0) return false;
        uint8_t packet[65535];
        while ((result = BIO_read(SSL_get_wbio(reference), packet, sizeof(packet))) > 0) {
            bool encrypted_handshake = contains_finished_record(packet, static_cast<size_t>(result));
            if (!dropped && drop_final && !server && encrypted_handshake) dropped = true;
            else {
                gm_error = turbo_gdtls_receive(session, packet, static_cast<size_t>(result), clock_ms);
                if (gm_error) return false;
            }
        }
        return true;
    }
    bool handshake() {
        for (unsigned i = 0; i < 2000; ++i) {
            if (!step()) return false;
            if (turbo_gdtls_ready(session) && SSL_is_init_finished(reference)) return true;
        }
        return false;
    }
};
Fixture fixture;
void check_interop(uint16_t profile) {
    bool connected = fixture.handshake();
    capture(fixture.gm_error, "%d"); capture(fixture.ssl_error, "%d");
    unsigned long reference_error = ERR_peek_error();
    capture(reference_error, "%lu");
    check_true(connected);
    check_equal(turbo_gdtls_srtp_profile(fixture.session), profile);
    const SRTP_PROTECTION_PROFILE *selected = SSL_get_selected_srtp_profile(fixture.reference);
    check_not_null(selected);
    check_equal(selected->id, static_cast<unsigned long>(profile));
    uint8_t actual[88], expected[88]; size_t size = 0;
    check_equal(turbo_gdtls_export_srtp(fixture.session, actual, sizeof(actual), &size), 0);
    constexpr char label[] = "EXTRACTOR-dtls_srtp";
    check_equal(SSL_export_keying_material(fixture.reference, expected, size, label,
                                          sizeof(label) - 1, nullptr, 0, 0), 1);
    check_equal(actual, expected, size);
    X509 *peer = SSL_get_peer_certificate(fixture.reference);
    check_not_null(peer);
    unsigned int digest_size = 0;
    int digest_result = X509_digest(peer, EVP_sha256(), actual, &digest_size);
    X509_free(peer);
    check_equal(digest_result, 1);
    check_equal(turbo_gdtls_context_fingerprint(fixture.context, expected), 0);
    check_equal(actual, expected, size_t{32});

    constexpr char payload[] = "one complete SCTP datagram";
    check_equal(turbo_gdtls_write(fixture.session, payload, sizeof(payload)), 0);
    check_true(fixture.step());
    check_equal(fixture.reference_plain_size, sizeof(payload));
    check_equal(fixture.reference_plain, payload, sizeof(payload));
    check_equal(SSL_write(fixture.reference, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    check_true(fixture.step());
    fixture.owned_packet = turbo_gdtls_take_plaintext(fixture.session);
    check_not_null(fixture.owned_packet);
    check_equal(tstr_len(fixture.owned_packet), sizeof(payload));
    check_equal(fixture.owned_packet, payload, sizeof(payload));
}
}

suite("GmSSL DTLS-SRTP independent interoperability") {
    after_each() { fixture.clear(); fixture = Fixture{}; }
    for (int server = 0; server < 2; ++server) {
        for (uint16_t profile : {uint16_t{1}, uint16_t{2}, uint16_t{7}, uint16_t{8}}) {
            for (int aes256 = 0; aes256 < 2; ++aes256) {
                it("role=%d profile=%u AES-%d, fragmented mutual handshake and payload", server,
                   static_cast<unsigned>(profile), aes256 ? 256 : 128) {
                    check_true(fixture.init(server != 0, profile, aes256 != 0));
                    check_interop(profile);
                }
            }
        }
        it("role=%d recovers ordinary first-flight datagram loss", server) {
            check_true(fixture.init(server != 0, 1));
            fixture.drop_first = true;
            check_interop(1); check_true(fixture.dropped);
        }
        it("role=%d recovers a lost final server Finished", server) {
            check_true(fixture.init(server != 0, 7));
            fixture.drop_final = true;
            check_interop(7); check_true(fixture.dropped);
        }
        it("role=%d reassembles reordered flight fragments", server) {
            check_true(fixture.init(server != 0, 8));
            fixture.reorder = true;
            check_interop(8);
        }
        it("role=%d rejects an incorrect peer fingerprint before key export", server) {
            check_true(fixture.init(server != 0, 1, false, true));
            check_false(fixture.handshake());
            check_equal(fixture.gm_error, TURBO_GDTLS_AUTH);
            check_false(turbo_gdtls_ready(fixture.session));
            uint8_t keys[88]; size_t size = 123;
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), TURBO_GDTLS_AUTH);
            check_equal(size, size_t{0});
        }
    }
}
