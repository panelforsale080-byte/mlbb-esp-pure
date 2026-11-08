package com.mlbb.esp.pure;

import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Pastebin format (3 lines, UTF-8):
 * <pre>
 * PASSWORD
 * YYYY/MM/DD   (or YYYY-MM-DD)
 * hex_hmac_sha256   (see tools/sign_license.py)
 * </pre>
 * URL comes from native only — swapping Java constants is not enough.
 */
public final class LicenseGate {

    private static final Pattern DATE_LINE = Pattern.compile(
            "^\\s*(20\\d{2})\\s*[/\\-]\\s*(\\d{1,2})\\s*[/\\-]\\s*(\\d{1,2})\\s*$");

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

            final List<String> lines = new ArrayList<>();
            try (BufferedReader br =
                         new BufferedReader(
                                 new InputStreamReader(c.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) {
                    final String t = line.trim();
                    if (!t.isEmpty()) lines.add(t);
                }
            }
            if (lines.size() < 3) {
                return new Result(false, "invalid");
            }

            final String remotePass = lines.get(0);
            if (!constantTimeEquals(pass, remotePass)) {
                return new Result(false, "password");
            }

            final Matcher m = DATE_LINE.matcher(lines.get(1));
            if (!m.matches()) {
                return new Result(false, "invalid");
            }
            final int y = Integer.parseInt(m.group(1));
            final int mo = Integer.parseInt(m.group(2));
            final int d = Integer.parseInt(m.group(3));
            final String dateKey =
                    String.format(Locale.US, "%04d-%02d-%02d", y, mo, d);

            final String sigExpected = computeSignature(remotePass, dateKey);
            final String sigRemote = lines.get(2).trim().toLowerCase(Locale.US);
            if (!constantTimeEquals(sigExpected, sigRemote)) {
                return new Result(false, "signature");
            }

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

    private static String computeSignature(String password, String dateYmd) throws Exception {
        final String pepper = LicenseNative.nativeGetHmacPepper();
        final String pasteId = LicenseNative.nativeGetBoundPasteId();
        final String msg = pepper + "|" + password + "|" + dateYmd + "|" + pasteId;
        final byte[] key = LicenseNative.nativeGetHmacKey();
        if (key == null || key.length == 0) throw new IllegalStateException("key");
        final Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        final byte[] out = mac.doFinal(msg.getBytes(StandardCharsets.UTF_8));
        return bytesToHex(out);
    }

    private static String bytesToHex(byte[] b) {
        final StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) sb.append(String.format(Locale.US, "%02x", x));
        return sb.toString();
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
