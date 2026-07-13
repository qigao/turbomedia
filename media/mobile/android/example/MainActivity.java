package com.turbonet.media.example;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import com.turbonet.media.MediaEngine;
import com.turbonet.media.Simulcast;
import com.turbonet.media.Recorder;
import com.turbonet.media.MobileOptimizer;

/**
 * TurboNet Media Example App
 * 
 * Demonstrates video conferencing on Android
 */
public class MainActivity extends Activity {
    
    private MediaEngine mediaEngine;
    private Simulcast simulcast;
    private Recorder recorder;
    private MobileOptimizer optimizer;
    
    private long videoTrack;
    private long audioTrack;
    
    private TextView statusText;
    private Button startButton;
    private Button stopButton;
    private Button recordButton;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        
        statusText = findViewById(R.id.status_text);
        startButton = findViewById(R.id.start_button);
        stopButton = findViewById(R.id.stop_button);
        recordButton = findViewById(R.id.record_button);
        
        // Initialize media engine
        initializeMediaEngine();
        
        // Setup buttons
        startButton.setOnClickListener(v -> startCall());
        stopButton.setOnClickListener(v -> stopCall());
        recordButton.setOnClickListener(v -> toggleRecording());
        
        stopButton.setEnabled(false);
        recordButton.setEnabled(false);
    }
    
    private void initializeMediaEngine() {
        try {
            // Create media engine
            mediaEngine = new MediaEngine();
            
            // Create mobile optimizer
            optimizer = new MobileOptimizer(this, 0);  // TODO: Pass actual handle
            
            // Auto-optimize for current conditions
            optimizer.autoOptimize();
            
            // Get recommended settings
            MobileOptimizer.RecommendedSettings settings = optimizer.getRecommendedSettings();
            
            updateStatus("Initialized\n" +
                        "Network: " + detectNetwork() + "\n" +
                        "Recommended: " + settings.maxResolution + " @ " + 
                        settings.maxFramerate + "fps, " + 
                        (settings.maxBitrate / 1000) + " kbps");
            
        } catch (Exception e) {
            Toast.makeText(this, "Failed to initialize: " + e.getMessage(), 
                          Toast.LENGTH_LONG).show();
        }
    }
    
    private void startCall() {
        try {
            // Get recommended settings
            MobileOptimizer.RecommendedSettings settings = optimizer.getRecommendedSettings();
            
            // Parse resolution
            String[] res = settings.maxResolution.split("x");
            int width = Integer.parseInt(res[0]);
            int height = Integer.parseInt(res[1]);
            
            // Add video track with simulcast
            simulcast = new Simulcast(width, height, settings.maxFramerate, "h264");
            simulcast.setBandwidth(settings.maxBitrate);
            
            videoTrack = mediaEngine.addVideoTrack(
                width, height, 
                settings.maxFramerate,
                settings.maxBitrate,
                "h264"
            );
            
            // Add audio track
            audioTrack = mediaEngine.addAudioTrack(48000, 2, 64000);
            
            // Start tracks
            mediaEngine.startTrack(videoTrack);
            mediaEngine.startTrack(audioTrack);
            
            updateStatus("Call started\n" +
                        "Video: " + width + "x" + height + " @ " + 
                        settings.maxFramerate + "fps\n" +
                        "Audio: 48kHz stereo");
            
            startButton.setEnabled(false);
            stopButton.setEnabled(true);
            recordButton.setEnabled(true);
            
        } catch (Exception e) {
            Toast.makeText(this, "Failed to start: " + e.getMessage(), 
                          Toast.LENGTH_LONG).show();
        }
    }
    
    private void stopCall() {
        try {
            // Stop tracks
            if (videoTrack != 0) {
                mediaEngine.stopTrack(videoTrack);
            }
            if (audioTrack != 0) {
                mediaEngine.stopTrack(audioTrack);
            }
            
            // Cleanup simulcast
            if (simulcast != null) {
                simulcast.destroy();
                simulcast = null;
            }
            
            // Stop recording if active
            if (recorder != null) {
                recorder.stop();
                recorder.destroy();
                recorder = null;
            }
            
            updateStatus("Call stopped");
            
            startButton.setEnabled(true);
            stopButton.setEnabled(false);
            recordButton.setEnabled(false);
            
        } catch (Exception e) {
            Toast.makeText(this, "Failed to stop: " + e.getMessage(), 
                          Toast.LENGTH_LONG).show();
        }
    }
    
    private void toggleRecording() {
        if (recorder == null) {
            startRecording();
        } else {
            stopRecording();
        }
    }
    
    private void startRecording() {
        try {
            // Create recorder
            String filename = getExternalFilesDir(null) + "/recording.mp4";
            recorder = new Recorder(filename, Recorder.Format.MP4);
            
            // Add tracks
            MobileOptimizer.RecommendedSettings settings = optimizer.getRecommendedSettings();
            String[] res = settings.maxResolution.split("x");
            int width = Integer.parseInt(res[0]);
            int height = Integer.parseInt(res[1]);
            
            recorder.addVideoTrack(width, height, settings.maxFramerate, 
                                  Recorder.Codec.H264);
            recorder.addAudioTrack(48000, 2, Recorder.Codec.OPUS);
            
            // Start recording
            recorder.start();
            
            recordButton.setText("Stop Recording");
            updateStatus("Recording to: " + filename);
            
        } catch (Exception e) {
            Toast.makeText(this, "Failed to start recording: " + e.getMessage(), 
                          Toast.LENGTH_LONG).show();
        }
    }
    
    private void stopRecording() {
        try {
            if (recorder != null) {
                recorder.stop();
                recorder.destroy();
                recorder = null;
            }
            
            recordButton.setText("Start Recording");
            updateStatus("Recording stopped");
            
        } catch (Exception e) {
            Toast.makeText(this, "Failed to stop recording: " + e.getMessage(), 
                          Toast.LENGTH_LONG).show();
        }
    }
    
    private String detectNetwork() {
        android.net.ConnectivityManager cm = 
            (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        android.net.NetworkInfo info = cm.getActiveNetworkInfo();
        
        if (info == null || !info.isConnected()) {
            return "No connection";
        }
        
        if (info.getType() == android.net.ConnectivityManager.TYPE_WIFI) {
            return "WiFi";
        }
        
        if (info.getType() == android.net.ConnectivityManager.TYPE_MOBILE) {
            return "Mobile " + info.getSubtypeName();
        }
        
        return "Unknown";
    }
    
    private void updateStatus(String status) {
        runOnUiThread(() -> statusText.setText(status));
    }
    
    @Override
    protected void onDestroy() {
        super.onDestroy();
        
        // Cleanup
        if (mediaEngine != null) {
            mediaEngine.destroy();
        }
        if (simulcast != null) {
            simulcast.destroy();
        }
        if (recorder != null) {
            recorder.destroy();
        }
    }
}
