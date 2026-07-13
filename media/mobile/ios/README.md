# TurboNet Media - iOS Library

Complete iOS implementation of TurboNet Media with native C/C++ performance and Swift API.

## Features

### Core Capabilities
- **Video Capture**: AVCaptureDevice with front/back camera support
- **Audio Capture**: AVAudioEngine for low-latency audio
- **Screen Capture**: ReplayKit 2 for screen recording/sharing
- **Hardware Codecs**: VideoToolbox API for H.264/HEVC acceleration
- **RTP/SRTP**: Real-time media streaming with encryption
- **TWCC**: Transport-Wide Congestion Control for adaptive bitrate
- **Simulcast**: Multi-quality streaming (Low/Medium/High)
- **NACK**: Packet loss recovery
- **SFU**: Selective Forwarding Unit for conferencing
- **Recording**: Save streams to MP4/MOV

### iOS Optimizations
- **Battery-Aware**: Automatic quality adjustment based on battery level
- **Network-Adaptive**: Adjusts bitrate for WiFi/Cellular
- **Low Power Mode**: Respects iOS Low Power Mode
- **Background Support**: CallKit integration for VoIP apps
- **Metal Acceleration**: GPU-accelerated video processing

## Build System

### Option 1: Swift Package Manager (Recommended)

```bash
# Add to Package.swift
dependencies: [
    .package(url: "https://github.com/turbonet/turbonet-ios", from: "1.0.0")
]
```

### Option 2: Build Script

```bash
# Build for all architectures
chmod +x build-ios.sh
./build-ios.sh

# Build for specific architecture
./build-ios.sh --arch arm64
```

### Option 3: CMake

```bash
cmake -B build -S . \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64

cmake --build build --config Release
```

## Xcode Integration

### 1. Add XCFramework

Drag `TurboMedia.xcframework` into your Xcode project and add to "Frameworks, Libraries, and Embedded Content" with "Embed & Sign".

### 2. Add Permissions (Info.plist)

```xml
<key>NSCameraUsageDescription</key>
<string>We need camera access for video calls</string>

<key>NSMicrophoneUsageDescription</key>
<string>We need microphone access for audio calls</string>

<key>UIBackgroundModes</key>
<array>
    <string>audio</string>
    <string>voip</string>
</array>
```

### 3. Import and Use

```swift
import TurboMedia

let engine = MediaEngine()
```

## Swift API Usage

### Basic Video Call

```swift
import TurboMedia
import AVFoundation

class VideoCallViewController: UIViewController {
    private var mediaEngine: MediaEngine?
    private var videoTrack: VideoTrack?
    private var audioTrack: AudioTrack?
    
    override func viewDidLoad() {
        super.viewDidLoad()
        requestPermissions()
    }
    
    private func requestPermissions() {
        AVCaptureDevice.requestAccess(for: .video) { granted in
            guard granted else { return }
            
            AVCaptureDevice.requestAccess(for: .audio) { granted in
                guard granted else { return }
                
                DispatchQueue.main.async {
                    self.startVideoCall()
                }
            }
        }
    }
    
    private func startVideoCall() {
        mediaEngine = MediaEngine()
        
        let videoConfig = VideoTrackConfig(
            width: 1280,
            height: 720,
            frameRate: 30,
            bitrate: 1_500_000,
            codec: .h264
        )
        
        videoTrack = mediaEngine?.addVideoTrack(
            config: videoConfig,
            cameraPosition: .back
        )
        
        let audioConfig = AudioTrackConfig(
            sampleRate: 48000,
            channels: 2,
            bitrate: 64000,
            codec: .opus
        )
        
        audioTrack = mediaEngine?.addAudioTrack(config: audioConfig)
        
        videoTrack?.onFrame = { frame in
            // Process video frame
        }
        
        audioTrack?.onFrame = { frame in
            // Process audio frame
        }
        
        videoTrack?.start()
        audioTrack?.start()
    }
    
    deinit {
        videoTrack?.stop()
        audioTrack?.stop()
    }
}
```

### With Mobile Optimizations

