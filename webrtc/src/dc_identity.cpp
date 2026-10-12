#include "dc_identity.h"
#include "dc_certificate_der.h"

#include <cmeta_crypto.h>
#include <fmt.h>
#include <gmssl/pem.h>
#include <memory>

namespace {
void clear_key(char *bytes) {
    if (bytes) cmeta_crypto_clear(bytes, tstr_len(bytes));
    tstr_free(bytes);
}

int read_pem(const char *path, const char *label, size_t capacity, tstr *bytes) {
    // FILE is the GmSSL PEM boundary. A single owned, bounded allocation is
    // retained at full length on failure so the caller wipes partial secrets.
    std::unique_ptr<FILE, decltype(&fclose)> input(fopen(path, "rb"), &fclose);
    if (!input) return -1;
    *bytes = tstr_new_len(nullptr, capacity);
    if (!*bytes) return -1;
    size_t size = 0;
    if (pem_read(input.get(), label, reinterpret_cast<uint8_t *>(*bytes),
                 &size, capacity) != 1 || !size || size > capacity || ferror(input.get()))
        return -1;
    cmeta_crypto_clear(*bytes + size, capacity - size);
    return tstr_set_len_checked(*bytes, size) ? 0 : -1;
}

int load_pem(const char *cert_path, const char *key_path, tstr *cert, tstr *key) {
    constexpr size_t max_certificate_der = 16 * 1024;
    constexpr size_t max_private_der = 16 * 1024;
    if (read_pem(cert_path, "CERTIFICATE", max_certificate_der, cert)) return -1;
    // PKCS#8 is the engine's native key format. Do not retry other encodings,
    // decrypt keys, or silently generate an identity after a configured failure.
    return read_pem(key_path, "PRIVATE KEY", max_private_der, key);
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
