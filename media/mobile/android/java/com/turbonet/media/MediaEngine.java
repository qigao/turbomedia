package com.turbonet.media;

/**
 * Compatibility stub for the older TurboWebRTC media-engine wrapper.
 *
 * TurboMedia currently owns capture, playback, mobile helpers, and staged
 * hardware codec hooks. It does not expose the WebRTC media-engine track API.
 */
@Deprecated
public final class MediaEngine {
    public MediaEngine() {
        throw unsupported();
    }

    public void destroy() {
    }

    public long addVideoTrack(int width, int height, int framerate, int bitrate, String codec) {
        throw unsupported();
    }

    public long addAudioTrack(int sampleRate, int channels, int bitrate) {
        throw unsupported();
    }

    public boolean startTrack(long trackHandle) {
        throw unsupported();
    }

    public boolean stopTrack(long trackHandle) {
        throw unsupported();
    }

    private static UnsupportedOperationException unsupported() {
        return new UnsupportedOperationException(
            "MediaEngine belongs to the old TurboWebRTC layer and is not exposed by TurboMedia");
    }
}
