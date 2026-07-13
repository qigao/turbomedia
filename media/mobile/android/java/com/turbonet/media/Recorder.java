package com.turbonet.media;

/**
 * Compatibility stub for the older TurboWebRTC recorder wrapper.
 */
@Deprecated
public final class Recorder {
    public enum Format {
        MP4(0),
        WEBM(1),
        MKV(2);

        private final int value;
        Format(int value) { this.value = value; }
        public int getValue() { return value; }
    }

    public enum Codec {
        OPUS(0),
        AAC(1),
        PCM(2),
        H264(3),
        H265(4),
        VP8(5),
        VP9(6);

        private final int value;
        Codec(int value) { this.value = value; }
        public int getValue() { return value; }
    }

    public Recorder(String filename, Format format) {
        throw unsupported();
    }

    public void destroy() {
    }

    public int addVideoTrack(int width, int height, int framerate, Codec codec) {
        throw unsupported();
    }

    public int addAudioTrack(int sampleRate, int channels, Codec codec) {
        throw unsupported();
    }

    public boolean start() {
        throw unsupported();
    }

    public boolean stop() {
        throw unsupported();
    }

    public boolean writeFrame(int trackId, byte[] data, long timestampUs, boolean isKeyframe) {
        throw unsupported();
    }

    private static UnsupportedOperationException unsupported() {
        return new UnsupportedOperationException(
            "Recorder belongs to the old TurboWebRTC layer and is not exposed by TurboMedia");
    }
}
