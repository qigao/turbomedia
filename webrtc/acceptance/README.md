# TurboMedia WebRTC Acceptance

This package is the repository-side acceptance harness for real-browser WebRTC/TURN verification. It owns
contracts, orchestration, bounded adapters, evidence classification, reports, and CLI behavior. External
labs own Selenium Grid nodes, TURN infrastructure, public topology controls, certificates, DNS, and
provider executables.

**A passing contract lab is not production release evidence.** Contract-lab reports always use
`environment.kind=contract_lab` and `release_eligible=false`. Issue #17 remains the public-browser/TURN
release gate until the approved release manifest is executed against the real lab and its artifacts are
retained.

## Install and verify

```sh
npm ci --ignore-scripts
npm test
npm run audit:deps
npm run audit:delivery
```

The GitHub acceptance gate uses the locked dependency graph and npm cache, runs the full Node test suite,
checks high/critical dependency advisories, and runs the delivery/example secret audit.

## CLI

Validate only:

```sh
npm run validate -- --manifest examples/diagnostic.manifest.json
```

Run acceptance:

```sh
npm run acceptance -- --manifest PATH [--output DIR] [--label TEXT]
```

Exit codes are stable: PASS=0, FAIL=2, INCOMPLETE=3, ERROR=4. stdout is one canonical JSON summary line;
fixed diagnostics go to stderr. The first SIGINT/SIGTERM cancels once and allows bounded drain. A second
signal exits with ERROR and warns that artifacts may be incomplete. See [CLI.md](CLI.md).

## Profiles and examples

- `examples/diagnostic.manifest.json`: a single-browser diagnostic placeholder.
- `examples/release.manifest.json`: the four-browser IPv4/IPv6 release-shape placeholder.
- `contract_lab`: local protocol-boundary verification only; never release-eligible.

Example manifests intentionally use `example.test` / `.invalid` placeholders and contain no real
credentials, private endpoints, or production account data.

## External lab prerequisites

A real diagnostic/release run requires all of the following before media resources are created:

- exact Selenium Grid browser/version/platform capabilities from the environment variable named by
  `grid.endpoint_env`;
- HTTPS test page matching `source.test_page_sha256`, with the required CORS/header exposure;
- SFU health/ready/control/media endpoints matching the manifest;
- a TURN credential provider and SFU token provider;
- topology setup/transition/teardown/probe hooks;
- TURN reachability and cleanup-baseline evidence for non-diagnostic profiles;
- writable artifact storage.

Release preflight is browser-first. If the required Grid/browser capability is unavailable, the run is
INCOMPLETE and provider/SFU/media side effects do not start.

## Provider process contract

Provider commands are argv arrays from the manifest. They are launched with `shell=false`; request JSON
is sent on stdin, bounded stdout is parsed as exactly one JSON value, and provider stderr/raw output is
never copied into public diagnostics.

TURN responses use the v1 TURN credential schema and include a stable non-secret `credential_id`,
TURN URLs, short-lived username/credential, issued/expiry timestamps, and coturn version.

SFU responses use the v1 SFU token schema. Requests bind audience, exact scope list, room, participant,
subject, and validity deadline. Control and media capabilities are issued separately; no all-powerful token
fallback is used.

## Topology hook contract

Hook commands are also argv arrays with stdin/stdout JSON. Hook receipts use schema v2 and must correlate
run/case, topology, action, controller-owned generation/sequence, effective status, evidence ID, and the
canonical relay-contract SHA-256. Topology names never imply relay behavior. See [CONTRACTS.md](CONTRACTS.md).

## Contract lab

The contract lab uses real external boundaries while staying local:

- provider and topology-hook child processes over stdin/stdout JSON;
- Selenium/WebDriver over an HTTP Grid boundary;
- SFU control plus WHIP/WHEP/PATCH/DELETE over HTTP;
- short-lived TURN credential expiry, fresh credential rotation, topology transition, ICE restart,
  stale-ETag rejection, media recovery, and full resource drain.

The suite includes PASS plus injected host-candidate, expired-credential-accepted, stale-ETag-accepted, and
cleanup-residue failures. PASS emits schema-valid `run.json`, `summary.md`, and `junit.xml`.

## Artifacts and cleanup audit

Report publication is atomic and hash-verified. The completion marker is written only after all report
formats are committed. Cleanup preserves the first primary failure and records later cleanup failures.

Drain order is fixed and bounded:

1. stop sampling;
2. delete/close media resources and browser sessions;
3. verify case resources are zero;
4. verify shared SFU baseline;
5. verify TURN baseline;
6. teardown topology;
7. publish the final report.

No cleanup failure may replace the first primary FAIL/INCOMPLETE/ERROR.

## Native repository regression

The delivery audit verifies that top-level CMake and `webrtc/CMakeLists.txt` do not reference this Node
acceptance package, npm, `node_modules`, or its `package.json`. The native Linux release build preset
must continue to target `release-linux-ninja`.

In a fully provisioned native build environment (Salts/SaltsUtils/SaltsNet package roots, required
SERVER-only package roots when applicable, and the pinned vcpkg toolchain), reproduce the native release
regression with:

```sh
cmake -S . -B build/linux-gcc-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTURBO_MEDIA_PRODUCT=CLIENT \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build/linux-gcc-release
ctest --test-dir build/linux-gcc-release --output-on-failure
```

Use `TURBO_MEDIA_PRODUCT=SERVER` only in an environment that also provides the documented RulesForge,
TurboDB, and PostgreSQL-only Orm package roots. The lightweight WebRTC acceptance workflow intentionally
does not pretend those native dependencies are installed.

## Release gate

Repository harness completion, unit tests, contract-lab PASS, or local browser smoke tests do **not** close
#17. Release eligibility requires the approved release manifest to produce retained evidence from the real
public lab for the required Chrome/Edge/Firefox/Safari matrix, IPv4/IPv6, TURN-only selected pairs,
credential expiry, migration/recovery, media/RTCP evidence, and successful drain.
