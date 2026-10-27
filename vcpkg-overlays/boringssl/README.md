# TurboMedia BoringSSL overlay

This port is **product-owned** and exists only because TurboMedia's
`libsrtp` dependency and native transport stack explicitly require
BoringSSL's OpenSSL-compatible API. The same source/test/patch files
formerly lived under `qigao/vcpkg-cache/ports/boringssl`.

The nine original tracked files were copied byte-for-byte; their Git blobs
are unchanged. In particular `0004-ios-library-only-install.patch`
avoids installing the `bssl` executable on iOS device and simulator,
where the public unmodified vcpkg port cannot satisfy this package contract.

`qigao/vcpkg-cache` now owns generic vcpkg tools, shared registry and
binary cache, but **does not own a BoringSSL port**. TurboMedia's
`vcpkg-configuration.json` already selects its `vcpkg-overlays/`
directory, so this is an ordinary product-local static input. The
public `Salts::Crypto` package continues to use GmSSL for its own
TLS functionality; the two dependencies are not aliases or fallbacks.

Port source provenance and platform behavior stay qualified by the
product's own native SDK and the six-platform TurboMedia media cache.
