#include "dc_identity.h"
#include "dtls_test_identity.h"
#include <cmeta_crypto.h>
#include <gmssl/pem.h>
#include <gmssl/x509.h>
#include <tinytest.hpp>
#include <ctime>
#include <memory>

namespace {
bool write_pem_file(const char *path, const char *label, const void *data, size_t size) {
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path, "wb"), &fclose);
    if (!file || pem_write(file.get(), label, static_cast<const uint8_t *>(data), size) != 1 ||
        fflush(file.get()) || ferror(file.get())) return false;
    return fclose(file.release()) == 0;
}
void check_fingerprint(turbo_gdtls_context *identity, tstr text, tstr certificate) {
    uint8_t expected[32], actual[32];
    check_equal(cmeta_sha256(certificate, tstr_len(certificate), expected), SALTS_OK);
    check_equal(turbo_gdtls_context_fingerprint(identity, actual), 0);
    check_equal(actual, expected, sizeof(expected));
    check_not_null(text);
    check_equal(tstr_len(text), size_t{95});
    constexpr char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < sizeof(actual); ++i) {
        check_equal(text[i * 3], hex[actual[i] >> 4]);
        check_equal(text[i * 3 + 1], hex[actual[i] & 15]);
        if (i < 31) check_equal(text[i * 3 + 2], ':');
    }
}
}

