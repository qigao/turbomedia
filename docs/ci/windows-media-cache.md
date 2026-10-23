# Windows media binary cache production

`Platform Build` consumes `turbomedia-media_*` packages on Windows and the
existing shared `vcpkg-cache_*` packages on other platforms, read-only with
`--only-binarycaching`. Missing ABI packages fail before configuration;
normal builds do not compile dependencies or publish cache entries.

The separate `Windows media binary cache` workflow owns production of this
repository's media dependency closure. This follows the infrastructure
repository's [product-owned producer policy](https://github.com/qigao/vcpkg-cache/blob/2e214ae44a3e3244eded5db6361194576356fc9e/README.md#product-owned-cache-production).
It uses this repository's
manifest, overlays and triplet, and the same locked shared infrastructure snapshot, vcpkg scripts
and executable as Platform Build. Only this producer job has `packages: write`.
It does not produce or publish a TurboMedia SDK.

Run this Windows-only workflow when a runner image changes the required package
ABIs. It records the exact recipe commit, image version, compiler file digest,
MSVC/Windows SDK versions and shared cache contract. After production it removes
the local binary cache and restores into a fresh installation using
`--only-binarycaching`; all packages must download from GitHub Packages with curl and restore successfully.
Then rerun only the failed Windows Platform Build job on the same product SHA.
Successful production alone does not qualify the product build or runtime tests.

Publication is restricted to this repository's own packages. The Windows
producer uses the `turbomedia-media` ID prefix and
`VCPKG_NUGET_REPOSITORY=https://github.com/qigao/turbomedia`, so new packages
are associated with the repository whose workflow token publishes them.
The account-level NuGet feed URL remains unchanged. Existing `vcpkg-cache_*`
packages belong to the shared repository and are never pushed by this producer.
The historical shared-package publication probe has been removed.

The initial producer must build and publish the product-owned Windows closure;
there is no read fallback to shared Windows packages. Namespace selection does
not change the locked build recipe or ABI validation. A rejected publication
or cache-only restore remains a failure, and normal builds never produce
dependencies. Only a successful clean L2 restore qualifies publication.

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

## curl package transport

All native dependency setup in this repository uses the local
`.github/actions/setup-native-deps` composite. It bootstraps the exact shared
vcpkg scripts/tool pins from `cmake/ci/m16-sdk-lock.json`, preserving the shared
contract hash and canonical overlay paths. It does not execute the upstream
action steps that install Mono or prewarm NuGet.exe.

`cmake/ci/github_packages.py` downloads SDKs, re2c, and media cache packages
using curl. Following [Stun #242](https://github.com/qigao/stun/pull/242), it
resolves `PackageBaseAddress/3.0.0` from the authenticated
`https://nuget.pkg.github.com/qigao/index.json` service index once per invocation.
The discovered URL must stay within the same HTTPS GitHub Packages owner path;
missing or ambiguous endpoints fail. Downloads use `GITHUB_ACTOR:GITHUB_TOKEN`
authentication and bounded transient-error retries. Index errors, including
HTTP 404, are always fatal; only an explicit producer may accept a package 404.
SDK source-tag/release provenance, original nupkg SHA256, CMake ownership and
RID contract checks remain enforced. SDK nuspec dependencies must fit the
locked graph; re2c is now explicitly pinned to the previously resolved 4.6.4.
There is no alternate feed or release-asset download fallback.

For media libraries, vcpkg's `--dry-run --x-write-nuget-packages-config` supplies
the exact package IDs, normalized versions and ABIs. The helper validates
nuspec identity and hashes each package's `share/<port>/vcpkg_abi_info.txt`,
then converts the payload to vcpkg's native `<abi[:2]>/<abi>.zip` file cache.
The real install still uses `--only-binarycaching`. Download credentials go to
curl over stdin, not process arguments or a persisted NuGet configuration.

Only the explicit Windows producer allows HTTP 404 cache misses and source
builds. Its native Windows NuGet client is used for upload only; downloads use
curl. The temporary upload configuration is removed when the helper exits.
Authentication, network, identity and ABI errors are fatal in both modes.
The final producer check deletes the file cache and downloads the entire plan
again, proving remote publication rather than reusing locally built archives.
