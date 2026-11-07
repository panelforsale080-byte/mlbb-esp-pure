package com.mlbb.esp.pure;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.graphics.Path;
import android.util.Log;
import android.view.accessibility.AccessibilityEvent;

/**
 * Injects Retribution tap (EXTERNAL-MLBB Touch_Tap equivalent). Enable in Accessibility settings once.
 */
public final class RetriGestureService extends AccessibilityService {
    private static final String TAG = "XorelRetriA11y";
    private static final int TAP_MS = 48;

    private static volatile RetriGestureService instance;

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        instance = this;
        Log.i(TAG, "connected");
    }

    @Override
    public void onDestroy() {
        instance = null;
        super.onDestroy();
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {}

    @Override
    public void onInterrupt() {}

    public static boolean ready() {
        return instance != null;
    }

    public static boolean dispatchTap(float x, float y) {
        RetriGestureService svc = instance;
        if (svc == null) return false;
        Path path = new Path();
        path.moveTo(x, y);
        path.lineTo(x, y);
        GestureDescription.StrokeDescription stroke =
                new GestureDescription.StrokeDescription(path, 0, TAP_MS);
        GestureDescription gesture = new GestureDescription.Builder().addStroke(stroke).build();
        boolean ok = svc.dispatchGesture(gesture, null, null);
        if (!ok) Log.w(TAG, "dispatchGesture failed");
        return ok;
    }
}
