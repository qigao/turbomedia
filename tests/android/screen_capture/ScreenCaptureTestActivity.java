package com.turbonet.media;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;

public final class ScreenCaptureTestActivity extends Activity
        implements ScreenCapture.ScreenCaptureCallback {
    private static final String TAG = "TurboMediaScreenTest";
    private static final long FRAME_TIMEOUT_MS = 5000;
    private static final long POLL_INTERVAL_MS = 100;
    private static final long LIFECYCLE_QUIET_MS = 500;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private ScreenCapture capture;
    private TextView statusView;
    private long deadlineMs;
    private long stoppedFrameCount;
    private boolean finished;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        statusView = new TextView(this);
        statusView.setBackgroundColor(Color.WHITE);
        statusView.setTextColor(Color.BLACK);
        statusView.setGravity(Gravity.CENTER);
        statusView.setTextSize(20);
        statusView.setText("Waiting for screen-capture permission…");
        setContentView(statusView);

        capture = new ScreenCapture(this);
        capture.setCallback(this);
        capture.requestPermission(this);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        capture.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onPermissionGranted() {
        statusView.setText("Permission granted; checking the pre-start gate…");
        try {
            if (!capture.attachVirtualDisplayForTest()) {
                fail("VirtualDisplay attachment returned false");
                return;
            }
        } catch (Exception error) {
            fail("VirtualDisplay attachment failed: " + error.getMessage());
            return;
        }

        deadlineMs = android.os.SystemClock.uptimeMillis() + LIFECYCLE_QUIET_MS;
        handler.post(this::verifyPreStartGate);
    }

    @Override
    public void onPermissionDenied() {
        fail("MediaProjection permission denied");
    }

    @Override
    public void onStarted() {
        Log.i(TAG, "VirtualDisplay and native capture started");
    }

    @Override
    public void onStopped() {
        Log.i(TAG, "Screen capture stopped");
    }

    @Override
    public void onError(String error) {
        fail(error);
    }

    private void pollFrameCount() {
        if (finished) {
            return;
        }

        long frameCount = capture.getCapturedFrameCountForTest();
        if (frameCount > 0) {
            capture.stop();
            stoppedFrameCount = capture.getCapturedFrameCountForTest();
            deadlineMs = android.os.SystemClock.uptimeMillis() + LIFECYCLE_QUIET_MS;
            statusView.setText("Frame received; checking the post-stop gate…");
            handler.post(this::verifyPostStopGate);
            return;
        }

        if (android.os.SystemClock.uptimeMillis() >= deadlineMs) {
            fail("No native screen frame arrived within " + FRAME_TIMEOUT_MS + " ms");
            return;
        }

        handler.postDelayed(this::pollFrameCount, POLL_INTERVAL_MS);
    }

    private void verifyPreStartGate() {
        if (finished) {
            return;
        }

        long frameCount = capture.getCapturedFrameCountForTest();
        if (frameCount != 0) {
            fail("Frames arrived before native start: " + frameCount);
            return;
        }

        if (android.os.SystemClock.uptimeMillis() < deadlineMs) {
            handler.postDelayed(this::verifyPreStartGate, POLL_INTERVAL_MS);
            return;
        }

        try {
            if (!capture.startNativeForTest()) {
                fail("Native screen capture start returned false");
                return;
            }
        } catch (Exception error) {
            fail("Native screen capture start failed: " + error.getMessage());
            return;
        }
        statusView.setText("Native capture started; waiting for an I420 frame…");
        deadlineMs = android.os.SystemClock.uptimeMillis() + FRAME_TIMEOUT_MS;
        handler.post(this::pollFrameCount);
    }

    private void verifyPostStopGate() {
        if (finished) {
            return;
        }

        long frameCount = capture.getCapturedFrameCountForTest();
        if (frameCount != stoppedFrameCount) {
            fail("Frames arrived after native stop: " + stoppedFrameCount + " -> " + frameCount);
            return;
        }

        if (android.os.SystemClock.uptimeMillis() < deadlineMs) {
            handler.postDelayed(this::verifyPostStopGate, POLL_INTERVAL_MS);
            return;
        }

        pass(stoppedFrameCount);
    }

    private void pass(long frameCount) {
        if (finished) {
            return;
        }
        finished = true;
        String result = "PASS frames=" + frameCount;
        statusView.setText(result);
        Log.i(TAG, result);
        capture.destroy();
        capture = null;
    }

    private void fail(String reason) {
        if (finished) {
            return;
        }
        finished = true;
        String result = "FAIL " + reason;
        statusView.setText(result);
        Log.e(TAG, result);
        if (capture != null) {
            capture.destroy();
            capture = null;
        }
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        if (capture != null) {
            capture.destroy();
            capture = null;
        }
        super.onDestroy();
    }
}
