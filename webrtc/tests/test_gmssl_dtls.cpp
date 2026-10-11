#include "gmssl_dtls.h"
#include "dc_certificate.h"
#include "dc_certificate_der.h"
#include <tinytest.hpp>
#include <cmeta_crypto.h>
#include <cstl/deque.h>
#include <openssl/bn.h>
#include <openssl/bytestring.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
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
// Generate independent RSA fixtures at runtime; no reusable private keys live
// in the repository. Only the test peer/fixture uses BoringSSL primitives.
bool rsa_identity(SSL_CTX *context, tstr *certificate = nullptr, tstr *private_key = nullptr) {
    bssl::UniquePtr<RSA> rsa(RSA_new());
    bssl::UniquePtr<BIGNUM> exponent(BN_new());
    bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
    bssl::UniquePtr<X509> cert(X509_new());
    if (!rsa || !exponent || !key || !cert || BN_set_word(exponent.get(), RSA_F4) != 1 ||
        RSA_generate_key_ex(rsa.get(), 2048, exponent.get(), nullptr) != 1 ||
        EVP_PKEY_set1_RSA(key.get(), rsa.get()) != 1 || X509_set_version(cert.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400) ||
        X509_set_pubkey(cert.get(), key.get()) != 1) return false;
    X509_NAME *name = X509_get_subject_name(cert.get());
    constexpr unsigned char cn[] = "DTLS RSA interoperability fixture";
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, cn, -1, -1, 0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1 ||
        X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) return false;
    if (context && (SSL_CTX_use_certificate(context, cert.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context, key.get()) != 1)) return false;
    if (certificate && private_key) {
        int size = i2d_X509(cert.get(), nullptr);
        if (size <= 0 || !(*certificate = tstr_new_len(nullptr, static_cast<size_t>(size)))) return false;
        auto *out = reinterpret_cast<uint8_t *>(*certificate);
        if (i2d_X509(cert.get(), &out) != size) return false;
        bssl::ScopedCBB bytes;
        if (!CBB_init(bytes.get(), 2048) || !EVP_marshal_private_key(bytes.get(), key.get())) return false;
        *private_key = tstr_new_len(CBB_data(bytes.get()), CBB_len(bytes.get()));
        cmeta_crypto_clear(const_cast<uint8_t *>(CBB_data(bytes.get())), CBB_len(bytes.get()));
        if (!*private_key) return false;
    }
    return true;
}
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
    tstr owned_packet = nullptr, held_packet = nullptr, reference_held_packet = nullptr;
    bool server = false;
    bool drop_first = false, drop_final = false, reorder = false, dropped = false;
    bool reorder_reference = false, early_application = false, early_sent = false;
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
        tstr_free(reference_held_packet); reference_held_packet = nullptr;
    }
    bool init(bool server_role, uint16_t profile, bool aes256 = false, bool wrong_pin = false,
              bool engine_rsa = false, bool reference_rsa = false) {
        ERR_clear_error();
        server = server_role; clock_ms = 1000000;
        reference_context = SSL_CTX_new(DTLS_method());
        if (!reference_context) return false;
        if (reference_rsa ? !rsa_identity(reference_context) :
            turbo_dc_generate_identity(reference_context, &fingerprint) != 0) return false;
        if (engine_rsa ? !rsa_identity(nullptr, &certificate, &private_key) :
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
        bool rsa_server = server ? engine_rsa : reference_rsa;
        const char *cipher = rsa_server ? (aes256 ? "ECDHE-RSA-AES256-GCM-SHA384" : "ECDHE-RSA-AES128-GCM-SHA256") :
            (aes256 ? "ECDHE-ECDSA-AES256-GCM-SHA384" : "ECDHE-ECDSA-AES128-GCM-SHA256");
        // The engine admits SHA-256 signatures for both supported identity types.
        const uint16_t signatures[] = {SSL_SIGN_ECDSA_SECP256R1_SHA256, SSL_SIGN_RSA_PKCS1_SHA256};
        if (SSL_CTX_set_tlsext_use_srtp(reference_context, profile_name) != 0 ||
            SSL_CTX_set_verify_algorithm_prefs(reference_context, signatures, 2) != 1 ||
            SSL_CTX_set_signing_algorithm_prefs(reference_context, signatures, 2) != 1 ||
            SSL_CTX_set_cipher_list(reference_context, cipher) != 1 ||
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
        if (early_application && !early_sent && SSL_is_init_finished(reference)) {
            if (SSL_write(reference, "early", 5) != 5) return false;
            early_sent = true;
        }
        uint8_t packet[65535];
        while ((result = BIO_read(SSL_get_wbio(reference), packet, sizeof(packet))) > 0) {
            bool encrypted_handshake = contains_finished_record(packet, static_cast<size_t>(result));
            if (!dropped && drop_final && !server && encrypted_handshake) dropped = true;
            else if (reorder_reference && !reference_held_packet) {
                reference_held_packet = tstr_new_len(packet, static_cast<size_t>(result));
                if (!reference_held_packet) return false;
            }
            else {
                gm_error = turbo_gdtls_receive(session, packet, static_cast<size_t>(result), clock_ms);
                if (gm_error) return false;
                if (reference_held_packet) {
                    gm_error = turbo_gdtls_receive(session, reference_held_packet,
                                                   tstr_len(reference_held_packet), clock_ms);
                    tstr_free(reference_held_packet); reference_held_packet = nullptr;
                    if (gm_error) return false;
                }
            }
        }
        if (reference_held_packet) {
            gm_error = turbo_gdtls_receive(session, reference_held_packet,
                                           tstr_len(reference_held_packet), clock_ms);
            tstr_free(reference_held_packet); reference_held_packet = nullptr;
            if (gm_error) return false;
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
    bool capture_reference_record() {
        if (owned_packet) return false;
        constexpr char payload[] = "authenticated datagram";
        if (SSL_write(reference, payload, sizeof(payload)) != static_cast<int>(sizeof(payload))) return false;
        uint8_t bytes[65535];
        int size = BIO_read(SSL_get_wbio(reference), bytes, sizeof(bytes));
        if (size <= 0) return false;
        owned_packet = tstr_new_len(bytes, static_cast<size_t>(size));
        return owned_packet != nullptr;
    }
    int receive_owned() {
        return turbo_gdtls_receive(session, owned_packet, tstr_len(owned_packet), clock_ms);
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
        for (int engine_rsa = 0; engine_rsa < 2; ++engine_rsa) {
            for (int reference_rsa = 0; reference_rsa < 2; ++reference_rsa) {
                if (!engine_rsa && !reference_rsa) continue;
                it("role=%d engine RSA=%d reference RSA=%d, mutual identity and exporter", server,
                   engine_rsa, reference_rsa) {
                    check_true(fixture.init(server != 0, 8, true, false, engine_rsa != 0, reference_rsa != 0));
                    check_interop(8);
                }
            }
        }
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
        it("role=%d reassembles independently reordered reference flight fragments", server) {
            check_true(fixture.init(server != 0, 8));
            fixture.reorder_reference = true;
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
        it("role=%d discards damaged and repeated records without losing the association", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_true(fixture.capture_reference_record());
            size_t last = tstr_len(fixture.owned_packet) - 1;
            fixture.owned_packet[last] ^= 1;
            check_equal(fixture.receive_owned(), 0);
            check_null(turbo_gdtls_take_plaintext(fixture.session));
            fixture.owned_packet[last] ^= 1;
            check_equal(fixture.receive_owned(), 0);
            fixture.held_packet = turbo_gdtls_take_plaintext(fixture.session);
            check_not_null(fixture.held_packet);
            check_equal(fixture.held_packet, "authenticated datagram");
            check_equal(fixture.receive_owned(), 0);
            check_null(turbo_gdtls_take_plaintext(fixture.session));
            check_true(turbo_gdtls_ready(fixture.session));
        }
        it("role=%d enforces export readiness and capacity without partial keys", server) {
            check_true(fixture.init(server != 0, 8));
            uint8_t keys[88]; memset(keys, 0xa5, sizeof(keys));
            size_t size = 123;
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), TURBO_GDTLS_AUTH);
            check_equal(size, size_t{0});
            check_true(fixture.handshake());
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys) - 1, &size), TURBO_GDTLS_CAPACITY);
            check_equal(size, size_t{0});
            for (uint8_t byte : keys) check_equal(byte, uint8_t{0xa5});
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), 0);
            check_equal(size, sizeof(keys));
        }
        it("role=%d exposes the handshake lifetime even with no incoming flight", server) {
            check_true(fixture.init(server != 0, 1));
            check_equal(turbo_gdtls_deadline(fixture.session), clock_ms + (server ? 60000u : 1000u));
            clock_ms += 60000;
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_TIMEOUT);
            check_equal(turbo_gdtls_deadline(fixture.session), uint64_t{0});
            check_null(turbo_gdtls_take_datagram(fixture.session));
            check_false(turbo_gdtls_ready(fixture.session));
        }
        it("role=%d cannot extend the handshake lifetime by receiving without polling", server) {
            check_true(fixture.init(server != 0, 1));
            clock_ms += 60000;
            check_equal(turbo_gdtls_receive(fixture.session, nullptr, 0, clock_ms), TURBO_GDTLS_TIMEOUT);
            check_equal(turbo_gdtls_deadline(fixture.session), uint64_t{0});
        }
        it("role=%d fails explicitly when the admitted output packet budget is exhausted", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            for (unsigned i = 0; i < 256; ++i)
                check_equal(turbo_gdtls_write(fixture.session, "x", 1), 0);
            check_equal(turbo_gdtls_write(fixture.session, "x", 1), TURBO_GDTLS_CAPACITY);
            check_false(turbo_gdtls_ready(fixture.session));
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_CAPACITY);
            check_null(turbo_gdtls_take_datagram(fixture.session));
        }
        it("role=%d drains local close_notify after revoking readiness and exporter access", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_equal(turbo_gdtls_close(fixture.session), 0);
            check_false(turbo_gdtls_ready(fixture.session));
            check_equal(turbo_gdtls_close(fixture.session), TURBO_GDTLS_CLOSED);
            uint8_t keys[88]; size_t size = 123;
            check_equal(turbo_gdtls_export_srtp(fixture.session, keys, sizeof(keys), &size), TURBO_GDTLS_AUTH);
            check_equal(size, size_t{0});
            fixture.owned_packet = turbo_gdtls_take_datagram(fixture.session);
            check_not_null(fixture.owned_packet);
            check_true(fixture.transfer_to_reference(fixture.owned_packet));
            uint8_t data[1];
            int result = SSL_read(fixture.reference, data, sizeof(data));
            check_equal(result, 0);
            check_equal(SSL_get_error(fixture.reference, result), SSL_ERROR_ZERO_RETURN);
        }
        it("role=%d closes on authenticated close_notify and returns the closing alert", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_equal(SSL_shutdown(fixture.reference), 0);
            uint8_t packet[512];
            int size = BIO_read(SSL_get_wbio(fixture.reference), packet, sizeof(packet));
            check_greater(size, 0);
            check_equal(turbo_gdtls_receive(fixture.session, packet, static_cast<size_t>(size), clock_ms), TURBO_GDTLS_CLOSED);
            check_false(turbo_gdtls_ready(fixture.session));
            check_equal(turbo_gdtls_deadline(fixture.session), uint64_t{0});
            fixture.owned_packet = turbo_gdtls_take_datagram(fixture.session);
            check_not_null(fixture.owned_packet);
            check_true(fixture.transfer_to_reference(fixture.owned_packet));
            check_equal(SSL_shutdown(fixture.reference), 1);
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_CLOSED);
            check_equal(turbo_gdtls_write(fixture.session, "x", 1), TURBO_GDTLS_CLOSED);
        }
        it("role=%d preserves authenticated trailing data coalesced with close_notify", server) {
            check_true(fixture.init(server != 0, 1));
            check_true(fixture.handshake());
            check_true(fixture.capture_reference_record());
            check_equal(SSL_shutdown(fixture.reference), 0);
            uint8_t closing[512];
            int count = BIO_read(SSL_get_wbio(fixture.reference), closing, sizeof(closing));
            check_greater(count, 0);
            size_t data_size = tstr_len(fixture.owned_packet);
            fixture.held_packet = tstr_new_len(nullptr, data_size + static_cast<size_t>(count));
            check_not_null(fixture.held_packet);
            memcpy(fixture.held_packet, fixture.owned_packet, data_size);
            memcpy(fixture.held_packet + data_size, closing, static_cast<size_t>(count));
            check_equal(turbo_gdtls_receive(fixture.session, fixture.held_packet,
                tstr_len(fixture.held_packet), clock_ms), TURBO_GDTLS_CLOSED);
            check_false(turbo_gdtls_ready(fixture.session));
            tstr_free(fixture.owned_packet);
            fixture.owned_packet = turbo_gdtls_take_plaintext(fixture.session);
            check_not_null(fixture.owned_packet);
            check_equal(fixture.owned_packet, "authenticated datagram");
            check_null(turbo_gdtls_take_plaintext(fixture.session));
            check_equal(turbo_gdtls_poll(fixture.session, clock_ms), TURBO_GDTLS_CLOSED);
        }
    }
    it("accepts Finished when UDP delivers authenticated application data ahead of it") {
        check_true(fixture.init(false, 1));
        fixture.reorder_reference = true;
        fixture.early_application = true;
        check_interop(1);
        check_true(fixture.early_sent);
    }
}

