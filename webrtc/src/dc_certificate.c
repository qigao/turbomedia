#include "dc_certificate.h"
#include <crypto/dtls_stream.h>
#include <crypto/key.h>
#include <core/fmt.h>
#include <file/fs.h>
#include <fcntl.h>

void dc_secure_clear(void *buffer, size_t size) {
    volatile unsigned char *bytes = buffer;
    while (size--) *bytes++ = 0;
}

static size_t read_pem(const char *path, uint8_t *buffer, size_t capacity) {
    file_t fd = fs_open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t used = 0;
    int n = 0;
    while (used < capacity && (n = fs_read(fd, (char *)buffer + used, capacity - used)) > 0)
        used += (size_t)n;
    (void)fs_close(fd);
    return n < 0 || used == capacity ? 0 : used;
}

int turbo_dc_create_identity(const char *cert_path, const char *key_path,
    crypto_tls_credential_object **identity, tstr *fingerprint) {
    *identity = NULL;
    *fingerprint = NULL;
    int status;
    if (cert_path && key_path) {
        uint8_t cert[CRYPTO_TLS_CREDENTIAL_INPUT_MAX_BYTES + 1];
        uint8_t key[CRYPTO_TLS_CREDENTIAL_INPUT_MAX_BYTES + 1];
        uint8_t private_der[SALTS_CRYPTO_KEY_DER_MAX_SIZE];
        size_t private_size = 0;
        const size_t cert_size = read_pem(cert_path, cert, sizeof(cert));
        const size_t key_size = read_pem(key_path, key, sizeof(key));
        salts_crypto_key *private_key = NULL;
        status = salts_crypto_key_from_pem(key, key_size, 1, &private_key);
        dc_secure_clear(key, sizeof(key));
        if (!status) status = salts_crypto_key_to_der(private_key, 1, private_der,
                                                       sizeof(private_der), &private_size);
        salts_crypto_key_destroy(private_key);
        if (!status) {
            const crypto_tls_credentials input = {
                .size = sizeof(input),
                .certificate_chain = {cert, cert_size, CRYPTO_TLS_ENCODING_PEM},
                .private_key = {private_der, private_size, CRYPTO_TLS_ENCODING_DER}
            };
            status = crypto_tls_credential_object_create(&input, CRYPTO_TLS_CREDENTIAL_SINGLE,
                                                          identity);
        }
        dc_secure_clear(private_der, sizeof(private_der));
    } else {
        status = crypto_dtls_srtp_identity_create(identity);
    }
    if (status) return -1;

    uint8_t digest[CRYPTO_DTLS_FINGERPRINT_BYTES];
    const size_t text_size = sizeof(digest) * 3u - 1u;
    tstr text = NULL;
    if (crypto_dtls_srtp_identity_fingerprint(*identity, digest) != 0 ||
        !(text = tstr_new_len(NULL, text_size))) goto fail;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        const size_t offset = i ? i * 3u - 1u : 0u;
        if (fmt(text + offset, text_size + 1u - offset, i ? ":{:02X}" : "{:02X}",
                (unsigned int)digest[i]) != (i ? 3 : 2)) goto fail;
    }
    *fingerprint = text;
    return 0;
fail:
    tstr_free(text);
    crypto_tls_credential_object_release(*identity);
    *identity = NULL;
    return -1;
}
