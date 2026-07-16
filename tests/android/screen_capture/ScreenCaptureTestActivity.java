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

    private final Handler handler = new Handler(Looper.getMainLooper());
    private ScreenCapture capture;
    private TextView statusView;
    private long deadlineMs;
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
        statusView.setText("Permission granted; waiting for a native I420 frame…");
        if (!capture.start()) {
            fail("ScreenCapture.start returned false");
            return;
        }

        deadlineMs = android.os.SystemClock.uptimeMillis() + FRAME_TIMEOUT_MS;
        handler.post(this::pollFrameCount);
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
            pass(frameCount);
            return;
        }

        if (android.os.SystemClock.uptimeMillis() >= deadlineMs) {
            fail("No native screen frame arrived within " + FRAME_TIMEOUT_MS + " ms");
            return;
        }

        handler.postDelayed(this::pollFrameCount, POLL_INTERVAL_MS);
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
