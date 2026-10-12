#pragma once

#include "dc_certificate_der.h"
#include "dtls_rsa_fixture.h"

// The provider imports RSA keys but has no RSA generator. These published
// test-only bytes preserve RSA admission/signature coverage without another
// crypto dependency. P-256 identities are generated afresh for every case.
inline int dtls_test_identity(bool rsa, tstr *certificate, tstr *private_key) {
    if (!rsa) return turbo_dc_generate_certificate_der(certificate, private_key);
    *certificate = tstr_new_len(rsa_test_certificate, sizeof(rsa_test_certificate));
    *private_key = tstr_new_len(rsa_test_private_key, sizeof(rsa_test_private_key));
    return *certificate && *private_key ? 0 : -1;
}
