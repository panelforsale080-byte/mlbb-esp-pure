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

/** Fetches expiry from pastebin raw URL; valid while local date (UTC) is on or before that day. */
public final class LicenseGate {
    public static final String KEY_URL = "https://pastebin.com/raw/5ZWQL5Zi";

    private static final Pattern DATE_LINE = Pattern.compile(
            "\\b(20\\d{2})\\s*[/\\-]\\s*(\\d{1,2})\\s*[/\\-]\\s*(\\d{1,2})\\b");

    public interface Callback {
        void onResult(boolean ok, String message);
    }

    private static final Executor IO = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private LicenseGate() {}

    public static void verifyAsync(Callback cb) {
        IO.execute(() -> {
            final Result r = fetchAndCompare();
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

    private static Result fetchAndCompare() {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(KEY_URL).openConnection();
            c.setConnectTimeout(12000);
            c.setReadTimeout(12000);
            c.setRequestMethod("GET");
            c.setRequestProperty(
                    "User-Agent",
                    "Xorel/1.0 (Android)");
            int code = c.getResponseCode();
            if (code != 200) {
                return new Result(false, "network");
            }
            StringBuilder sb = new StringBuilder();
            try (BufferedReader br =
                         new BufferedReader(new InputStreamReader(c.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) {
                    if (!sb.isEmpty()) sb.append('\n');
                    sb.append(line);
                }
            }
            Matcher m = DATE_LINE.matcher(sb);
            if (!m.find()) {
                return new Result(false, "invalid");
            }
            int y = Integer.parseInt(m.group(1));
            int mo = Integer.parseInt(m.group(2));
            int d = Integer.parseInt(m.group(3));
            Calendar end = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
            end.set(y, mo - 1, d, 23, 59, 59);
            end.set(Calendar.MILLISECOND, 999);

            Calendar now = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
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
}
