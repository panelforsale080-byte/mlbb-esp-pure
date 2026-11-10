package com.mlbb.esp.pure;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;

/** Lists and plays .mp3 files. The repo URL is pinned in the native library. */
final class MusicController {
    private static final String TAG = "XorelMusic";

    private static Context appContext;
    private static final Object lock = new Object();
    private static final List<String> names = new ArrayList<>();
    private static final List<String> paths = new ArrayList<>();
    private static String status = "Check music available";
    private static int progress = -1;
    private static boolean downloading;
    private static boolean playing;
    private static boolean muted;
    private static int volumePercent = 70;
    private static int generation;
    private static MediaPlayer player;

    private MusicController() {}

    static void attach(Context context) {
        if (context != null) appContext = context.getApplicationContext();
    }

    static void refresh() {
        final int gen;
        synchronized (lock) {
            gen = ++generation;
            downloading = false;
            progress = -1;
            status = "Checking music...";
        }
        new Thread(() -> fetchList(gen), "xorel-music-list").start();
    }

    static void play(int index) {
        final String path;
        final String name;
        synchronized (lock) {
            if (index < 0 || index >= paths.size() || downloading) return;
            path = paths.get(index);
            name = names.get(index);
        }
        final int gen;
        synchronized (lock) {
            gen = ++generation;
        }
        new Thread(() -> downloadAndPlay(gen, path, name), "xorel-music-dl").start();
    }

    static void setVolume(int percent) {
        int v = Math.max(0, Math.min(100, percent));
        synchronized (lock) {
            volumePercent = v;
        }
        applyVolume();
    }

    static void setMuted(boolean on) {
        synchronized (lock) {
            muted = on;
        }
        applyVolume();
    }

    static void stop() {
        synchronized (lock) {
            generation++;
            downloading = false;
            progress = -1;
            playing = false;
            status = names.isEmpty() ? "No music available" : "Stopped";
        }
        releasePlayer();
    }

    static String snapshot() {
        synchronized (lock) {
            StringBuilder sb = new StringBuilder();
            sb.append(progress)
                    .append('\t')
                    .append(volumePercent)
                    .append('\t')
                    .append(muted ? 1 : 0)
                    .append('\t')
                    .append(playing ? 1 : 0)
                    .append('\t')
                    .append(downloading ? 1 : 0)
                    .append('\t')
                    .append(status == null ? "" : status.replace('\n', ' '))
                    .append('\n');
            for (String name : names) {
                sb.append(name.replace('\n', ' ')).append('\n');
            }
            return sb.toString();
        }
    }

