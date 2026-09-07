# TurboNet Media - Android NDK Library

Complete Android implementation of TurboNet Media with native C/C++ performance and Java/Kotlin API.

## Features

### Core Capabilities
- **Video Capture**: Camera2 API with front/back camera support
- **Audio Capture**: OpenSL ES + miniaudio fallback
- **Screen Capture**: MediaProjection API for screen recording/sharing
- **Hardware Codecs**: MediaCodec API for H.264/H.265 acceleration
- **RTP/SRTP**: Real-time media streaming with encryption
- **TWCC**: Transport-Wide Congestion Control for adaptive bitrate
- **Simulcast**: Multi-quality streaming (Low/Medium/High)
- **NACK**: Packet loss recovery
- **SFU**: Selective Forwarding Unit for conferencing
- **Recording**: Save streams to MP4/WebM/MKV

### Mobile Optimizations
- **Battery-Aware**: Automatic quality adjustment based on battery level
- **Network-Adaptive**: Adjusts bitrate for WiFi/4G/3G/2G
- **Signal-Aware**: Reduces bitrate on poor signal
- **Power Modes**: Normal, Low Power, Ultra Low Power
- **Hardware Acceleration**: Uses MediaCodec when available

## Build System

### Option 1: CMake (Recommended for Android Studio)

```bash
# Configure
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_shared

# Build
cmake --build build --config Release

# Install
cmake --install build --prefix ./install
```

### Option 2: ndk-build (Alternative)

```bash
# Build all ABIs
ndk-build -j8

# Build specific ABI
ndk-build APP_ABI=arm64-v8a -j8

# Clean
ndk-build clean
```

### Option 3: Gradle (Android Studio Integration)

```bash
# Build library
./gradlew assembleRelease

# Run tests
./gradlew test

# Install to local Maven
./gradlew publishToMavenLocal
```

## Android Studio Integration

### 1. Add to your project

**settings.gradle**:
```groovy
include ':turbonet-media'
project(':turbonet-media').projectDir = new File('path/to/media/android')
```

**app/build.gradle**:
```groovy
dependencies {
    implementation project(':turbonet-media')
}
```

### 2. Permissions

**AndroidManifest.xml**:
```xml
<uses-permission android:name="android.permission.CAMERA" />
<uses-permission android:name="android.permission.RECORD_AUDIO" />
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
<uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PROJECTION" />

<uses-feature android:name="android.hardware.camera" />
<uses-feature android:name="android.hardware.camera.autofocus" />
```

### 3. Request Permissions at Runtime

```kotlin
val permissions = arrayOf(
    Manifest.permission.CAMERA,
    Manifest.permission.RECORD_AUDIO
)
ActivityCompat.requestPermissions(this, permissions, REQUEST_CODE)
```

## Java/Kotlin API Usage

### Basic Video Call

```java
import com.turbonet.media.*;

// Initialize
MediaEngine engine = new MediaEngine();
engine.initialize();

// Configure video
engine.setVideoSource("camera:1"); // Back camera
engine.setVideoCodec("H264", 1280, 720, 30, 1500000);

// Configure audio
engine.setAudioSource("audio:default");
engine.setAudioCodec("OPUS", 48000, 2, 64000);

// Set callback
engine.setFrameCallback(new MediaEngine.FrameCallback() {
    @Override
    public void onVideoFrame(byte[] data, int width, int height, long timestamp) {
        // Send via RTP or display
    }
    
    @Override
    public void onAudioFrame(byte[] data, int samples, long timestamp) {
        // Send via RTP or play
    }
});

// Start
engine.start();

// Stop
engine.stop();
engine.destroy();
```

### Simulcast Streaming

```java
import com.turbonet.media.Simulcast;

Simulcast simulcast = new Simulcast();
simulcast.initialize(1280, 720, 30);

// Enable layers
simulcast.enableLayer(Simulcast.Layer.LOW, true);    // 320x180 @ 15fps
simulcast.enableLayer(Simulcast.Layer.MEDIUM, true); // 640x360 @ 30fps
simulcast.enableLayer(Simulcast.Layer.HIGH, true);   // 1280x720 @ 30fps

// Encode frame
byte[] frame = ...; // YUV420 data
Simulcast.EncodedLayers layers = simulcast.encode(frame, timestamp);

// Send appropriate layer based on receiver bandwidth
if (receiverBandwidth > 1000000) {
    sendRTP(layers.high);
} else if (receiverBandwidth > 400000) {
    sendRTP(layers.medium);
} else {
    sendRTP(layers.low);
}
```

### Recording

```java
import com.turbonet.media.Recorder;

Recorder recorder = new Recorder();
recorder.initialize("/sdcard/recording.mp4", Recorder.Format.MP4);

// Add tracks
recorder.addVideoTrack("H264", 1280, 720, 30, 1500000);
recorder.addAudioTrack("OPUS", 48000, 2, 64000);

// Start recording
recorder.start();

// Write frames
recorder.writeVideoFrame(data, size, timestamp, isKeyframe);
recorder.writeAudioFrame(data, size, timestamp);

// Stop
recorder.stop();
recorder.destroy();
```

