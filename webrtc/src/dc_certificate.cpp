#include "dc_certificate.h"
#include "dc_certificate_der.h"

#include <cmeta_crypto.h>
#include <fmt.h>
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <limits>
#include <memory>

int turbo_dc_generate_identity(SSL_CTX *context, tstr *fingerprint) {
    if (!fingerprint) return -1;
    *fingerprint = nullptr;
    if (!context) return -1;

    tstr cert_bytes = nullptr, key_bytes = nullptr;
    if (turbo_dc_generate_certificate_der(&cert_bytes, &key_bytes) != 0) return -1;
    auto clear_private_key = [](char *bytes) {
        if (bytes) cmeta_crypto_clear(bytes, tstr_len(bytes));
        tstr_free(bytes);
    };
    std::unique_ptr<char, decltype(&tstr_free)> cert_der(cert_bytes, &tstr_free);
    std::unique_ptr<char, decltype(clear_private_key)> key_der(key_bytes, clear_private_key);
    if (tstr_len(cert_bytes) > static_cast<size_t>(std::numeric_limits<long>::max())) return -1;

    const auto *cert_in = reinterpret_cast<const uint8_t *>(cert_bytes);
    bssl::UniquePtr<X509> certificate(d2i_X509(nullptr, &cert_in,
        static_cast<long>(tstr_len(cert_bytes))));
    CBS key_in;
    CBS_init(&key_in, reinterpret_cast<const uint8_t *>(key_bytes), tstr_len(key_bytes));
    bssl::UniquePtr<EVP_PKEY> key(EVP_parse_private_key(&key_in));
    if (!certificate || !key || CBS_len(&key_in) != 0 ||
        cert_in != reinterpret_cast<const uint8_t *>(cert_bytes) + tstr_len(cert_bytes)) return -1;
    key_der.reset(); // Import owns its key; do not retain the plaintext PKCS#8 copy.

    unsigned char digest[32];
    if (cmeta_sha256(cert_bytes, tstr_len(cert_bytes), digest) != SALTS_OK) return -1;

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