suite("GmSSL DTLS-SRTP paired owner contracts") {
    static turbo_gdtls_context *contexts[2];
    static turbo_gdtls *sessions[2];
    static tstr certificate, key, packet, repeated_finished;
    before_each() {
        clock_ms = 1000000;
        for (auto &context : contexts) {
            check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
            context = turbo_gdtls_context_create(certificate, tstr_len(certificate), key, tstr_len(key));
            cmeta_crypto_clear(key, tstr_len(key));
            tstr_free(key); key = nullptr;
            tstr_free(certificate); certificate = nullptr;
            check_not_null(context);
        }
    }
    after_each() {
        for (auto &session : sessions) { turbo_gdtls_destroy(session); session = nullptr; }
        for (auto &context : contexts) { turbo_gdtls_context_destroy(context); context = nullptr; }
        if (key) cmeta_crypto_clear(key, tstr_len(key));
        tstr_free(key); key = nullptr;
        tstr_free(certificate); certificate = nullptr;
        tstr_free(packet); packet = nullptr;
        tstr_free(repeated_finished); repeated_finished = nullptr;
    }
    it("recovers the first final-flight retry despite transit delay and limits later replays") {
        constexpr uint16_t profile = 1;
        for (int i = 0; i < 2; ++i) {
            uint8_t pin[32];
            check_equal(turbo_gdtls_context_fingerprint(contexts[1 - i], pin), 0);
            sessions[i] = turbo_gdtls_create(contexts[i], i, 300, pin, &profile, 1);
            check_not_null(sessions[i]);
            check_equal(turbo_gdtls_start(sessions[i], clock_ms), 0);
        }
        uint64_t client_retry = 0, server_ready_at = 0;
        for (unsigned step = 0; step < 20 && !turbo_gdtls_ready(sessions[1]); ++step) {
            clock_ms += 20;
            while ((packet = turbo_gdtls_take_datagram(sessions[0])) != nullptr) {
                if (static_cast<uint8_t>(packet[0]) == 22 && packet[4] == 1) {
                    client_retry = turbo_gdtls_deadline(sessions[0]);
                    clock_ms += 50; // Client's RTO is already running in transit.
                }
                check_equal(turbo_gdtls_receive(sessions[1], packet, tstr_len(packet), clock_ms), 0);
                tstr_free(packet); packet = nullptr;
            }
            if (turbo_gdtls_ready(sessions[1])) server_ready_at = clock_ms;
            while ((packet = turbo_gdtls_take_datagram(sessions[1])) != nullptr) {
                // Drop the entire final server flight, including CCS.
                if (!server_ready_at)
                    check_equal(turbo_gdtls_receive(sessions[0], packet, tstr_len(packet), clock_ms), 0);
                tstr_free(packet); packet = nullptr;
            }
        }
        check_true(turbo_gdtls_ready(sessions[1]));
        check_false(turbo_gdtls_ready(sessions[0]));
        check_greater(client_retry, server_ready_at);
        check_less(client_retry - server_ready_at, uint64_t{1000});
        clock_ms = client_retry;
        check_equal(turbo_gdtls_poll(sessions[0], clock_ms), 0);
        while ((packet = turbo_gdtls_take_datagram(sessions[0])) != nullptr) {
            if (static_cast<uint8_t>(packet[0]) == 22 && packet[4] == 1) {
                repeated_finished = tstr_new_len(packet, tstr_len(packet));
                check_not_null(repeated_finished);
            }
            check_equal(turbo_gdtls_receive(sessions[1], packet, tstr_len(packet), clock_ms), 0);
            tstr_free(packet); packet = nullptr;
        }
        check_not_null(repeated_finished);
        unsigned replies = 0;
        while ((packet = turbo_gdtls_take_datagram(sessions[1])) != nullptr) {
            ++replies;
            check_equal(turbo_gdtls_receive(sessions[0], packet, tstr_len(packet), clock_ms), 0);
            tstr_free(packet); packet = nullptr;
        }
        check_greater(replies, 0u);
        check_true(turbo_gdtls_ready(sessions[0]));
        for (unsigned i = 0; i < 10; ++i)
            check_equal(turbo_gdtls_receive(sessions[1], repeated_finished,
                tstr_len(repeated_finished), clock_ms), 0);
        check_null(turbo_gdtls_take_datagram(sessions[1]));
        clock_ms += 999;
        check_equal(turbo_gdtls_receive(sessions[1], repeated_finished,
            tstr_len(repeated_finished), clock_ms), 0);
        check_null(turbo_gdtls_take_datagram(sessions[1]));
        ++clock_ms;
        check_equal(turbo_gdtls_receive(sessions[1], repeated_finished,
            tstr_len(repeated_finished), clock_ms), 0);
        packet = turbo_gdtls_take_datagram(sessions[1]);
        check_not_null(packet);
    }
    it("negotiates server preference, exports matching keys and retires the retained final flight") {
        constexpr uint16_t profiles[2][4] = {{1, 2, 7, 8}, {8, 7, 2, 1}};
        for (int i = 0; i < 2; ++i) {
            uint8_t pin[32];
            check_equal(turbo_gdtls_context_fingerprint(contexts[1 - i], pin), 0);
            sessions[i] = turbo_gdtls_create(contexts[i], i, 300, pin, profiles[i], 4);
            check_not_null(sessions[i]);
            check_equal(turbo_gdtls_start(sessions[i], clock_ms), 0);
        }
        for (unsigned step = 0; step < 2000; ++step) {
            clock_ms += 20;
            for (int sender = 0; sender < 2; ++sender) {
                check_equal(turbo_gdtls_poll(sessions[sender], clock_ms), 0);
                while ((packet = turbo_gdtls_take_datagram(sessions[sender])) != nullptr) {
                    check_equal(turbo_gdtls_receive(sessions[1 - sender], packet, tstr_len(packet), clock_ms), 0);
                    tstr_free(packet); packet = nullptr;
                }
            }
            if (turbo_gdtls_ready(sessions[0]) && turbo_gdtls_ready(sessions[1])) break;
        }
        uint8_t keys[2][88]; size_t sizes[2];
        for (int i = 0; i < 2; ++i) {
            check_true(turbo_gdtls_ready(sessions[i]));
            check_equal(turbo_gdtls_srtp_profile(sessions[i]), uint16_t{8});
            check_equal(turbo_gdtls_export_srtp(sessions[i], keys[i], sizeof(keys[i]), &sizes[i]), 0);
            check_equal(sizes[i], sizeof(keys[i]));
        }
        check_equal(keys[0], keys[1], sizeof(keys[0]));
        check_equal(turbo_gdtls_deadline(sessions[0]), uint64_t{0});
        uint64_t retires = turbo_gdtls_deadline(sessions[1]);
        check_greater(retires, clock_ms);
        check_equal(turbo_gdtls_poll(sessions[1], retires), 0);
        check_equal(turbo_gdtls_deadline(sessions[1]), uint64_t{0});
        check_null(turbo_gdtls_take_datagram(sessions[1]));
        check_true(turbo_gdtls_ready(sessions[1]));
        check_equal(turbo_gdtls_export_srtp(sessions[1], keys[1], sizeof(keys[1]), &sizes[1]), 0);
        check_equal(keys[0], keys[1], sizeof(keys[0]));
    }
}
