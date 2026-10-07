# WebRTC Acceptance CLI

## Commands

Validate a manifest without creating media resources:

```sh
npm run validate -- --manifest PATH
```

Run acceptance:

```sh
npm run acceptance -- --manifest PATH [--output DIR] [--label TEXT]
```

Arguments are strict. Unknown flags, duplicate flags, missing values, and runtime overrides other than
`--output` and `--label` are errors. External provider and topology-hook requests are serialized to
stdin of bounded child processes; tokens and credentials are not placed in child argv.

## Exit codes

| Outcome | Exit code |
| --- | ---: |
| PASS | 0 |
| FAIL | 2 |
| INCOMPLETE | 3 |
| ERROR | 4 |

stdout contains exactly one canonical JSON summary line. Diagnostics use fixed public error codes on stderr;
external exception text, provider output, TURN credentials, SFU tokens, and Grid credentials are not echoed.

## Signals

The first SIGINT or SIGTERM latches cancellation and aborts the active run once. Controller drain continues
with its independent bounded cleanup signal. A second SIGINT/SIGTERM exits with code 4 and warns on stderr
that artifacts may be incomplete.

## Runtime evidence

The Grid endpoint is read only from the environment variable named by `grid.endpoint_env` in the manifest.
The production runtime composes the existing bounded provider/hook process adapter, SFU adapter, and Selenium
adapter. It never falls back to a local browser or a different protocol path.

Release runs fail closed when required release evidence is unavailable. In particular, the current runtime
does not manufacture TURN allocation-baseline evidence or credential-expiry rejection evidence. Those gaps
remain part of the external public-browser/TURN acceptance work tracked by #17.
