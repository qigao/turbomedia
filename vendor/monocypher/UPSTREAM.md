# Monocypher provenance

- Upstream: https://github.com/LoupVaillant/Monocypher
- Upstream commit: `636cc057bc8866213997eef23a190f9ff9ab1fad`
- Release tag: `4.0.3`
- License: `BSD-2-Clause OR CC0-1.0`
- Local modifications: build integration only; `monocypher.c` and `monocypher.h` are unchanged.

The vendored target is linked privately by `TurboMedia::Crypto`. Monocypher
types and symbols are not part of the TurboMedia public ABI.
