# TurboNet Media Android - Quick Start Guide

Get up and running with TurboNet Media on Android in 5 minutes.

## Step 1: Build the Library (Choose One)

### Option A: Using Build Script (Easiest)

**Windows**:
```cmd
cd media\android
build-android.bat
```

**Linux/Mac**:
```bash
cd media/android
chmod +x build-android.sh
./build-android.sh
```

### Option B: Using Gradle

```bash
cd media/android
./gradlew assembleRelease
```

### Option C: Using CMake Directly

```bash
cd media/android
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26

cmake --build build -j8
```

## Step 2: Add to Your Android Project

### Method 1: As a Module (Recommended)

**settings.gradle**:
```groovy
include ':app', ':turbonet-media'
project(':turbonet-media').projectDir = new File('../media/android')
```

**app/build.gradle**:
```groovy
dependencies {
    implementation project(':turbonet-media')
}
```

### Method 2: Copy Native Libraries

```bash
# Copy .so files to your project
cp -r install/*/lib/* app/src/main/jniLibs/
```

**app/build.gradle**:
```groovy
android {
    sourceSets {
        main {
            jniLibs.srcDirs = ['src/main/jniLibs']
        }
    }
}
```

## Step 3: Add Permissions

**AndroidManifest.xml**:
```xml
<manifest>
    <uses-permission android:name="android.permission.CAMERA" />
    <uses-permission android:name="android.permission.RECORD_AUDIO" />
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
</manifest>
```

## Step 4: Request Runtime Permissions

**MainActivity.java**:
```java
import android.Manifest;
import android.content.pm.PackageManager;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

public class MainActivity extends AppCompatActivity {
    private static final int PERMISSION_REQUEST = 100;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        
        // Request permissions
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                new String[]{
                    Manifest.permission.CAMERA,
                    Manifest.permission.RECORD_AUDIO
                },
                PERMISSION_REQUEST);
        } else {
            startVideoCall();
        }
    }
    
    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                startVideoCall();
            }
        }
    }
    
    private void startVideoCall() {
        // Your video call code here
    }
}
```

## Step 5: Use the Library

### Simple Video Call

```java
import com.turbonet.media.MediaEngine;

public class VideoCallActivity extends AppCompatActivity {
    private MediaEngine engine;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // Initialize engine
        engine = new MediaEngine();
        engine.initialize();
        
        // Configure video
        engine.setVideoSource("camera:1"); // Back camera
        engine.setVideoCodec("H264", 1280, 720, 30, 1500000);
        
        // Configure audio
        engine.setAudioSource("audio:default");
        engine.setAudioCodec("OPUS", 48000, 2, 64000);
        
        // Set callback for frames
        engine.setFrameCallback(new MediaEngine.FrameCallback() {
            @Override
            public void onVideoFrame(byte[] data, int width, int height, long timestamp) {
                // Send via network or display locally
                Log.d("Video", "Frame: " + width + "x" + height);
            }
            
            @Override
            public void onAudioFrame(byte[] data, int samples, long timestamp) {
                // Send via network or play locally
                Log.d("Audio", "Samples: " + samples);
            }
        });
        
        // Start capture
        engine.start();
    }
    
    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (engine != null) {
            engine.stop();
            engine.destroy();
        }
    }
}
```

### With Mobile Optimizations

```java
import com.turbonet.media.MediaEngine;
import com.turbonet.media.MobileOptimizer;

public class OptimizedCallActivity extends AppCompatActivity {
    private MediaEngine engine;
    private MobileOptimizer optimizer;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // Create optimizer
        optimizer = new MobileOptimizer();
        optimizer.startBatteryMonitoring(this);
        optimizer.startNetworkMonitoring(this);
        
        // Get recommended settings
        int bitrate = optimizer.getTargetBitrate();
        int fps = optimizer.getTargetFps();
        int scale = optimizer.getResolutionScale();
        
        // Initialize engine with optimized settings
        engine = new MediaEngine();
        engine.initialize();
        
        int width = 1280 * scale / 100;
        int height = 720 * scale / 100;
        
        engine.setVideoSource("camera:1");
        engine.setVideoCodec("H264", width, height, fps, bitrate);
        engine.setAudioSource("audio:default");
        engine.setAudioCodec("OPUS", 48000, 2, 64000);
        
        engine.start();
        
        Log.i("Optimizer", "Power mode: " + optimizer.getPowerModeString());
        Log.i("Optimizer", "Settings: " + width + "x" + height + " @ " + fps + "fps, " + bitrate + " bps");
    }
    
    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (engine != null) {
            engine.stop();
            engine.destroy();
        }
        if (optimizer != null) {
            optimizer.stopMonitoring();
        }
    }
}
```

### Recording a Call

```java
import com.turbonet.media.Recorder;

// Create recorder
String outputPath = getExternalFilesDir(null) + "/call_recording.mp4";
Recorder recorder = new Recorder();
recorder.initialize(outputPath, Recorder.Format.MP4);

// Add tracks
recorder.addVideoTrack("H264", 1280, 720, 30, 1500000);
recorder.addAudioTrack("OPUS", 48000, 2, 64000);

// Start recording
recorder.start();

// In your frame callback:
engine.setFrameCallback(new MediaEngine.FrameCallback() {
    @Override
    public void onVideoFrame(byte[] data, int width, int height, long timestamp) {
        recorder.writeVideoFrame(data, data.length, timestamp, isKeyframe);
    }
    
    @Override
    public void onAudioFrame(byte[] data, int samples, long timestamp) {
        recorder.writeAudioFrame(data, data.length, timestamp);
    }
});

// Stop recording
recorder.stop();
recorder.destroy();
```

## Common Issues

### Build Fails

**Problem**: CMake can't find Android NDK
```bash
# Solution: Set environment variable
export ANDROID_NDK=/path/to/ndk
# or
set ANDROID_NDK=C:\path\to\ndk
```

**Problem**: Gradle sync fails
```bash
# Solution: Clean and refresh
./gradlew clean
./gradlew --refresh-dependencies
```

### Runtime Issues

**Problem**: Camera not working
- Check permissions are granted at runtime
- Try different camera ID (0 for front, 1 for back)
- Check if another app is using the camera

**Problem**: No audio
- Check RECORD_AUDIO permission granted
- Check microphone not muted
- Try different sample rate (44100 or 48000)

**Problem**: App crashes on start
- Check all .so files are in jniLibs
- Check ABI matches device (arm64-v8a for most modern devices)
- Check logcat for detailed error

## Next Steps

- See `android/example/MainActivity.java` for complete working example
- Read `android/README.md` for detailed documentation
- Check `media/docs/ANDROID.md` for architecture details
- Explore simulcast, SFU, and advanced features

## Support

- GitHub Issues: https://github.com/turbonet/turbonet/issues
- Documentation: https://turbonet.dev/docs
- Examples: `media/android/example/`

## Requirements

- Android 8.0+ (API 26+)
- Android NDK r25+
- Gradle 7.0+
- CMake 3.18.1+

## License

See LICENSE file in project root.
