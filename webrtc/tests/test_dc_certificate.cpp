#include "dc_certificate.h"
#include "dc_certificate_der.h"
#include "dc_identity.h"
#include <cmeta_crypto.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <tinytest.hpp>

#include <openssl/bn.h>
#include <openssl/ec_key.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <ctime>

suite("DataChannel ephemeral identity") {
    static bssl::UniquePtr<SSL_CTX> context;
    static bssl::UniquePtr<EVP_PKEY> public_key;
    static bssl::UniquePtr<BIGNUM> serial;
    static tstr fingerprint;

    before_each() {
        fingerprint = nullptr;
        context.reset(SSL_CTX_new(DTLS_method()));
    }
    after_each() {
        tstr_free(fingerprint);
        fingerprint = nullptr;
        serial.reset();
        public_key.reset();
        context.reset();
    }

    it("imports a self-signed P-256 identity and advertises its exact SHA-256 digest") {
        check_not_null(context.get());
        const time_t before = std::time(nullptr);
        check_equal(turbo_dc_generate_identity(context.get(), &fingerprint), 0);
        const time_t after = std::time(nullptr);
        X509 *certificate = SSL_CTX_get0_certificate(context.get());
        check_not_null(certificate);
        check_equal(SSL_CTX_check_private_key(context.get()), 1);
        public_key.reset(X509_get_pubkey(certificate));
        check_not_null(public_key.get());
        check_equal(EVP_PKEY_id(public_key.get()), EVP_PKEY_EC);
        const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(public_key.get());
        check_not_null(ec);
        check_equal(EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)), NID_X9_62_prime256v1);
        check_equal(X509_get_signature_nid(certificate), NID_ecdsa_with_SHA256);
        check_equal(X509_verify(certificate, public_key.get()), 1);
        check_equal(X509_get_version(certificate), 2L);
        check_equal(X509_NAME_cmp(X509_get_subject_name(certificate),
                                 X509_get_issuer_name(certificate)), 0);
        char common_name[64] = {};
        check_equal(X509_NAME_get_text_by_NID(X509_get_subject_name(certificate),
            NID_commonName, common_name, sizeof(common_name)),
            static_cast<int>(sizeof("TurboNet DataChannel") - 1));
        check_equal(common_name, "TurboNet DataChannel");

        serial.reset(ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate), nullptr));
        check_not_null(serial.get());
        check_false(BN_is_negative(serial.get()));
        check_false(BN_is_zero(serial.get()));
        check_less_equal(BN_num_bits(serial.get()), 128u);
        int days = 0, seconds = 0;
        check_equal(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(certificate),
                                  X509_get0_notAfter(certificate)), 1);
        check_equal(days, 365);
        check_equal(seconds, 0);
        time_t not_before = 0;
        check_equal(ASN1_TIME_to_time_t(X509_get0_notBefore(certificate), &not_before), 1);
        check_greater_equal(not_before, before);
        check_less_equal(not_before, after);

        // Compute through the importing provider, independently of Salts Core.
        unsigned char digest[32];
        unsigned int digest_size = 0;
        check_equal(X509_digest(certificate, EVP_sha256(), digest, &digest_size), 1);
        check_equal(digest_size, 32u);
        check_not_null(fingerprint);
        check_equal(tstr_len(fingerprint), size_t{95});
        constexpr char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < sizeof(digest); ++i) {
            check_equal(fingerprint[i * 3], hex[digest[i] >> 4]);
            check_equal(fingerprint[i * 3 + 1], hex[digest[i] & 15]);
            if (i + 1 < sizeof(digest)) check_equal(fingerprint[i * 3 + 2], ':');
        }
    }

    it("rejects invalid owners without publishing an identity") {
        check_not_null(context.get());
        check_equal(turbo_dc_generate_identity(nullptr, &fingerprint), -1);
        check_null(fingerprint);
        check_equal(turbo_dc_generate_identity(context.get(), nullptr), -1);
        check_null(SSL_CTX_get0_certificate(context.get()));
        tstr output = nullptr;
        check_equal(turbo_dc_generate_certificate_der(nullptr, &output), -1);
        check_null(output);
        check_equal(turbo_dc_generate_certificate_der(&output, nullptr), -1);
        check_null(output);
        check_equal(turbo_dc_generate_certificate_der(&output, &output), -1);
        check_null(output);
    }
}


