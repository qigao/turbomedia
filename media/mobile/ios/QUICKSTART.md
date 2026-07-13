# TurboNet Media iOS - Quick Start Guide

Get up and running with TurboNet Media on iOS in 5 minutes.

## Step 1: Build the Library

### Option A: Using Build Script (Easiest)

```bash
cd media/ios
chmod +x build-ios.sh
./build-ios.sh
```

This creates `TurboMedia.xcframework` ready for use.

### Option B: Swift Package Manager

Add to your `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/turbonet/turbonet-ios", from: "1.0.0")
]
```

## Step 2: Add to Xcode Project

### Drag and Drop

1. Drag `TurboMedia.xcframework` into your Xcode project
2. In target settings, go to "Frameworks, Libraries, and Embedded Content"
3. Set to "Embed & Sign"

## Step 3: Add Permissions

Edit `Info.plist`:

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

## Step 4: Request Permissions

```swift
import AVFoundation

func requestPermissions() {
    AVCaptureDevice.requestAccess(for: .video) { granted in
        print("Camera: \(granted)")
    }
    
    AVCaptureDevice.requestAccess(for: .audio) { granted in
        print("Microphone: \(granted)")
    }
}
```

## Step 5: Use the Library

### Simple Video Call

```swift
import TurboMedia

class ViewController: UIViewController {
    private var engine: MediaEngine?
    private var videoTrack: VideoTrack?
    
    override func viewDidLoad() {
        super.viewDidLoad()
        startCall()
    }
    
    func startCall() {
        engine = MediaEngine()
        
        let config = VideoTrackConfig(
            width: 1280,
            height: 720,
            frameRate: 30,
            bitrate: 1_500_000,
            codec: .h264
        )
        
        videoTrack = engine?.addVideoTrack(
            config: config,
            cameraPosition: .back
        )
        
        videoTrack?.onFrame = { frame in
            print("Video frame: \(frame.width)x\(frame.height)")
        }
        
        videoTrack?.start()
    }
    
    deinit {
        videoTrack?.stop()
    }
}
```

### With Optimizations

```swift
import TurboMedia

class OptimizedViewController: UIViewController {
    private var optimizer: MobileOptimizer?
    private var engine: MediaEngine?
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        optimizer = MobileOptimizer()
        optimizer?.startBatteryMonitoring()
        optimizer?.startNetworkMonitoring()
        
        let settings = optimizer?.recommendedSettings
        
        let config = VideoTrackConfig(
            width: Int(settings?.resolution.width ?? 1280),
            height: Int(settings?.resolution.height ?? 720),
            frameRate: settings?.frameRate ?? 30,
            bitrate: settings?.videoBitrate ?? 1_500_000,
            codec: .h264
        )
        
        engine = MediaEngine()
        let videoTrack = engine?.addVideoTrack(
            config: config,
            cameraPosition: .back
        )
        
        videoTrack?.start()
        
        print("Using optimized settings:")
        print("  Resolution: \(settings?.resolution ?? .zero)")
        print("  FPS: \(settings?.frameRate ?? 0)")
        print("  Bitrate: \((settings?.videoBitrate ?? 0) / 1000) kbps")
    }
    
    deinit {
        optimizer?.stopMonitoring()
    }
}
```

## Common Issues

### Build Fails

**Problem**: Cannot find framework
```bash
# Solution: Rebuild
./build-ios.sh
```

**Problem**: Architecture mismatch
```bash
# Solution: Build for specific architecture
./build-ios.sh --arch arm64  # For device
./build-ios.sh --arch x86_64 # For simulator
```

### Runtime Issues

**Problem**: Camera not working
- Check permissions granted in Settings
- Verify Info.plist has NSCameraUsageDescription
- Try different camera position (.front or .back)

**Problem**: No audio
- Check microphone permission
- Verify NSMicrophoneUsageDescription in Info.plist
- Check audio session not interrupted

**Problem**: App crashes
- Check all frameworks embedded
- Verify deployment target is iOS 14.0+
- Check console for detailed error

## Next Steps

- See `ios/README.md` for detailed documentation
- Check `media/docs/IOS.md` for architecture details
- Explore simulcast, recording, and advanced features
- Review example projects in `ios/example/`

## Requirements

- iOS 14.0+
- Xcode 14.0+
- Swift 5.7+
- macOS 12.0+ (for building)

## Support

- GitHub Issues: https://github.com/turbonet/turbonet/issues
- Documentation: https://turbonet.dev/docs/ios
- Examples: `media/ios/example/`