### Mobile Optimizations

```java
import com.turbonet.media.MobileOptimizer;

MobileOptimizer optimizer = new MobileOptimizer();

// Monitor battery
optimizer.startBatteryMonitoring(context);

// Monitor network
optimizer.startNetworkMonitoring(context);

// Get recommendations
int targetBitrate = optimizer.getTargetBitrate();
int targetFps = optimizer.getTargetFps();
int resolutionScale = optimizer.getResolutionScale();
boolean useHardwareCodec = optimizer.shouldUseHardwareCodec();
boolean enableSimulcast = optimizer.shouldEnableSimulcast();

// Apply to engine
engine.setVideoBitrate(targetBitrate);
engine.setVideoFramerate(targetFps);
```

### Screen Capture

```java
import com.turbonet.media.ScreenCapture;

ScreenCapture screenCapture = new ScreenCapture(this);
screenCapture.setCallback(new ScreenCapture.ScreenCaptureCallback() {
    @Override
    public void onPermissionGranted() {
        if (!screenCapture.start()) {
            // Handle native/VirtualDisplay startup failure.
        }
    }

    @Override public void onPermissionDenied() { }
    @Override public void onStarted() { }
    @Override public void onStopped() { }
    @Override public void onError(String error) { }
});

// Shows the system MediaProjection consent dialog.
screenCapture.requestPermission(this);

@Override
protected void onActivityResult(int requestCode, int resultCode, Intent data) {
    super.onActivityResult(requestCode, resultCode, data);
    screenCapture.onActivityResult(requestCode, resultCode, data);
}

screenCapture.stop();
screenCapture.destroy();
```

`start()` must only be called after `onPermissionGranted()`. `destroy()` releases the
VirtualDisplay, stops MediaProjection, and destroys the native ImageReader context.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Java/Kotlin API                      │
│  MediaEngine, Simulcast, Recorder, MobileOptimizer      │
└────────────────────┬────────────────────────────────────┘
                     │ JNI
┌────────────────────┴────────────────────────────────────┐
│                  Native C Library                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │ Capture  │  │  Codec   │  │   RTP    │             │
│  │ Camera2  │  │MediaCodec│  │  TWCC    │             │
│  │ OpenSLES │  │  H.264   │  │Simulcast │             │
│  │MediaProj │  │  H.265   │  │  NACK    │             │
│  └──────────┘  └──────────┘  └──────────┘             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │   SFU    │  │ Recorder │  │  Mobile  │             │
│  │Forwarding│  │ MP4/WebM │  │   Opt    │             │
│  └──────────┘  └──────────┘  └──────────┘             │
└─────────────────────────────────────────────────────────┘
```

## Performance

### Benchmarks (Pixel 6 Pro)

| Operation | Time | CPU | Memory |
|-----------|------|-----|--------|
| H.264 Encode (720p) | 8ms | 15% | 12MB |
| H.264 Decode (720p) | 6ms | 12% | 10MB |
| Simulcast Encode | 12ms | 20% | 18MB |
| RTP Send | 1ms | 2% | 2MB |
| TWCC Processing | 0.5ms | 1% | 1MB |

### Battery Impact

| Mode | Bitrate | FPS | Battery/Hour |
|------|---------|-----|--------------|
| Normal | 1.5Mbps | 30 | 15% |
| Low Power | 800Kbps | 24 | 10% |
| Ultra Low | 300Kbps | 15 | 6% |

## Supported ABIs

- arm64-v8a (64-bit ARM)
- x86_64 (64-bit Intel)
- x86 (32-bit Intel)

## Requirements

- Android API 26+ (Android 8.0+)
- NDK r25+
- CMake 3.18.1+
- Gradle 7.0+

## Dependencies

- BoringSSL（由上游网络依赖提供）
- libSRTP (SRTP)
- miniaudio (audio fallback)
- Android NDK APIs:
  - Camera2
  - MediaCodec
  - OpenSL ES
  - MediaProjection
  - Salts Capture（拥有 AImageReader/ANativeWindow，并向 JNI 提供 Surface）

## Troubleshooting

### Build Issues

**CMake can't find NDK**:
```bash
export ANDROID_NDK=/path/to/ndk
```

**Gradle sync fails**:
```bash
./gradlew clean
./gradlew --refresh-dependencies
```

### Runtime Issues

**Camera not working**:
- Check permissions granted
- Check camera in use by another app
- Try different camera ID

**Audio not working**:
- Check RECORD_AUDIO permission
- Check microphone not muted
- Try different sample rate

**Screen capture fails**:
- Must request permission via MediaProjection
- Requires foreground service on Android 10+
- Check FOREGROUND_SERVICE permission

## Examples

See `example/MainActivity.java` for complete working example.

## License

See LICENSE file in project root.

## Support

For issues and questions:
- GitHub Issues: https://github.com/turbonet/turbonet
- Documentation: https://turbonet.dev/docs/android
