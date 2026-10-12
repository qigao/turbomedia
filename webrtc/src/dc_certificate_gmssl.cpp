#include "dc_certificate_der.h"

#include <cmeta_crypto.h>
#include <gmssl/rand.h>
#include <gmssl/x509.h>

#include <ctime>
#include <limits>
#include <memory>

namespace {
struct IdentityKey {
    X509_KEY value{};
    ~IdentityKey() {
        x509_key_cleanup(&value);
        cmeta_crypto_clear(&value, sizeof(value));
    }
};
}

int turbo_dc_generate_certificate_der(tstr *certificate, tstr *private_key) {
    if (certificate) *certificate = nullptr;
    if (private_key) *private_key = nullptr;
    if (!certificate || !private_key || certificate == private_key) return -1;

    IdentityKey key;
    const int curve = OID_secp256r1;
    if (x509_key_generate(&key.value, OID_ec_public_key, &curve, sizeof(curve)) != 1)
        return -1;

    uint8_t serial[16];
    if (rand_bytes(serial, sizeof(serial)) != 1) return -1;
    serial[0] |= 1; // Keep the positive certificate serial nonzero.

    // Name construction is bounded by GmSSL; only the existing CN is emitted.
    uint8_t name[256];
    size_t name_size = 0;
    if (x509_name_set(name, &name_size, sizeof(name), nullptr, nullptr, nullptr,
            nullptr, nullptr, "TurboNet DataChannel") != 1) return -1;

    constexpr time_t validity_seconds = 365L * 24 * 60 * 60;
    const time_t not_before = std::time(nullptr);
    if (not_before < 0 || not_before == static_cast<time_t>(-1) ||
        not_before > (std::numeric_limits<time_t>::max)() - validity_seconds) return -1;
    const time_t not_after = not_before + validity_seconds;

    auto sign_certificate = [&](uint8_t **out, size_t *size) {
        return x509_cert_sign_to_der(X509_version_v3, serial, sizeof(serial),
            OID_ecdsa_with_sha256, name, name_size, not_before, not_after,
            name, name_size, &key.value, nullptr, 0, nullptr, 0, nullptr, 0,
            &key.value, nullptr, 0, out, size);
    };

    // The provider's sizing pass uses the same fixed-length ECDSA signature
    // contract as its encoding pass. Allocate exactly that DER capacity.
    size_t certificate_size = 0, private_key_size = 0;
    if (sign_certificate(nullptr, &certificate_size) != 1 || !certificate_size ||
        x509_private_key_info_to_der(&key.value, nullptr, &private_key_size) != 1 ||
        !private_key_size) return -1;

    auto clear_private_key = [](char *bytes) {
        if (bytes) cmeta_crypto_clear(bytes, tstr_len(bytes));
        tstr_free(bytes);
    };
    std::unique_ptr<char, decltype(&tstr_free)> cert(
        tstr_new_len(nullptr, certificate_size), &tstr_free);
    std::unique_ptr<char, decltype(clear_private_key)> secret(
        tstr_new_len(nullptr, private_key_size), clear_private_key);
    if (!cert || !secret) return -1;

    uint8_t *cert_out = reinterpret_cast<uint8_t *>(cert.get());
    uint8_t *key_out = reinterpret_cast<uint8_t *>(secret.get());
    size_t cert_written = 0, key_written = 0;
    if (sign_certificate(&cert_out, &cert_written) != 1 ||
        cert_written != certificate_size ||
        x509_private_key_info_to_der(&key.value, &key_out, &key_written) != 1 ||
        key_written != private_key_size) return -1;

    *certificate = cert.release();
    *private_key = secret.release();
    return 0;
}
