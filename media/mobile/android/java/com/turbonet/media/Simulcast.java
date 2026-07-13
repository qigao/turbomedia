package com.turbonet.media;

/**
 * Compatibility stub for the older TurboWebRTC simulcast wrapper.
 */
@Deprecated
public final class Simulcast {
    public Simulcast(int width, int height, int framerate, String codec) {
        throw unsupported();
    }

    public void destroy() {
    }

    public void setBandwidth(int bandwidthBps) {
        throw unsupported();
    }

    public int encodeFrame(byte[] frameData, long timestampUs) {
        throw unsupported();
    }

    private static UnsupportedOperationException unsupported() {
        return new UnsupportedOperationException(
            "Simulcast belongs to the old TurboWebRTC layer and is not exposed by TurboMedia");
    }
}
