package com.turbonet.media;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.util.DisplayMetrics;
import android.view.Surface;

/**
 * Screen Capture using MediaProjection
 * 
 * Handles permission and VirtualDisplay creation
 */
public class ScreenCapture {
    static {
        System.loadLibrary("turbo_media_android");
    }
    
    private static final int REQUEST_CODE_SCREEN_CAPTURE = 1001;
    
    private Context context;
    private MediaProjectionManager projectionManager;
    private MediaProjection mediaProjection;
    private VirtualDisplay virtualDisplay;
    
    private long nativeHandle;
    private int width;
    private int height;
    private int dpi;
    
    private ScreenCaptureCallback callback;
    
    public interface ScreenCaptureCallback {
        void onPermissionGranted();
        void onPermissionDenied();
        void onStarted();
        void onStopped();
        void onError(String error);
    }
    
    public ScreenCapture(Context context) {
        this.context = context;
        this.projectionManager = (MediaProjectionManager) 
            context.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        
        // Get screen dimensions
        DisplayMetrics metrics = context.getResources().getDisplayMetrics();
        this.width = metrics.widthPixels;
        this.height = metrics.heightPixels;
        this.dpi = metrics.densityDpi;
        
        // Create native context
        nativeHandle = nativeCreate(width, height, 30);  /* 30 fps */
    }
    
    public void setCallback(ScreenCaptureCallback callback) {
        this.callback = callback;
    }
    
    /**
     * Request screen capture permission
     * 
     * Must be called from Activity
     */
    public void requestPermission(Activity activity) {
        Intent intent = projectionManager.createScreenCaptureIntent();
        activity.startActivityForResult(intent, REQUEST_CODE_SCREEN_CAPTURE);
    }
    
    /**
     * Handle permission result
     * 
     * Call from Activity.onActivityResult()
     */
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_CODE_SCREEN_CAPTURE) {
            return;
        }
        
        if (resultCode != Activity.RESULT_OK) {
            if (callback != null) {
                callback.onPermissionDenied();
            }
            return;
        }
        
        // Permission granted
        mediaProjection = projectionManager.getMediaProjection(resultCode, data);
        
        if (mediaProjection == null) {
            if (callback != null) {
                callback.onError("Failed to get MediaProjection");
            }
            return;
        }
        
        // Register callback
        mediaProjection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                stop();
            }
        }, null);
        
        if (callback != null) {
            callback.onPermissionGranted();
        }
    }
    
    /**
     * Start screen capture
     * 
     * Must be called after permission is granted
     */
    public boolean start() {
        try {
            return attachVirtualDisplayForTest() && startNativeForTest();
        } catch (Exception e) {
            if (callback != null) {
                callback.onError("Exception: " + e.getMessage());
            }
            return false;
        }
    }

    boolean attachVirtualDisplayForTest() {
        if (mediaProjection == null) {
            if (callback != null) {
                callback.onError("Permission not granted");
            }
            return false;
        }
        
        if (virtualDisplay != null) {
            return true;  /* Already started */
        }

        Surface surface = nativeGetSurface(nativeHandle);
        if (surface == null) {
            if (callback != null) {
                callback.onError("Failed to get surface");
            }
            return false;
        }

        virtualDisplay = mediaProjection.createVirtualDisplay(
            "TurboNetScreenCapture",
            width, height, dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            surface,
            null,  /* Callback */
            null   /* Handler */
        );

        if (virtualDisplay == null) {
            if (callback != null) {
                callback.onError("Failed to create VirtualDisplay");
            }
            return false;
        }

        return true;
    }

    boolean startNativeForTest() {
        if (virtualDisplay == null) {
            if (callback != null) {
                callback.onError("VirtualDisplay not attached");
            }
            return false;
        }

        if (!nativeStart(nativeHandle)) {
            virtualDisplay.release();
            virtualDisplay = null;
            if (callback != null) {
                callback.onError("Failed to start native screen capture");
            }
            return false;
        }

        if (callback != null) {
            callback.onStarted();
        }

        return true;
    }
    
    /**
     * Stop screen capture
     */
    public void stop() {
        boolean wasActive = virtualDisplay != null || mediaProjection != null;
        VirtualDisplay display = virtualDisplay;
        MediaProjection projection = mediaProjection;
        virtualDisplay = null;
        mediaProjection = null;

        if (display != null) {
            display.release();
        }
        
        if (projection != null) {
            projection.stop();
        }
        
        if (nativeHandle != 0) {
            nativeStop(nativeHandle);
        }
        
        if (wasActive && callback != null) {
            callback.onStopped();
        }
    }
    
    /**
     * Destroy screen capture
     */
    public void destroy() {
        stop();
        
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }
    
    /**
     * Check if currently capturing
     */
    public boolean isCapturing() {
        return virtualDisplay != null;
    }
    
    /**
     * Get screen dimensions
     */
    public int getWidth() {
        return width;
    }
    
    public int getHeight() {
        return height;
    }

    long getCapturedFrameCountForTest() {
        return nativeHandle != 0 ? nativeGetFrameCount(nativeHandle) : 0;
    }
    
    @Override
    protected void finalize() throws Throwable {
        destroy();
        super.finalize();
    }
    
    // Native methods
    private native long nativeCreate(int width, int height, int framerate);
    private native void nativeDestroy(long handle);
    private native Surface nativeGetSurface(long handle);
    private native boolean nativeStart(long handle);
    private native boolean nativeStop(long handle);
    private native long nativeGetFrameCount(long handle);
    
    // Static helper for request code
    public static int getRequestCode() {
        return REQUEST_CODE_SCREEN_CAPTURE;
    }
}