suite("GmSSL production identity admission") {
    static bssl::UniquePtr<SSL_CTX> source;
    static bssl::UniquePtr<BIO> certificate_pem, private_pem;
    static turbo_gdtls_context *identity;
    static tstr expected, actual;
    static char *certificate_path, *key_path;

    before_each() {
        identity = nullptr;
        expected = actual = nullptr;
        certificate_path = key_path = nullptr;
        source.reset(SSL_CTX_new(DTLS_method()));
        check_not_null(source.get());
        check_equal(turbo_dc_generate_identity(source.get(), &expected), 0);
        certificate_pem.reset(BIO_new(BIO_s_mem()));
        private_pem.reset(BIO_new(BIO_s_mem()));
        certificate_path = tt_make_temp_file("dtls-cert-", ".pem");
        key_path = tt_make_temp_file("dtls-key-", ".pem");
        check_not_null(certificate_path);
        check_not_null(key_path);
    }
    after_each() {
        turbo_gdtls_context_destroy(identity);
        tstr_free(expected);
        tstr_free(actual);
        if (certificate_path) { tt_remove_file(certificate_path); free(certificate_path); }
        if (key_path) { tt_remove_file(key_path); free(key_path); }
        if (private_pem) {
            char *bytes = nullptr;
            long size = BIO_get_mem_data(private_pem.get(), &bytes);
            if (size > 0) cmeta_crypto_clear(bytes, static_cast<size_t>(size));
        }
        private_pem.reset();
        certificate_pem.reset();
        source.reset();
    }

    for (int format = 0; format < 3; ++format) {
        it("normalizes PEM identity format %d and publishes its exact fingerprint", format) {
            if (format == 2) {
                bssl::UniquePtr<RSA> rsa(RSA_new());
                bssl::UniquePtr<BIGNUM> exponent(BN_new());
                bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
                bssl::UniquePtr<X509> cert(X509_dup(SSL_CTX_get0_certificate(source.get())));
                check_equal(BN_set_word(exponent.get(), RSA_F4), 1);
                check_equal(RSA_generate_key_ex(rsa.get(), 2048, exponent.get(), nullptr), 1);
                check_equal(EVP_PKEY_set1_RSA(key.get(), rsa.get()), 1);
                check_equal(X509_set_pubkey(cert.get(), key.get()), 1);
                check_greater(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
                check_equal(SSL_CTX_use_certificate(source.get(), cert.get()), 1);
                check_equal(SSL_CTX_use_PrivateKey(source.get(), key.get()), 1);
            }
            X509 *cert = SSL_CTX_get0_certificate(source.get());
            EVP_PKEY *key = SSL_CTX_get0_privatekey(source.get());
            check_equal(PEM_write_bio_X509(certificate_pem.get(), cert), 1);
            if (format == 0) {
                check_equal(PEM_write_bio_PKCS8PrivateKey(private_pem.get(), key,
                    nullptr, nullptr, 0, nullptr, nullptr), 1);
            } else if (format == 1) {
                check_equal(PEM_write_bio_ECPrivateKey(private_pem.get(), EVP_PKEY_get0_EC_KEY(key),
                    nullptr, nullptr, 0, nullptr, nullptr), 1);
            } else {
                check_equal(PEM_write_bio_RSAPrivateKey(private_pem.get(), EVP_PKEY_get0_RSA(key),
                    nullptr, nullptr, 0, nullptr, nullptr), 1);
            }
            char *bytes = nullptr;
            long size = BIO_get_mem_data(certificate_pem.get(), &bytes);
            check_greater(size, 0L);
            check_equal(tt_write_file(certificate_path, bytes, static_cast<size_t>(size)), 0);
            size = BIO_get_mem_data(private_pem.get(), &bytes);
            check_greater(size, 0L);
            check_equal(tt_write_file(key_path, bytes, static_cast<size_t>(size)), 0);
            check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &actual), 0);
            check_not_null(identity);
            uint8_t digest[32], independent[32];
            unsigned length = 0;
            check_equal(X509_digest(cert, EVP_sha256(), independent, &length), 1);
            check_equal(length, 32u);
            check_equal(turbo_gdtls_context_fingerprint(identity, digest), 0);
            check_equal(memcmp(digest, independent, sizeof(digest)), 0);
            check_equal(tstr_len(actual), size_t{95});
            constexpr char hex[] = "0123456789ABCDEF";
            for (size_t i = 0; i < sizeof(digest); ++i) {
                check_equal(actual[i * 3], hex[digest[i] >> 4]);
                check_equal(actual[i * 3 + 1], hex[digest[i] & 15]);
                if (i < 31) check_equal(actual[i * 3 + 2], ':');
            }
        }
    }

    it("rejects missing, partial and mismatched configured identities without fallback") {
        check_equal(turbo_dc_identity_create(certificate_path, nullptr, &identity, &actual), -1);
        check_null(identity);
        check_null(actual);
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &actual), -1);
        check_null(identity);
        check_null(actual);
        check_equal(PEM_write_bio_X509(certificate_pem.get(), SSL_CTX_get0_certificate(source.get())), 1);
        tstr_free(expected);
        expected = nullptr;
        check_equal(turbo_dc_generate_identity(source.get(), &expected), 0);
        check_equal(PEM_write_bio_PKCS8PrivateKey(private_pem.get(), SSL_CTX_get0_privatekey(source.get()),
            nullptr, nullptr, 0, nullptr, nullptr), 1);
        char *bytes = nullptr;
        long size = BIO_get_mem_data(certificate_pem.get(), &bytes);
        check_equal(tt_write_file(certificate_path, bytes, static_cast<size_t>(size)), 0);
        size = BIO_get_mem_data(private_pem.get(), &bytes);
        check_equal(tt_write_file(key_path, bytes, static_cast<size_t>(size)), 0);
        check_equal(turbo_dc_identity_create(certificate_path, key_path, &identity, &actual), -1);
        check_null(identity);
        check_null(actual);
    }
}
