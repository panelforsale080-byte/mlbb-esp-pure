package com.mlbb.esp.pure;

import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

/** Fetches the license file. Password and expiry are checked inside the native library. */
public final class LicenseGate {

    public interface Callback {
        void onResult(boolean ok, String message);
    }

    private static final Executor IO = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private LicenseGate() {}

    public static void verifyAsync(String userPassword, Callback cb) {
        IO.execute(() -> {
            final Result r = fetchAndCompare(userPassword);
            MAIN.post(() -> cb.onResult(r.ok, r.message));
        });
    }

    private static final class Result {
        final boolean ok;
        final String message;

        Result(boolean ok, String message) {
            this.ok = ok;
            this.message = message;
        }
    }

    private static Result fetchAndCompare(String userPassword) {
        if (userPassword == null || userPassword.trim().isEmpty()) {
            return new Result(false, "password");
        }
        final String pass = userPassword.trim();

        HttpURLConnection c = null;
        try {
            final String url = LicenseNative.nativeGetFetchUrl();
            if (url == null || url.isEmpty()) return new Result(false, "invalid");

            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(12000);
            c.setReadTimeout(12000);
            c.setRequestMethod("GET");
            c.setRequestProperty("User-Agent", "Xorel/2.0 (Android)");
            final int code = c.getResponseCode();
            if (code != 200) {
                return new Result(false, "network");
            }

            String bodyLine = null;
            try (BufferedReader br =
                         new BufferedReader(
                                 new InputStreamReader(c.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) {
                    final String t = line.trim();
                    if (!t.isEmpty()) {
                        bodyLine = t;
                        break;
                    }
                }
            }
            if (bodyLine == null) {
                return new Result(false, "invalid");
            }

            final String msg = LicenseNative.nativeSubmitLicense(pass, bodyLine);
            return new Result("ok".equals(msg), msg == null ? "invalid" : msg);
        } catch (Exception e) {
            return new Result(false, "network");
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
