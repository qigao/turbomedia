#include "dc_certificate.h"

#include <fmt.h>
#include <openssl/bn.h>
#include <openssl/ec_key.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <memory>

int turbo_dc_generate_identity(SSL_CTX *context, tstr *fingerprint) {
    if (!fingerprint) return -1;
    *fingerprint = nullptr;
    if (!context) return -1;

    bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
    bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
    bssl::UniquePtr<X509> certificate(X509_new());
    if (!ec || !key || !certificate || EC_KEY_generate_key(ec.get()) != 1 ||
        EVP_PKEY_set1_EC_KEY(key.get(), ec.get()) != 1) return -1;

    unsigned char serial_bytes[16];
    if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) != 1) return -1;
    serial_bytes[0] |= 1; // Keep the positive certificate serial nonzero.
    bssl::UniquePtr<BIGNUM> serial(BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr));
    if (!serial || !BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get())) ||
        X509_set_version(certificate.get(), 2) != 1 ||
        !X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_get_notAfter(certificate.get()), 365L * 24 * 60 * 60) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) return -1;

    X509_NAME *subject = X509_get_subject_name(certificate.get());
    if (!subject || X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>("TurboNet DataChannel"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) return -1;

    unsigned char digest[32];
    unsigned int digest_size = 0;
    if (X509_digest(certificate.get(), EVP_sha256(), digest, &digest_size) != 1 ||
        digest_size != sizeof(digest)) return -1;

    constexpr size_t fingerprint_size = sizeof(digest) * 3 - 1;
    std::unique_ptr<char, decltype(&tstr_free)> text(
        tstr_new_len(nullptr, fingerprint_size), &tstr_free);
    if (!text) return -1;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        const size_t offset = i == 0 ? 0 : i * 3 - 1;
        const int expected = i == 0 ? 2 : 3;
        if (fmt(text.get() + offset, fingerprint_size + 1 - offset,
                i == 0 ? "{:02X}" : ":{:02X}", static_cast<unsigned int>(digest[i])) != expected)
            return -1;
    }
    if (SSL_CTX_use_certificate(context, certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context, key.get()) != 1 ||
        SSL_CTX_check_private_key(context) != 1) return -1;
    *fingerprint = text.release();
    return 0;
}
