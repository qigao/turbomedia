# Third-Party Notices

TurboMedia first-party code is licensed under the Apache License 2.0.
The components below retain their upstream license terms.

| Component | Repository path | Upstream license | Notes |
| --- | --- | --- | --- |
| CRoaring | `vendor/croar/` | Apache-2.0 OR MIT | License notices are embedded in the amalgamated source. |
| SQLite Lemon parser generator | `vendor/lemon/`, `tools/lemon/` | Public-domain dedication | The source headers explicitly disclaim copyright. |
| Monocypher | `vendor/monocypher/` | BSD-2-Clause OR CC0-1.0 | Upstream license notices are embedded in the source. |
| Unity | `vendor/unity/` | MIT | Upstream source retains its MIT SPDX/license notice. |

Dependencies downloaded by vcpkg or another package manager are not relicensed
by TurboMedia and remain governed by their respective upstream licenses.

GmSSL is a static provider dependency of the installed SaltsUtils Crypto
component used by SaltsNet/CHttp. It is restored through the existing vcpkg
manifest and shared cache, not vendored here. [GmSSL](https://github.com/guanzhi/GmSSL)
uses Apache-2.0; the shared [GmSSL port](https://github.com/qigao/vcpkg-cache/tree/master/ports/gmssl)
records its source revision, local patches and packaged license. The current
provider recipe is 3.2.0 port revision 9.

The libSRTP 2.8.0 overlay also uses that GmSSL provider for AES-CTR, AES-GCM and
HMAC-SHA1. [libSRTP](https://github.com/cisco/libsrtp) retains its BSD-3-Clause
license, installed by the port; the project-maintained adapter and patch history
are recorded in `vcpkg-overlays/libsrtp/README.md`. The upstream cipher/auth
known-answer vectors remain part of libSRTP and retain their original notices.
