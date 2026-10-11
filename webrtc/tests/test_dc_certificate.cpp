#include "dc_certificate.h"
#include "dc_certificate_der.h"
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
