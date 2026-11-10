package com.mlbb.esp.pure;

import android.os.Handler;
import android.os.Looper;

import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

/** Asks the native library to check the password. The license URL is not chosen here. */
public final class LicenseGate {

    public interface Callback {
        void onResult(boolean ok, String message);
    }

    private static final Executor IO = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private LicenseGate() {}

    public static void verifyAsync(String userPassword, Callback cb) {
        IO.execute(() -> {
            final String pass = userPassword == null ? "" : userPassword.trim();
            String msg = "password";
            if (!pass.isEmpty()) {
                try {
                    msg = LicenseNative.nativeVerifyLicense(pass);
                } catch (Throwable t) {
                    msg = "network";
                }
                if (msg == null) msg = "network";
            }
            final String message = msg;
            final boolean ok = "ok".equals(message);
            MAIN.post(() -> cb.onResult(ok, message));
        });
    }
}
