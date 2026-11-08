package com.mlbb.esp.pure;

import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Calendar;
import java.util.TimeZone;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * GitHub raw file, one line:
 * <pre>
 * PASSWORD YYYY/MM/DD
 * </pre>
 * A trailing hex token is ignored so older signed lines still match on password + date.
 */
public final class LicenseGate {

    private static final Pattern ONE_LINE = Pattern.compile(
            "^(\\S+)\\s+(20\\d{2})[/\\-](\\d{1,2})[/\\-](\\d{1,2})(?:\\s+\\S+)?\\s*$");

    public interface Callback {
        void onResult(boolean ok, String message);
    }

    private static final Executor IO = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private LicenseGate() {}

    public static void verifyAsync(String userPassword, Callback cb) {
        IO.execute(() -> {
            final Result r = fetchAndCompare(userPassword);
            MAIN.post(() -> {
                LicenseNative.nativeSetLicenseAttested(r.ok);
                cb.onResult(r.ok, r.message);
            });
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

            final Matcher m = ONE_LINE.matcher(bodyLine);
            if (!m.matches()) {
                return new Result(false, "invalid");
            }

            final String remotePass = m.group(1);
            if (!constantTimeEquals(pass, remotePass)) {
                return new Result(false, "password");
            }

            final int y = Integer.parseInt(m.group(2));
            final int mo = Integer.parseInt(m.group(3));
            final int d = Integer.parseInt(m.group(4));

            final Calendar end = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
            end.set(y, mo - 1, d, 23, 59, 59);
            end.set(Calendar.MILLISECOND, 999);

            final Calendar now = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
            if (now.after(end)) {
                return new Result(false, "expired");
            }
            return new Result(true, "ok");
        } catch (Exception e) {
            return new Result(false, "network");
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static boolean constantTimeEquals(String a, String b) {
        if (a == null || b == null) return false;
        final byte[] x = a.getBytes(StandardCharsets.UTF_8);
        final byte[] y = b.getBytes(StandardCharsets.UTF_8);
        if (x.length != y.length) return false;
        int diff = 0;
        for (int i = 0; i < x.length; ++i) diff |= x[i] ^ y[i];
        return diff == 0;
    }
}
