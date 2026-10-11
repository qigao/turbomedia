#ifndef TURBO_DC_IDENTITY_H
#define TURBO_DC_IDENTITY_H

#include "gmssl_dtls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private context factory. Both PEM file paths must be supplied, or neither
 * for an ephemeral P-256 identity. Outputs are owned, NULL on failure.
 * GmSSL reads a leaf CERTIFICATE and an unencrypted PKCS#8 PRIVATE KEY;
 * each decoded object is bounded to 16 KiB. Other encodings fail. */
int turbo_dc_identity_create(const char *cert_pem, const char *key_pem,
                             turbo_gdtls_context **context, tstr *fingerprint);

#ifdef __cplusplus
}
#endif
#endif
