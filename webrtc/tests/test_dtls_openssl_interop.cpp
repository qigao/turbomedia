/*
 * #171: independent OpenSSL 3.x DTLS 1.2 peer for the private GmSSL engine.
 * Only compiled on Linux with TURBO_MEDIA_TEST_INDEPENDENT_DTLS=ON.
 * The production engine never links or dispatches to OpenSSL.
 */
#include "gmssl_dtls.h"
#include "dtls_test_identity.h"
#include <tinytest.hpp>
#include <cmeta_crypto.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <climits>

namespace {
constexpr uint64_t START_MS = 1000000;
constexpr size_t MTU = 1200;
constexpr char EXPORT_LABEL[] = "EXTRACTOR-dtls_srtp";

int trust_test_certificate(int, X509_STORE_CTX *) {
    /* The independent peer uses a deliberately self-signed test identity.
     * Pinning is checked explicitly after Finished and before key export. */
    return 1;
}

const char *srtp_name(uint16_t id) {
    switch (id) {
    case 1: return "SRTP_AES128_CM_SHA1_80";
    case 2: return "SRTP_AES128_CM_SHA1_32";
    case 7: return "SRTP_AEAD_AES_128_GCM";
    case 8: return "SRTP_AEAD_AES_256_GCM";
    default: return nullptr;
    }
}

bool create_udp_pair(int fd[2]) {
    sockaddr_in address[2] = {};
    for (int i = 0; i < 2; ++i) {
        fd[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd[i] < 0) return false;
        address[i].sin_family = AF_INET;
        address[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address[i].sin_port = 0;
        if (bind(fd[i], reinterpret_cast<sockaddr *>(&address[i]),
                 sizeof(address[i])) != 0) return false;
        socklen_t length = sizeof(address[i]);
        if (getsockname(fd[i], reinterpret_cast<sockaddr *>(&address[i]),
                        &length) != 0) return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (connect(fd[i], reinterpret_cast<sockaddr *>(&address[1-i]),
                    sizeof(address[1-i])) != 0) return false;
        int flags = fcntl(fd[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fd[i], F_SETFL, flags | O_NONBLOCK) != 0)
            return false;
    }
    return true;
}

struct IndependentPair {
    bool gm_server;
    uint16_t profile;
    bool rsa_identity;
    tstr gm_certificate = nullptr, gm_private_key = nullptr;
    tstr external_certificate = nullptr, external_private_key = nullptr;
    turbo_gdtls_context *gm_context = nullptr;
    turbo_gdtls *gm = nullptr;
    SSL_CTX *openssl_context = nullptr;
    SSL *ssl = nullptr;
    int socket_fd[2] = {-1, -1};
    uint64_t clock_ms = START_MS;
    const char *stage = "initialize";

    IndependentPair(bool server, uint16_t id, bool rsa)
        : gm_server(server), profile(id), rsa_identity(rsa) {}

    ~IndependentPair() {
        SSL_free(ssl);
        SSL_CTX_free(openssl_context);
        turbo_gdtls_destroy(gm);
        turbo_gdtls_context_destroy(gm_context);
        if (gm_private_key) cmeta_crypto_clear(gm_private_key, tstr_len(gm_private_key));
        if (external_private_key)
            cmeta_crypto_clear(external_private_key, tstr_len(external_private_key));
        tstr_free(gm_certificate);
        tstr_free(gm_private_key);
        tstr_free(external_certificate);
        tstr_free(external_private_key);
        for (int &fd : socket_fd) if (fd >= 0) close(fd);
    }

    bool failure(const char *reason) const {
        unsigned long detail = ERR_peek_last_error();
        fprintf(stderr,
                "[independent-dtls] provider=OpenSSL-%s role=%s profile=%u rsa=%d stage=%s reason=%s openssl=%s\n",
                OpenSSL_version(OPENSSL_VERSION), gm_server ? "GmSSL-server" : "GmSSL-client",
                static_cast<unsigned>(profile), rsa_identity ? 1 : 0, stage, reason,
                detail ? ERR_reason_error_string(detail) : "none");
        return false;
    }

    bool init() {
        if (OPENSSL_version_major() != 3 || !srtp_name(profile))
            return failure("unsupported-reference-version-or-profile");
        if (dtls_test_identity(rsa_identity, &gm_certificate, &gm_private_key) ||
            dtls_test_identity(rsa_identity, &external_certificate, &external_private_key))
            return failure("test-identity-generation");
        gm_context = turbo_gdtls_context_create(
            gm_certificate, tstr_len(gm_certificate),
            gm_private_key, tstr_len(gm_private_key));
        if (!gm_context) return failure("GmSSL-context");
        const unsigned char *p =
            reinterpret_cast<const unsigned char *>(external_certificate);
        X509 *cert = d2i_X509(nullptr, &p, static_cast<long>(tstr_len(external_certificate)));
        p = reinterpret_cast<const unsigned char *>(external_private_key);
        EVP_PKEY *key = d2i_AutoPrivateKey(nullptr, &p,
                                         static_cast<long>(tstr_len(external_private_key)));
        if (!cert || !key) {
            X509_free(cert); EVP_PKEY_free(key);
            return failure("OpenSSL-import-DER");
        }
        uint8_t expected[32], gm_pin[32];
        unsigned int pin_size = 0;
        bool pin_ok = X509_digest(cert, EVP_sha256(), expected, &pin_size) == 1 &&
                      pin_size == 32 &&
                      turbo_gdtls_context_fingerprint(gm_context, gm_pin) == 0;
        openssl_context = SSL_CTX_new(DTLS_method());
        bool configured = pin_ok && openssl_context &&
            SSL_CTX_set_min_proto_version(openssl_context, DTLS1_2_VERSION) == 1 &&
            SSL_CTX_set_max_proto_version(openssl_context, DTLS1_2_VERSION) == 1 &&
            SSL_CTX_set_cipher_list(openssl_context,
                "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384") == 1 &&
            SSL_CTX_set1_groups_list(openssl_context, "P-256") == 1 &&
            SSL_CTX_set_tlsext_use_srtp(openssl_context, srtp_name(profile)) == 0 &&
            SSL_CTX_use_certificate(openssl_context, cert) == 1 &&
            SSL_CTX_use_PrivateKey(openssl_context, key) == 1 &&
            SSL_CTX_check_private_key(openssl_context) == 1;
        X509_free(cert); EVP_PKEY_free(key);
        if (!configured) return failure("OpenSSL-context-policy");
        SSL_CTX_set_read_ahead(openssl_context, 1);
        SSL_CTX_set_options(openssl_context, SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_verify(openssl_context, SSL_VERIFY_PEER |
            (gm_server ? 0 : SSL_VERIFY_FAIL_IF_NO_PEER_CERT), trust_test_certificate);
        gm = turbo_gdtls_create(gm_context, gm_server ? 1 : 0, MTU,
                                expected, &profile, 1, 0);
        if (!gm || turbo_gdtls_start(gm, clock_ms) != 0)
            return failure("GmSSL-start");
        if (!create_udp_pair(socket_fd)) return failure("UDP-loopback");
        ssl = SSL_new(openssl_context);
        BIO *bio = socket_fd[1] >= 0 ? BIO_new_dgram(socket_fd[1], BIO_NOCLOSE) : nullptr;
        if (!ssl || !bio) {
            BIO_free(bio); return failure("OpenSSL-datagram-BIO");
        }
        SSL_set_bio(ssl, bio, bio);
        if (SSL_set_mtu(ssl, MTU) <= 0) return failure("OpenSSL-MTU");
        if (gm_server) SSL_set_connect_state(ssl);
        else SSL_set_accept_state(ssl);
        return true;
    }

    bool pump_gm_output() {
        for (tstr packet; (packet = turbo_gdtls_take_datagram(gm)) != nullptr;) {
            ssize_t sent = send(socket_fd[0], packet, tstr_len(packet), 0);
            bool ok = sent >= 0 && static_cast<size_t>(sent) == tstr_len(packet);
            tstr_free(packet);
            if (!ok) return failure("UDP-send");
        }
        return true;
    }

    bool receive_gm() {
        uint8_t packet[65535];
        for (;;) {
            ssize_t n = recv(socket_fd[0], packet, sizeof(packet), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                return failure("UDP-receive");
            }
            if (n == 0) continue;
            int status = turbo_gdtls_receive(gm, packet, static_cast<size_t>(n), clock_ms);
            if (status != TURBO_GDTLS_OK) return failure("GmSSL-receive");
        }
    }

    bool handshake() {
        stage = "handshake";
        uint8_t not_exported[88] = {};
        size_t n = 12;
        if (turbo_gdtls_export_srtp(gm, not_exported, sizeof(not_exported), &n) !=
                TURBO_GDTLS_AUTH || n != 0)
            return failure("pre-Finished-key-exposure");
        bool openssl_ready = false;
        for (unsigned i = 0; i < 3500; ++i) {
            clock_ms += 10;
            if (!pump_gm_output()) return false;
            if (!openssl_ready) {
                int result = SSL_do_handshake(ssl);
                if (result == 1) openssl_ready = true;
                else {
                    int error = SSL_get_error(ssl, result);
                    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                        return failure("OpenSSL-handshake");
                }
            }
            if (!receive_gm()) return false;
            int result = turbo_gdtls_poll(gm, clock_ms);
            if (result != TURBO_GDTLS_OK) return failure("GmSSL-timer");
            if (openssl_ready && turbo_gdtls_ready(gm)) {
                if (!pump_gm_output()) return false;
                return true;
            }
        }
        return failure("bounded-handshake-timeout");
    }

    bool qualify_exporter() {
        stage = "authenticated-exporter";
        if (!SSL_is_init_finished(ssl) || !turbo_gdtls_ready(gm) ||
            SSL_version(ssl) != DTLS1_2_VERSION || SSL_get_extms_support(ssl) != 1)
            return failure("DTLS12-or-EMS-not-negotiated");
        SRTP_PROTECTION_PROFILE *selected = SSL_get_selected_srtp_profile(ssl);
        if (!selected || selected->id != profile || turbo_gdtls_srtp_profile(gm) != profile)
            return failure("SRTP-profile-disagreement");

        X509 *peer = SSL_get1_peer_certificate(ssl);
        uint8_t actual_pin[32], gm_pin[32], exported[88], other[88];
        unsigned int pin_size = 0;
        bool authenticated = peer &&
            X509_digest(peer, EVP_sha256(), actual_pin, &pin_size) == 1 &&
            pin_size == 32 &&
            turbo_gdtls_context_fingerprint(gm_context, gm_pin) == 0 &&
            CRYPTO_memcmp(actual_pin, gm_pin, 32) == 0;
        X509_free(peer);
        if (!authenticated) return failure("OpenSSL-peer-fingerprint-pin");

        size_t size = 0;
        const size_t expected_size = profile == 8 ? 88 : profile == 7 ? 56 : 60;
        bool equal = turbo_gdtls_export_srtp(gm, exported, sizeof(exported), &size) == 0 &&
            size == expected_size &&
            SSL_export_keying_material(ssl, other, expected_size, EXPORT_LABEL,
                sizeof(EXPORT_LABEL) - 1, nullptr, 0, 0) == 1 &&
            CRYPTO_memcmp(exported, other, expected_size) == 0;
        OPENSSL_cleanse(exported, sizeof(exported));
        OPENSSL_cleanse(other, sizeof(other));
        if (!equal) return failure("RFC5764-exporter-mismatch");
        return true;
    }

    bool exchange_application() {
        stage = "application-datagrams";
        static constexpr char from_openssl[] = "from OpenSSL";
        if (SSL_write(ssl, from_openssl, sizeof(from_openssl)) !=
                static_cast<int>(sizeof(from_openssl))) return failure("OpenSSL-write");
        if (!receive_gm()) return false;
        tstr received = turbo_gdtls_take_plaintext(gm);
        bool correct = received && tstr_len(received) == sizeof(from_openssl) &&
            memcmp(received, from_openssl, sizeof(from_openssl)) == 0;
        if (received) {
            cmeta_crypto_clear(received, tstr_len(received));
            tstr_free(received);
        }
        if (!correct) return failure("OpenSSL-to-GmSSL-payload");
        static constexpr char from_gm[] = "from GmSSL";
        if (turbo_gdtls_write(gm, from_gm, sizeof(from_gm)) != TURBO_GDTLS_OK ||
            !pump_gm_output()) return failure("GmSSL-write");
        char buffer[64];
        int bytes = SSL_read(ssl, buffer, sizeof(buffer));
        if (bytes != static_cast<int>(sizeof(from_gm)) ||
            memcmp(buffer, from_gm, sizeof(from_gm)) != 0)
            return failure("GmSSL-to-OpenSSL-payload");
        return true;
    }
};

} // namespace

suite("OpenSSL 3.x independent DTLS 1.2 interop (#171)") {
    for (int server = 0; server < 2; ++server) {
        for (uint16_t profile : {uint16_t{1}, uint16_t{2}, uint16_t{7}, uint16_t{8}}) {
            it("GmSSL server=%d profile=%u negotiates OpenSSL DTLS and identical SRTP exporter",
               server, static_cast<unsigned>(profile)) {
                IndependentPair fixture(server != 0, profile, false);
                check_true(fixture.init());
                check_true(fixture.handshake());
                check_true(fixture.qualify_exporter());
                check_true(fixture.exchange_application());
            }
        }
    }
}
