#ifndef TURBO_DC_CERTIFICATE_H
#define TURBO_DC_CERTIFICATE_H

#include <openssl/ssl.h>
#include <tstr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs an ephemeral ECDSA P-256 identity. On success the caller owns
 * *fingerprint; on failure it is NULL. The SSL context retains the identity. */
int turbo_dc_generate_identity(SSL_CTX *context, tstr *fingerprint);

#ifdef __cplusplus
}
#endif

#endif
