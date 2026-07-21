# libsrtp BoringSSL overlay

This overlay is copied from the vcpkg `libsrtp` 2.8.0 port at vcpkg commit
`a51bb4d1434e6d0927ff79db8033bed8522b85df`. The upstream port was last
changed by commit `fb31f0bb58ab5e3b3dee4e74eaab3c9b2647801c`.

The existing `openssl` feature depends on `boringssl` instead of the mutually
exclusive `openssl` port. libSRTP continues to compile its OpenSSL-compatible
AES-ICM, AES-GCM, and HMAC-SHA1 providers. The compatibility patch removes
libSRTP's OpenSSL version constraint because BoringSSL intentionally does not
publish an OpenSSL version. On Windows, it also defines `WIN32_LEAN_AND_MEAN`
so Windows CryptoAPI declarations do not conflict with BoringSSL's
OpenSSL-compatible types.

When updating this overlay, refresh it from the matching vcpkg `ports/libsrtp`
directory, reapply the dependency and Windows compatibility changes, then run
the TurboMedia SRTP tests for AES-CM/HMAC-SHA1 and AES-GCM profiles.
