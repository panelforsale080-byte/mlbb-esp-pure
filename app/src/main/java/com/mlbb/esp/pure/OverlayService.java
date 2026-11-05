package com.mlbb.esp.pure;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;

public class OverlayService extends Service {
    private static final String TAG = "MlbbEspOverlay";
    public static OverlayService instance;

    private WindowManager wm;
    private FrameLayout root;
    private GlImGuiView glView;
    private View fab;
    private View fabStart;
    private View fabStop;
    private WindowManager.LayoutParams surfaceLp;
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            stopSelf();
            return;
        }
        try {
            NativeBridge.nativeInit();
            NativeBridge.nativeSetContext(getApplicationContext());
            NativeBridge.nativeSetInjectScratchDir(getCacheDir().getAbsolutePath());
            stageW2sInjectAgent();
            startForegroundNotif();
            wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            DisplayMetrics dm = new DisplayMetrics();
            wm.getDefaultDisplay().getRealMetrics(dm);
            NativeBridge.nativeSetDisplaySize(
                    Math.max(dm.widthPixels, 1), Math.max(dm.heightPixels, 1));
            attachWindows();
        } catch (Throwable t) {
            Log.e(TAG, "onCreate", t);
            stopSelf();
        }
    }

    /** Copy packaged libmlbb_espw2s.so to app files (readable path for ptrace inject). */
    private void stageW2sInjectAgent() {
        try {
            String libDir = getApplicationInfo().nativeLibraryDir;
            if (libDir == null) return;
            File src = new File(libDir, "libmlbb_espw2s.so");
            if (!src.isFile()) {
                Log.w(TAG, "missing " + src.getAbsolutePath());
                return;
            }
            File dst = new File(getFilesDir(), "libmlbb_espw2s.so");
            try (FileInputStream in = new FileInputStream(src);
                 FileOutputStream out = new FileOutputStream(dst, false)) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            dst.setReadable(true, false);
            NativeBridge.nativeSetW2sAgentPath(dst.getAbsolutePath());
            Log.i(TAG, "W2S agent staged: " + dst.getAbsolutePath());
        } catch (Throwable t) {
            Log.e(TAG, "stageW2sInjectAgent", t);
        }
    }

    private void attachWindows() {
        root = new FrameLayout(this);
        glView = new GlImGuiView(this);
        glView.configureOverlaySurface();
        glView.setOnGlReadyListener(v -> main.post(() -> {
            setMenuTouch(false);
            Log.i(TAG, "overlay ready — touches pass to game until you tap ≡");
        }));
        root.addView(glView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        final int overlayType = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;

        surfaceLp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                overlayType,
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                PixelFormat.TRANSLUCENT);
        surfaceLp.gravity = Gravity.TOP | Gravity.START;

        fab = new TextView(this);
        ((TextView) fab).setText("≡");
        ((TextView) fab).setTextSize(22f);
        ((TextView) fab).setPadding(28, 18, 28, 18);
        fab.setBackgroundColor(0xCC0A3555);
        fab.setOnClickListener(v -> NativeBridge.nativeToggleMenu());

        WindowManager.LayoutParams fabLp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                overlayType,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        fabLp.gravity = Gravity.TOP | Gravity.END;
        fabLp.x = 24;
        fabLp.y = 56;

        fabStart = new TextView(this);
        ((TextView) fabStart).setText("Start ESP");
        ((TextView) fabStart).setTextSize(13f);
        ((TextView) fabStart).setPadding(22, 14, 22, 14);
        fabStart.setBackgroundColor(0xCC1B5E20);
        fabStart.setOnClickListener(v -> NativeBridge.nativeStartEsp());

        fabStop = new TextView(this);
        ((TextView) fabStop).setText("Stop ESP");
        ((TextView) fabStop).setTextSize(13f);
        ((TextView) fabStop).setPadding(22, 14, 22, 14);
        fabStop.setBackgroundColor(0xCC8B1A1A);
        fabStop.setOnClickListener(v -> NativeBridge.nativeStopEsp());

        WindowManager.LayoutParams startLp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                overlayType,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        startLp.gravity = Gravity.TOP | Gravity.END;
        startLp.x = 24;
        startLp.y = 124;

        WindowManager.LayoutParams stopLp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                overlayType,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        stopLp.gravity = Gravity.TOP | Gravity.END;
        stopLp.x = 24;
        stopLp.y = 188;

        wm.addView(root, surfaceLp);
        wm.addView(fab, fabLp);
        wm.addView(fabStart, startLp);
        wm.addView(fabStop, stopLp);
    }

    void setMenuTouch(boolean interactive) {
        main.post(() -> {
            if (surfaceLp != null && wm != null && root != null) {
                if (interactive) {
                    surfaceLp.flags &= ~WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
                    surfaceLp.flags &= ~WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
                } else {
                    surfaceLp.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
                    surfaceLp.flags |= WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
                }
                try {
                    wm.updateViewLayout(root, surfaceLp);
                } catch (Throwable ignored) {
                }
            }
            int fabVis = interactive ? View.GONE : View.VISIBLE;
            if (fab != null) fab.setVisibility(fabVis);
            if (fabStart != null) fabStart.setVisibility(fabVis);
            if (fabStop != null) fabStop.setVisibility(fabVis);
            if (glView != null) glView.requestRender();
        });
    }

    private void startForegroundNotif() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                    new NotificationChannel("esp", "MLBB ESP", NotificationManager.IMPORTANCE_LOW));
            Notification n = new Notification.Builder(this, "esp")
                    .setContentTitle("MLBB ESP")
                    .setContentText("Tap ≡ for menu")
                    .setSmallIcon(android.R.drawable.ic_menu_compass)
                    .build();
            try {
                if (Build.VERSION.SDK_INT >= 34) {
                    startForeground(7, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
                } else {
                    startForeground(7, n);
                }
            } catch (Throwable t) {
                startForeground(7, n);
            }
        } else {
            @SuppressWarnings("deprecation")
            Notification n = new Notification.Builder(this)
                    .setContentTitle("MLBB ESP")
                    .setSmallIcon(android.R.drawable.ic_menu_compass)
                    .build();
            startForeground(7, n);
        }
    }

    @Override
    public void onDestroy() {
        try {
            if (glView != null) glView.releaseGl();
        } catch (Throwable t) {
            Log.e(TAG, "releaseGl", t);
        }
        try {
            if (fabStop != null) wm.removeView(fabStop);
        } catch (Throwable ignored) {
        }
        try {
            if (fabStart != null) wm.removeView(fabStart);
        } catch (Throwable ignored) {
        }
        try {
            if (fab != null) wm.removeView(fab);
        } catch (Throwable ignored) {
        }
        try {
            if (root != null) wm.removeView(root);
        } catch (Throwable ignored) {
        }
        instance = null;
        super.onDestroy();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
