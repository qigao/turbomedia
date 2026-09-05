# TurboMedia Speech

`TurboMedia::Speech` defines provider-neutral streaming ASR and TTS sessions.
It does not select a cloud service or bundle a model. A provider adapter owns
that integration and supplies the operation table declared in
`turbo_speech.h`.

## Data and threading contract

- PCM frames, transcript text, error messages, request text, language, and
  voice strings are borrowed views. A provider copies a view before returning
  when work continues asynchronously.
- ASR `write()` runs on the audio capture thread and must not block. An async
  provider copies into its own bounded queue or returns
  `TURBO_SPEECH_ERR_BUSY`; TurboMedia does not allocate an unbounded fallback
  queue or silently drop frames. Capture users can query
  `turbo_asr_get_rejected_frame_count()` because a capture callback has no
  return channel.
- A provider serializes callbacks for one session. User callbacks may execute
  on the provider thread and must not destroy or cancel the session
  reentrantly.
- `cancel()` is the shutdown barrier: it returns only after provider callbacks
  have quiesced. The session owner serializes start, finish, cancel, and
  destroy.

## Microphone capture to ASR

When ASR is the only microphone consumer, create and start ASR with the same
sample rate, channel count, and sample width as the capture, then pass
`turbo_asr_capture_callback` and the ASR session to
`salts_audio_capture_set_callback()` before starting capture. The complete,
executable mock-provider flow is covered by
`speech/tests/test_speech.c`.

Stop capture before finishing or cancelling ASR. Destroy capture before ASR so
the capture thread cannot retain the ASR callback context.

For simultaneous WebRTC publishing and recognition, do not replace the
track-owned capture callback. Start ASR, call
`turbo_media_track_attach_asr(track, asr)` while the track is idle, then start
the track. On shutdown, stop the track, detach ASR, and finish/cancel ASR. The
ASR pointer is borrowed by the track and must outlive the attachment.

## TTS to a WebRTC audio track

The TTS audio callback forwards provider PCM to an active audio track with
`turbo_media_track_send_speech_frame()`. Return `TURBO_SPEECH_OK` only when the
track accepts the frame; otherwise return `TURBO_SPEECH_ERR_FORMAT` or the
application's mapped provider error. The complete track lifecycle is exercised
by `webrtc/tests/test_speech_integration.c`.

The provider output must be signed 16-bit interleaved PCM matching the track's
sample rate, channel count, and exact `frame_size_ms`. Resampling and chunk
reframing are deliberately outside this interface; a provider adapter must do
them explicitly rather than changing audio timing silently.

For local speaker output, the same TTS callback may call
`turbo_playback_write()`. A short write is explicit playback backpressure and
must be returned to the provider as `TURBO_SPEECH_ERR_BUSY`.