    private static void fetchList(int gen) {
        HttpURLConnection conn = null;
        try {
            conn = open(requireUrl(LicenseNative.nativeMusicTreeUrl()));
            String body = readAll(conn);
            JSONArray tree = new JSONObject(body).getJSONArray("tree");
            List<String> nextNames = new ArrayList<>();
            List<String> nextPaths = new ArrayList<>();
            for (int i = 0; i < tree.length(); i++) {
                JSONObject item = tree.getJSONObject(i);
                if (!"blob".equals(item.optString("type"))) continue;
                String path = item.optString("path", "");
                if (!path.toLowerCase(Locale.US).endsWith(".mp3")) continue;
                int slash = path.lastIndexOf('/');
                String name = slash >= 0 ? path.substring(slash + 1) : path;
                nextNames.add(name);
                nextPaths.add(path);
            }
            synchronized (lock) {
                if (gen != generation) return;
                names.clear();
                paths.clear();
                names.addAll(nextNames);
                paths.addAll(nextPaths);
                status = names.isEmpty() ? "No music available" : names.size() + " tracks";
            }
        } catch (Throwable t) {
            Log.w(TAG, "list", t);
            synchronized (lock) {
                if (gen != generation) return;
                status = "Could not check music";
            }
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static void downloadAndPlay(int gen, String path, String name) {
        File dest = localFile(path);
        if (dest == null) {
            synchronized (lock) {
                if (gen == generation) status = "Music not ready";
            }
            return;
        }
        if (dest.exists() && dest.length() > 1024) {
            if (!startPlayer(gen, dest, name)) {
                synchronized (lock) {
                    if (gen == generation) status = "Playback failed";
                }
            }
            return;
        }
        synchronized (lock) {
            if (gen != generation) return;
            downloading = true;
            progress = 0;
            playing = false;
            status = "Please wait downloading 0%";
        }
        HttpURLConnection conn = null;
        try {
            conn = open(rawUrl(path));
            int length = conn.getContentLength();
            InputStream in = conn.getInputStream();
            File tmp = new File(dest.getParentFile(), dest.getName() + ".part");
            FileOutputStream out = new FileOutputStream(tmp);
            byte[] buf = new byte[8192];
            int read;
            int got = 0;
            int lastPct = 0;
            while ((read = in.read(buf)) != -1) {
                synchronized (lock) {
                    if (gen != generation) {
                        out.close();
                        in.close();
                        tmp.delete();
                        return;
                    }
                }
                out.write(buf, 0, read);
                got += read;
                int pct = length > 0 ? Math.min(99, (int) ((got * 100L) / length)) : -1;
                if (pct != lastPct) {
                    lastPct = pct;
                    synchronized (lock) {
                        if (gen != generation) {
                            out.close();
                            in.close();
                            tmp.delete();
                            return;
                        }
                        progress = pct;
                        status =
                                pct >= 0
                                        ? "Please wait downloading " + pct + "%"
                                        : "Please wait downloading...";
                    }
                }
            }
            out.close();
            in.close();
            if (dest.exists()) dest.delete();
            if (!tmp.renameTo(dest)) {
                throw new IllegalStateException("rename");
            }
            synchronized (lock) {
                if (gen != generation) return;
                downloading = false;
                progress = 100;
            }
            if (!startPlayer(gen, dest, name)) {
                synchronized (lock) {
                    if (gen == generation) status = "Playback failed";
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "download", t);
            synchronized (lock) {
                if (gen != generation) return;
                downloading = false;
                progress = -1;
                status = "Download failed";
            }
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static boolean startPlayer(int gen, File file, String name) {
        releasePlayer();
        try {
            MediaPlayer mp = new MediaPlayer();
            mp.setAudioAttributes(
                    new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build());
            mp.setDataSource(file.getAbsolutePath());
            mp.setLooping(true);
            mp.prepare();
            mp.start();
            synchronized (lock) {
                if (gen != generation) {
                    mp.release();
                    return false;
                }
                player = mp;
                playing = true;
                downloading = false;
                progress = -1;
                status = "Playing " + name;
            }
            applyVolume();
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "play", t);
            return false;
        }
    }

    private static void applyVolume() {
        float v;
        synchronized (lock) {
            v = muted ? 0f : volumePercent / 100f;
            if (player == null) return;
            try {
                player.setVolume(v, v);
            } catch (Throwable ignored) {
            }
        }
    }

    private static void releasePlayer() {
        MediaPlayer mp;
        synchronized (lock) {
            mp = player;
            player = null;
            playing = false;
        }
        if (mp == null) return;
        try {
            mp.stop();
        } catch (Throwable ignored) {
        }
        try {
            mp.release();
        } catch (Throwable ignored) {
        }
    }

    private static File localFile(String path) {
        if (appContext == null) return null;
        File dir = new File(appContext.getFilesDir(), "music");
        if (!dir.exists()) dir.mkdirs();
        String safe = path.replace('/', '_').replace('\\', '_');
        return new File(dir, safe);
    }

    private static String rawUrl(String path) throws Exception {
        String[] parts = path.split("/");
        StringBuilder sb = new StringBuilder(requireUrl(LicenseNative.nativeMusicRawBase()));
        for (int i = 0; i < parts.length; i++) {
            if (i > 0) sb.append('/');
            sb.append(URLEncoder.encode(parts[i], "UTF-8").replace("+", "%20"));
        }
        return sb.toString();
    }

    private static String requireUrl(String url) {
        if (url == null || url.isEmpty()) throw new IllegalStateException("music");
        return url;
    }

    private static HttpURLConnection open(String url) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(15000);
        conn.setReadTimeout(30000);
        conn.setInstanceFollowRedirects(true);
        conn.setRequestProperty("User-Agent", "Xorel");
        conn.connect();
        int code = conn.getResponseCode();
        if (code >= 400) throw new IllegalStateException("HTTP " + code);
        return conn;
    }

    private static String readAll(HttpURLConnection conn) throws Exception {
        InputStream in = conn.getInputStream();
        byte[] buf = new byte[4096];
        StringBuilder sb = new StringBuilder();
        int n;
        while ((n = in.read(buf)) != -1) sb.append(new String(buf, 0, n, "UTF-8"));
        in.close();
        return sb.toString();
    }
}
