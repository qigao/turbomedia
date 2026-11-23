# TurboMedia Recognition

`TurboMedia::Recognition` supplies provider-neutral interfaces for:

- streaming voice activity detection (VAD);
- biometric voiceprint extraction;
- perceptual audio/video content fingerprint extraction;
- normalized fingerprint comparison and threshold decisions.

It deliberately does not hash PCM or pixels and call the result a perceptual
fingerprint. Applications provide a maintained DSP/ML adapter through the
operation tables in `turbo_recognition.h`.

## Data, threading, and shutdown

- Input frames and callback results are borrowed for the duration of the call.
  An asynchronous provider copies them into provider-owned bounded storage or
  returns `TURBO_RECOGNITION_ERR_BUSY`.
- TurboMedia does not allocate a hidden queue. Rejected frames are observable
  through each session's rejected-frame counter.
- Providers serialize callbacks for one session. `cancel()` is a callback
  quiescence boundary and the owner serializes lifecycle calls.
- `max_duration_us` is enforced by the fingerprint wrapper before provider
  submission. It bounds offline VOD and live capture analysis.

## Capture and WebRTC

For a detector-only or fingerprint-only microphone capture, configure the
session with the exact capture PCM format and use
`turbo_voice_detector_capture_callback` or
`turbo_fingerprint_capture_callback`.

For simultaneous WebRTC publishing, attach running sessions while the audio
track is idle with `turbo_media_track_attach_voice_detector()` and
`turbo_media_track_attach_voice_fingerprint()`. Stop the track and detach both
before cancelling or destroying the borrowed sessions. Analysis backpressure
does not interrupt RTP transmission.

## Offline and VOD media

FFmpeg-backed `turbo_player_t` already decodes local files, HTTP media, HLS VOD,
and DASH inputs. Set its audio/video callbacks to
`turbo_fingerprint_player_audio_callback` and
`turbo_fingerprint_player_video_callback`. The adapters forward borrowed F32
audio or I420/RGBA video without an additional copy. A caller that also needs
application callbacks owns a small fan-out callback and invokes both sinks.

## Matching and biometric safety

The matcher accepts only fingerprints from the same domain, algorithm, and
model version. Provider similarity must be normalized to `[0, 1]`, where larger
means more similar; TurboMedia applies the caller's explicit threshold.

Voiceprints are biometric data. Do not log them, place them in URLs, or persist
them unencrypted. Enrollment, consent, retention, revocation, liveness/anti-
spoofing, and threshold calibration remain application/provider policy. A high
similarity score alone is not an authentication decision.
