package com.mlbb.esp.pure;

import android.os.Build;
import android.os.Handler;
import android.view.WindowManager;
import java.lang.reflect.Proxy;
import java.util.concurrent.Executor;

/**
 * Android 15 screen-recording signal via reflection.
 * The platform stub in CI does not always expose ScreenRecordingCallback.
 */
final class ScreenRecordingWatch {
    interface Listener {
        void onRecordingChanged(boolean recording);
    }

    private static final int STATE_VISIBLE = 1;

    private ScreenRecordingWatch() {}

    static Object register(WindowManager wm, Handler main, Listener listener) {
        if (Build.VERSION.SDK_INT < 35 || wm == null || listener == null) return null;
        try {
            Class<?> callbackClass = Class.forName("android.view.WindowManager$ScreenRecordingCallback");
            Object callback =
                    Proxy.newProxyInstance(
                            callbackClass.getClassLoader(),
                            new Class<?>[] {callbackClass},
                            (proxy, method, args) -> {
                                if ("onScreenRecordingStateChanged".equals(method.getName())
                                        && args != null
                                        && args.length > 0
                                        && args[0] instanceof Integer) {
                                    listener.onRecordingChanged(((Integer) args[0]) == STATE_VISIBLE);
                                }
                                return null;
                            });
            Executor executor = main::post;
            Object initial =
                    wm.getClass()
                            .getMethod("addScreenRecordingCallback", Executor.class, callbackClass)
                            .invoke(wm, executor, callback);
            if (initial instanceof Integer) {
                listener.onRecordingChanged(((Integer) initial) == STATE_VISIBLE);
            }
            return callback;
        } catch (Throwable ignored) {
            return null;
        }
    }

    static void unregister(WindowManager wm, Object callback) {
        if (Build.VERSION.SDK_INT < 35 || wm == null || callback == null) return;
        try {
            Class<?> callbackClass = Class.forName("android.view.WindowManager$ScreenRecordingCallback");
            wm.getClass()
                    .getMethod("removeScreenRecordingCallback", callbackClass)
                    .invoke(wm, callback);
        } catch (Throwable ignored) {
        }
    }
}
