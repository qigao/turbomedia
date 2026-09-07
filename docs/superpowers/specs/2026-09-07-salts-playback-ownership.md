# Salts Playback Ownership

## Background

TurboMedia historically implemented output-device discovery, miniaudio device
ownership, PCM buffering, file playback, and playlists behind
`turbo_playback.h`. SaltsUtils now exports the same platform-facing device
capability as `Salts::Playback` on Windows, Linux, macOS, Android, and iOS.
Keeping both implementations would create two lifecycle owners and two device
identity models.

## Decision

- SaltsUtils exclusively owns capture and playback devices, native callbacks,
  bounded PCM storage, device identity, and device lifecycle.
- TurboMedia owns demux, decode, resampling, media clocks, seek, and player
  orchestration. `TurboMedia::Player` submits decoded PCM to
  `Salts::Playback` and treats a short write as backpressure.
- `turbo_playback.h`, `TurboMedia::Device`, the local miniaudio translation
  unit, and the file/playlist playback API are removed without a compatibility
  fallback.
- `turbo_player_config_t.audio_device` accepts an enumeration-scoped
  `salts_playback_device_t`; `NULL` selects the default output device.

## Alternatives

1. Keep `TurboMedia::Device` as a forwarding adapter. Rejected because it
   preserves a misleading owner and expands the installed dependency surface.
2. Keep file and playlist APIs in TurboMedia while forwarding PCM output.
   Rejected because `TurboMedia::Player` already owns file demux/decode and a
   second playlist path would retain duplicate state.
3. Use CHTTP/WebSocket as the playback transport. Rejected because local PCM
   device I/O is a data-plane boundary, not an HTTP control-plane operation.

## State and failure semantics

The player decode thread is the only PCM producer. While playback is running,
it serializes playback start, pause, resume, clear, drain, and stop; public
control calls only publish atomic intent. Before playback starts, the single
control owner may apply seek and clear synchronously while the producer is
quiescent. The Salts native callback is the only consumer. Close signals stop,
joins the producer, then destroys playback. Device creation,
start, write, drain, clear, pause, resume, and stop failures are converted to
`TURBO_PLAYER_ERR_PLAYBACK`; no alternate backend or codec is selected.

The end-of-stream drain is bounded by the configured buffer duration plus one
second. Explicit sample rates must be supported by SaltsUtils. When the caller
leaves rate or channel count unset, an unsupported source layout is normalized
to 48 kHz stereo before resampling.

## Compatibility and migration

This intentionally removes the `TurboMedia::Device` package component and
`turbo_playback_*` public API. Consumers enumerate devices and manage standalone
PCM sinks through `<salts_playback.h>`. Consumers of `turbo_player.h` replace
the former string device ID with a pointer to the unchanged Salts enumeration
result during `turbo_player_open()`.

Because the player config ABI changes, TurboMedia advances to 2.0.0 and uses
same-major package compatibility. On Windows the Player DLL is named
`turbo_media_player-2.dll`, so an old binary fails to load instead of passing a
legacy string pointer to the new device contract.

Rollback is a source revert of this change together with selecting a pre-
playback SaltsUtils package. Runtime fallback is not provided.

## Verification

- Compile the installed-package player consumer, which assigns a
  `salts_playback_device_t` to `turbo_player_config_t.audio_device`.
- Build and run the Windows test preset.
- Configure and build Android API 26, verifying that TurboMedia no longer
  compiles or links miniaudio directly.
- Search non-historical production and test sources for `turbo_playback`,
  `TurboMedia::Device`, and `MINIAUDIO`.