suite("GmSSL DataChannel identities") {
    static turbo_gdtls_context *identity;
    static tstr certificate, key, other_certificate, other_key, fingerprint, oversized;
    static char *certificate_path, *key_path;
    static X509_KEY public_key;
    before_each() {
        identity = nullptr;
        certificate = key = other_certificate = other_key = fingerprint = oversized = nullptr;
        public_key = {};
        certificate_path = tt_make_temp_file("dtls-cert-", ".pem");
        key_path = tt_make_temp_file("dtls-key-", ".pem");
        check_not_null(certificate_path);
        check_not_null(key_path);
    }
    after_each() {
        turbo_gdtls_context_destroy(identity);
        if (key) cmeta_crypto_clear(key, tstr_len(key));
        if (other_key) cmeta_crypto_clear(other_key, tstr_len(other_key));
        tstr_free(certificate); tstr_free(key);
        tstr_free(other_certificate); tstr_free(other_key);
        tstr_free(fingerprint); tstr_free(oversized);
        x509_key_cleanup(&public_key);
        if (certificate_path) { tt_remove_file(certificate_path); free(certificate_path); }
        if (key_path) { tt_remove_file(key_path); free(key_path); }
    }

    it("generates a self-signed P-256 identity with the expected name, serial and validity") {
        const time_t before = std::time(nullptr);
        check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
        const time_t after = std::time(nullptr);
        const auto *der = reinterpret_cast<const uint8_t *>(certificate);
        size_t size = tstr_len(certificate);
        check_equal(x509_cert_verify_by_ca_cert(der, size, der, size, nullptr, 0), 1);
        int version, inner_algorithm, algorithm;
        const uint8_t *serial, *issuer, *subject;
        size_t serial_size, issuer_size, subject_size;
        time_t not_before, not_after;
        check_equal(x509_cert_get_details(der, size, &version, &serial, &serial_size,
            &inner_algorithm, &issuer, &issuer_size, &not_before, &not_after,
            &subject, &subject_size, &public_key, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, &algorithm, nullptr, nullptr), 1);
        check_equal(version, X509_version_v3);
        check_equal(inner_algorithm, OID_ecdsa_with_sha256);
        check_equal(algorithm, OID_ecdsa_with_sha256);
        check_equal(public_key.algor, OID_ec_public_key);
        check_equal(public_key.algor_param, OID_secp256r1);
        check_equal(issuer_size, subject_size);
        check_equal(issuer, subject, subject_size);
        check_greater(serial_size, size_t{0});
        check_less_equal(serial_size, size_t{16});
        bool nonzero = false;
        for (size_t i = 0; i < serial_size; ++i) nonzero |= serial[i] != 0;
        check_true(nonzero);
        check_greater_equal(not_before, before);
        check_less_equal(not_before, after);
        check_equal(not_after - not_before, time_t{365L * 24 * 60 * 60});
        int tag;
        const uint8_t *name; size_t name_size;
        check_equal(x509_name_get_common_name(subject, subject_size, &tag, &name, &name_size), 1);
        constexpr char expected[] = "TurboNet DataChannel";
        check_equal(name_size, sizeof(expected) - 1);
        check_equal(name, expected, name_size);
        identity = turbo_gdtls_context_create(certificate, tstr_len(certificate), key, tstr_len(key));
        check_not_null(identity);
    }

    it("creates an ephemeral identity only when both paths are absent") {
        check_equal(turbo_dc_identity_create(nullptr, nullptr, &identity, &fingerprint), 0);
        check_not_null(identity);
        check_equal(tstr_len(fingerprint), size_t{95});
    }

    for (int rsa = 0; rsa < 2; ++rsa) {
        it("loads native PKCS#8 PEM, RSA=%d, and advertises the exact leaf fingerprint", rsa) {
            check_equal(dtls_test_identity(rsa != 0, &certificate, &key), 0);
            check_true(write_pem_file(certificate_path, "CERTIFICATE", certificate, tstr_len(certificate)));
            check_true(write_pem_file(key_path, "PRIVATE KEY", key, tstr_len(key)));
            check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), 0);
            check_not_null(identity);
            check_fingerprint(identity, fingerprint, certificate);
        }
    }

    it("rejects invalid owners, partial paths and empty or missing files without publishing outputs") {
        check_equal(turbo_dc_generate_certificate_der(nullptr, &key), -1);
        check_null(key);
        check_equal(turbo_dc_generate_certificate_der(&certificate, nullptr), -1);
        check_null(certificate);
        check_equal(turbo_dc_generate_certificate_der(&key, &key), -1);
        check_null(key);
        check_equal(turbo_dc_identity_create(nullptr, nullptr, nullptr, &fingerprint), -1);
        check_null(fingerprint);
        check_equal(turbo_dc_identity_create(nullptr, nullptr, &identity, nullptr), -1);
        check_null(identity);
        check_equal(turbo_dc_identity_create(certificate_path, nullptr, &identity, &fingerprint), -1);
        check_equal(turbo_dc_identity_create(nullptr, key_path, &identity, &fingerprint), -1);
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
        tt_remove_file(certificate_path);
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
        check_null(identity); check_null(fingerprint);
    }

    it("rejects mismatched keys and malformed DER without generating a replacement identity") {
        check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
        check_equal(turbo_dc_generate_certificate_der(&other_certificate, &other_key), 0);
        check_true(write_pem_file(certificate_path, "CERTIFICATE", certificate, tstr_len(certificate)));
        check_true(write_pem_file(key_path, "PRIVATE KEY", other_key, tstr_len(other_key)));
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
        check_null(identity); check_null(fingerprint);
        key[0] = 0;
        check_true(write_pem_file(key_path, "PRIVATE KEY", key, tstr_len(key)));
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
        check_null(identity); check_null(fingerprint);
    }

    for (const char *label : {"EC PRIVATE KEY", "RSA PRIVATE KEY", "ENCRYPTED PRIVATE KEY"}) {
        it("rejects unsupported %s encoding without a compatibility parser", label) {
            check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
            check_true(write_pem_file(certificate_path, "CERTIFICATE", certificate, tstr_len(certificate)));
            check_true(write_pem_file(key_path, label, key, tstr_len(key)));
            check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
            check_null(identity); check_null(fingerprint);
        }
    }

    it("rejects truncated PEM after partially decoding private bytes") {
        check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
        check_true(write_pem_file(certificate_path, "CERTIFICATE", certificate, tstr_len(certificate)));
        constexpr char partial[] = "-----BEGIN PRIVATE KEY-----\nMAECAwQFBgcICQoLDA0ODw==\n";
        check_equal(tt_write_file(key_path, partial, sizeof(partial) - 1), 0);
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
        check_null(identity); check_null(fingerprint);
    }

    for (int oversized_key = 0; oversized_key < 2; ++oversized_key) {
        it("rejects a decoded object exceeding 16 KiB, private=%d", oversized_key) {
            check_equal(turbo_dc_generate_certificate_der(&certificate, &key), 0);
            oversized = tstr_new_len(nullptr, 16 * 1024 + 1);
            check_not_null(oversized);
            memset(oversized, 0xa5, tstr_len(oversized));
            tstr cert_data = oversized_key ? certificate : oversized;
            tstr key_data = oversized_key ? oversized : key;
            check_true(write_pem_file(certificate_path, "CERTIFICATE", cert_data, tstr_len(cert_data)));
            check_true(write_pem_file(key_path, "PRIVATE KEY", key_data, tstr_len(key_data)));
            check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &fingerprint), -1);
            check_null(identity); check_null(fingerprint);
        }
    }
}
