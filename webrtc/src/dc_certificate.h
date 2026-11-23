#ifndef TURBO_DC_CERTIFICATE_H
#define TURBO_DC_CERTIFICATE_H

#include <crypto/tls_credentials.h>
#include <core/tstr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The caller owns both outputs on success. NULL paths generate an ephemeral
 * ECDSA P-256 identity. */
int turbo_dc_create_identity(const char *cert_path, const char *key_path,
    crypto_tls_credential_object **identity, tstr *fingerprint);

#ifdef __cplusplus
}
#endif

#endif
