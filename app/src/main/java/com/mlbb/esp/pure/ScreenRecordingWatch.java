package com.mlbb.esp.pure;

import android.os.Build;
import android.os.Handler;
import android.view.WindowManager;

/** Android 15+ screen-recording signal. Loaded only on API 35 so older devices never resolve it. */
final class ScreenRecordingWatch {
    interface Listener {
        void onRecordingChanged(boolean recording);
    }

    private ScreenRecordingWatch() {}

    static Object register(WindowManager wm, Handler main, Listener listener) {
        if (Build.VERSION.SDK_INT < 35 || wm == null || listener == null) return null;
        WindowManager.ScreenRecordingCallback callback =
                state ->
                        listener.onRecordingChanged(
                                state == WindowManager.SCREEN_RECORDING_STATE_VISIBLE);
        try {
            int initial = wm.addScreenRecordingCallback(main::post, callback);
            listener.onRecordingChanged(initial == WindowManager.SCREEN_RECORDING_STATE_VISIBLE);
            return callback;
        } catch (Throwable ignored) {
            return null;
        }
    }

    static void unregister(WindowManager wm, Object callback) {
        if (Build.VERSION.SDK_INT < 35 || wm == null || !(callback instanceof WindowManager.ScreenRecordingCallback))
            return;
        try {
            wm.removeScreenRecordingCallback((WindowManager.ScreenRecordingCallback) callback);
        } catch (Throwable ignored) {
        }
    }
}