```swift
import TurboMedia

class OptimizedCallViewController: UIViewController {
    private var optimizer: MobileOptimizer?
    private var mediaEngine: MediaEngine?
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        optimizer = MobileOptimizer()
        optimizer?.startBatteryMonitoring()
        optimizer?.startNetworkMonitoring()
        
        if let settings = optimizer?.recommendedSettings {
            setupMediaEngine(with: settings)
        }
    }
    
    private func setupMediaEngine(with settings: MobileOptimizer.OptimizedSettings) {
        mediaEngine = MediaEngine()
        
        let videoConfig = VideoTrackConfig(
            width: Int(settings.resolution.width),
            height: Int(settings.resolution.height),
            frameRate: settings.frameRate,
            bitrate: settings.videoBitrate,
            codec: settings.useHardwareCodec ? .h264 : .vp8
        )
        
        let videoTrack = mediaEngine?.addVideoTrack(
            config: videoConfig,
            cameraPosition: .back
        )
        
        videoTrack?.start()
        
        print("Optimized settings:")
        print("  Resolution: \(settings.resolution)")
        print("  Frame rate: \(settings.frameRate) fps")
        print("  Bitrate: \(settings.videoBitrate / 1000) kbps")
    }
    
    deinit {
        optimizer?.stopMonitoring()
    }
}
```

### Recording

```swift
import TurboMedia

class RecordingViewController: UIViewController {
    private var recorder: Recorder?
    
    func startRecording() {
        let documentsPath = FileManager.default.urls(
            for: .documentDirectory,
            in: .userDomainMask
        )[0]
        
        let outputURL = documentsPath.appendingPathComponent("recording.mp4")
        
        recorder = Recorder(outputURL: outputURL, format: .mp4)
        
        _ = recorder?.addVideoTrack(
            codec: .h264,
            width: 1280,
            height: 720,
            frameRate: 30,
            bitrate: 1_500_000
        )
        
        _ = recorder?.addAudioTrack(
            codec: .opus,
            sampleRate: 48000,
            channels: 2,
            bitrate: 64000
        )
        
        recorder?.start()
        
        print("Recording started: \(outputURL.path)")
    }
    
    func stopRecording() {
        recorder?.stop()
        
        if let stats = recorder?.statistics {
            print("Recording stats:")
            print("  Duration: \(stats.duration)s")
            print("  Size: \(stats.fileSize / 1024 / 1024) MB")
        }
        
        recorder = nil
    }
}
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Swift API                            │
│  MediaEngine, Simulcast, Recorder, MobileOptimizer      │
└────────────────────┬────────────────────────────────────┘
                     │ Swift/C Bridge
┌────────────────────┴────────────────────────────────────┐
│                  Native C Library                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │ Capture  │  │  Codec   │  │   RTP    │             │
│  │AVCapture │  │VideoTool │  │  TWCC    │             │
│  │AVAudio   │  │  H.264   │  │Simulcast │             │
│  │ReplayKit │  │  HEVC    │  │  NACK    │             │
│  └──────────┘  └──────────┘  └──────────┘             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │   SFU    │  │ Recorder │  │  Mobile  │             │
│  │Forwarding│  │ MP4/MOV  │  │   Opt    │             │
│  └──────────┘  └──────────┘  └──────────┘             │
└─────────────────────────────────────────────────────────┘
```

## Performance

### Benchmarks (iPhone 13 Pro)

| Operation | Time | CPU | Memory |
|-----------|------|-----|--------|
| H.264 Encode (720p) | 6ms | 12% | 10MB |
| H.264 Decode (720p) | 4ms | 8% | 8MB |
| Simulcast Encode | 10ms | 18% | 15MB |
| RTP Send | 0.8ms | 2% | 2MB |
| TWCC Processing | 0.3ms | 1% | 1MB |

### Battery Impact

| Mode | Bitrate | FPS | Battery/Hour |
|------|---------|-----|--------------|
| Normal | 2.0Mbps | 30 | 12% |
| Low Power | 800Kbps | 24 | 8% |
| Ultra Low | 300Kbps | 15 | 5% |

## Supported Devices

- iPhone 6s and later
- iPad Air 2 and later
- iPad mini 4 and later
- iPod touch (7th generation)

## Requirements

- iOS 14.0+
- Xcode 14.0+
- Swift 5.7+

## Dependencies

- AVFoundation (video/audio capture)
- VideoToolbox (hardware codecs)
- ReplayKit (screen capture)
- Network (network monitoring)
- Metal (GPU acceleration)

## Troubleshooting

### Camera not working
- Check camera permission granted
- Verify no other app using camera
- Try different camera position

### Audio not working
- Check microphone permission
- Verify audio session configuration
- Check for audio interruptions

### High battery drain
- Enable Low Power Mode optimizations
- Reduce frame rate and resolution
- Use hardware codecs

## Examples

See `example/` directory for complete working examples.

## License

See LICENSE file in project root.

## Support

- GitHub Issues: https://github.com/turbonet/turbonet
- Documentation: https://turbonet.dev/docs/ios
