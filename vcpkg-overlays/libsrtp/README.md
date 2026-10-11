# libsrtp GmSSL overlay

This overlay is copied from the vcpkg `libsrtp` 2.8.0 port at vcpkg commit
`a51bb4d1434e6d0927ff79db8033bed8522b85df`. The upstream port was last
changed by commit `fb31f0bb58ab5e3b3dee4e74eaab3c9b2647801c`.

The overlay requires the central vcpkg-cache GmSSL port directly. The mandatory
patch removes the provider selector and alternate crypto-engine branches, while
`gmssl_crypto.c` adapts the existing libSRTP cipher/auth tables to GmSSL AES-CTR,
AES-GCM and HMAC-SHA1. It retains libSRTP's key derivation, replay protection and
all existing AES-CM/GCM profiles. CMake exports GmSSL::GmSSL as a private link
dependency; pkg-config lists the static provider in Libs.private. The adapter
is maintained by this project; upstream library and test-vector licenses remain
unchanged. This does not replace the separate BoringSSL DTLS implementation.

ICM preserves partial CTR blocks between calls. GCM owns and reuses copied AAD,
bounded by libSRTP's INT_MAX packet-length domain, including SRTCP's separate
trailer contribution. IV reset starts a new packet; provider authentication uses
constant-time tag comparison before decrypting. Teardown wipes retained state.
No provider or profile fallback is used. See the ownership, compatibility,
qualification and rollback notes in `webrtc/docs/arch-en.md`.

When updating this overlay, refresh it from the matching vcpkg `ports/libsrtp`
directory, reapply the GmSSL adapter and build/export changes, then run the
TurboMedia CTest SRTP suite for all four public profiles, in both DTLS roles and
for RTP/SRTCP, including authentication failure/recovery. libSRTP initialization
also runs its own independent cipher/auth known-answer vectors. Publish the new
recipe through the existing media cache producer before cache-only consumer CI.
