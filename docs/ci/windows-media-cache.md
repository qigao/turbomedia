# Windows media binary cache production

`Platform Build` consumes the shared `vcpkg-cache_*` NuGet namespace read-only
with `--only-binarycaching`. Missing ABI packages fail before configuration;
normal builds do not compile dependencies or publish cache entries.

The separate `Windows media binary cache` workflow owns production of this
repository's media dependency closure. This follows the infrastructure
repository's [product-owned producer policy](https://github.com/qigao/vcpkg-cache/blob/2e214ae44a3e3244eded5db6361194576356fc9e/README.md#product-owned-cache-production).
It uses this repository's
manifest, overlays and triplet, and the same locked shared Action, vcpkg scripts
and executable as Platform Build. Only this producer job has `packages: write`.
It does not produce or publish a TurboMedia SDK.

Run this Windows-only workflow when a runner image changes the required package
ABIs. It records the exact recipe commit, image version, compiler file digest,
MSVC/Windows SDK versions and shared cache contract. After production it removes
the local binary cache and restores into a fresh installation using
`--only-binarycaching`; a source build or missing NuGet restore fails validation.
Then rerun only the failed Windows Platform Build job on the same product SHA.
Successful production alone does not qualify the product build or runtime tests.

GitHub Packages must grant the producer token write access to the existing
namespace. A rejected publication or cache-only restore remains a failure; do
not rename the namespace, relax ABI checks, or make normal builds produce their
own dependencies to bypass it.

The motivating failure is Platform Build `39888884262`, Windows job
`120203565530`, on master `3897002ffa1aea8fe76e18b74a265b88f91c64a9`.
The previously successful PR run used image `20261012.333.1`; the failing master
run used `20261021.343.1`. Both report MSVC `14.44.35207` and Windows SDK
`10.0.26100.0`, but vcpkg requested different ABI hashes for all 27 packages.
For example, `vcpkg-cmake-config` changed from
`54cb9c70ca64f19271062b63dba4fb536a3e92c62c5652fa2e77a74ed5d85fc9` to
`3aba26ccdb0348ba29399deb355b75cebc2743335efe52795797298db5a2f466`.
The logs establish image/ABI drift and a cache miss, not a TurboMedia test
failure or proof of the precise compiler/tool input that changed.
