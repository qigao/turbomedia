#ifndef TURBO_DC_CERTIFICATE_DER_H
#define TURBO_DC_CERTIFICATE_DER_H

#include <tstr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private provider boundary: generate an owned X.509 certificate and PKCS#8
 * private key, both DER. Outputs are NULL on failure. The caller must securely
 * clear the private-key bytes before tstr_free, including after import. */
int turbo_dc_generate_certificate_der(tstr *certificate, tstr *private_key);

#ifdef __cplusplus
}
#endif

#endif
