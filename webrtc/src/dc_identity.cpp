#include "dc_identity.h"
#include "dc_certificate_der.h"

#include <cmeta_crypto.h>
#include <fmt.h>
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <memory>

namespace {
void clear_key(char *bytes) {
    if (bytes) cmeta_crypto_clear(bytes, tstr_len(bytes));
    tstr_free(bytes);
}

int load_pem(const char *cert_path, const char *key_path, tstr *cert, tstr *key) {
    // Preserve the existing PEM formats (including SEC1 EC and PKCS#1 RSA).
    // This temporary owner never creates a DTLS session or negotiates a cipher.
    bssl::UniquePtr<SSL_CTX> parser(SSL_CTX_new(DTLS_method()));
    if (!parser || SSL_CTX_use_certificate_file(parser.get(), cert_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(parser.get(), key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(parser.get()) != 1) return -1;
    X509 *leaf = SSL_CTX_get0_certificate(parser.get());
    int cert_size = i2d_X509(leaf, nullptr);
    if (cert_size <= 0) return -1;
    *cert = tstr_new_len(nullptr, static_cast<size_t>(cert_size));
    if (!*cert) return -1;
    auto *out = reinterpret_cast<uint8_t *>(*cert);
    if (i2d_X509(leaf, &out) != cert_size) return -1;
    // Fixed owned storage lets failure cleanup wipe partial DER without
    // inspecting a failed CBB, and avoids retaining realloc copies of secrets.
    constexpr size_t max_private_der = 16 * 1024;
    *key = tstr_new_len(nullptr, max_private_der);
    if (!*key) return -1;
    bssl::ScopedCBB encoded;
    CBB_init_fixed(encoded.get(), reinterpret_cast<uint8_t *>(*key), max_private_der);
    if (!EVP_marshal_private_key(encoded.get(), SSL_CTX_get0_privatekey(parser.get())) ||
        !CBB_flush(encoded.get())) return -1;
    return tstr_set_len_checked(*key, CBB_len(encoded.get())) ? 0 : -1;
}
}

int turbo_dc_identity_create(const char *cert_pem, const char *key_pem,
                             turbo_gdtls_context **context, tstr *fingerprint) {
    if (context) *context = nullptr;
    if (fingerprint) *fingerprint = nullptr;
    if (!context || !fingerprint || (!!cert_pem != !!key_pem)) return -1;
    tstr cert_bytes = nullptr, key_bytes = nullptr;
    int result = cert_pem ? load_pem(cert_pem, key_pem, &cert_bytes, &key_bytes)
                         : turbo_dc_generate_certificate_der(&cert_bytes, &key_bytes);
    std::unique_ptr<char, decltype(&tstr_free)> cert(cert_bytes, &tstr_free);
    std::unique_ptr<char, decltype(&clear_key)> key(key_bytes, &clear_key);
    if (result) return -1;
    std::unique_ptr<turbo_gdtls_context, decltype(&turbo_gdtls_context_destroy)> identity(
        turbo_gdtls_context_create(cert.get(), tstr_len(cert.get()), key.get(), tstr_len(key.get())),
        &turbo_gdtls_context_destroy);
    key.reset();
    if (!identity) return -1;
    uint8_t digest[32];
    if (turbo_gdtls_context_fingerprint(identity.get(), digest)) return -1;
    constexpr size_t length = sizeof(digest) * 3 - 1;
    std::unique_ptr<char, decltype(&tstr_free)> text(tstr_new_len(nullptr, length), &tstr_free);
    if (!text) return -1;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        size_t offset = i ? i * 3 - 1 : 0;
        if (fmt(text.get() + offset, length + 1 - offset, i ? ":{:02X}" : "{:02X}",
                static_cast<unsigned>(digest[i])) != (i ? 3 : 2)) return -1;
    }
    *context = identity.release();
    *fingerprint = text.release();
    return 0;
}
