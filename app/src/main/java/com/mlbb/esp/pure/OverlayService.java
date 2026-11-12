package com.mlbb.esp.pure;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import java.util.Locale;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageButton;

public class OverlayService extends Service {
    public static final String EXTRA_CMD = "xorel_cmd";
    public static final String CMD_START_ESP = "start_esp";
    public static final String CMD_STOP_ESP = "stop_esp";

    private static final String TAG = "XorelOverlay";
    public static OverlayService instance;

    private WindowManager wm;
    private FrameLayout root;
    private GlImGuiView glView;
    private MenuVideoPanel menuVideo;
    private MenuBgPanel menuBg;
    private ImageButton fabLogo;
    private Button fabCombo;
    private WindowManager.LayoutParams surfaceLp;
    private WindowManager.LayoutParams fabLp;
    private WindowManager.LayoutParams fabComboLp;
    private WindowManager.LayoutParams menuVideoLp;
    private WindowManager.LayoutParams menuBgLp;
    private boolean comboFabWanted;
    private boolean menuInteractive;
    private boolean antiCaptureSecure;
    private boolean hideWhileRecording;
    private boolean concealForRecording;
    private boolean glSurfaceSecure;
    private boolean glReadyOnce;
    private boolean api35Recording;
    private Object screenRecordingCallback;
    private final Runnable recordingPoll =
            new Runnable() {
                @Override
                public void run() {
                    if (!hideWhileRecording) return;
                    refreshRecordingConceal();
                    main.postDelayed(this, 1000L);
                }
            };
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean glReady = false;
    private String pendingCmd = null;
    private float fabTouchLastRawX;
    private float fabTouchLastRawY;
    private float fabTouchDownRawX;
    private float fabTouchDownRawY;
    private boolean fabDragging;
    private boolean fabHoldExitTriggered;
    private Runnable fabHoldExitTask;
    private static final long FAB_HOLD_EXIT_MS = 700L;
    private DisplayManager displayManager;
    private DisplayManager.DisplayListener displayListener;

    private static final int SURFACE_FLAGS_PASS =
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_SPLIT_TOUCH;
    private static final int SURFACE_FLAGS_MENU =
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            stopSelf();
            return;
        }
        if (!LicenseNative.nativeIsLicenseAttested()) {
            stopSelf();
            return;
        }
        try {
            NativeBridge.nativeInit();
            NativeBridge.nativeSetContext(getApplicationContext());
            MusicController.attach(getApplicationContext());
            startForegroundNotif();
            wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            DisplayMetrics dm = new DisplayMetrics();
            wm.getDefaultDisplay().getRealMetrics(dm);
            NativeBridge.nativeSetDisplaySize(
                    Math.max(dm.widthPixels, 1), Math.max(dm.heightPixels, 1));
            attachWindows();
            registerDisplayListener();
        } catch (Throwable t) {
            Log.e(TAG, "onCreate", t);
            stopSelf();
        }
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void getScreenSize(int[] out) {
        DisplayMetrics dm = new DisplayMetrics();
        wm.getDefaultDisplay().getRealMetrics(dm);
        out[0] = Math.max(dm.widthPixels, 1);
        out[1] = Math.max(dm.heightPixels, 1);
    }

    private void registerDisplayListener() {
        displayManager = (DisplayManager) getSystemService(DISPLAY_SERVICE);
        if (displayManager == null) return;
        displayListener =
                new DisplayManager.DisplayListener() {
                    @Override
                    public void onDisplayAdded(int displayId) {
                        main.post(OverlayService.this::refreshRecordingConceal);
                    }

                    @Override
                    public void onDisplayRemoved(int displayId) {
                        main.post(OverlayService.this::refreshRecordingConceal);
                    }

                    @Override
                    public void onDisplayChanged(int displayId) {
                        if (displayId == android.view.Display.DEFAULT_DISPLAY) {
                            main.post(OverlayService.this::applyDisplayGeometry);
                        }
                    }
                };
        displayManager.registerDisplayListener(displayListener, main);
    }

    /** Rotation / resolution change (e.g. MLBB landscape): sync FAB + ImGui + ESP screen size. */
    void applyDisplayGeometry() {
        if (wm == null) return;
        int[] wh = new int[2];
        getScreenSize(wh);
        NativeBridge.nativeSetDisplaySize(wh[0], wh[1]);
        if (surfaceLp != null && root != null) {
            surfaceLp.width = WindowManager.LayoutParams.MATCH_PARENT;
            surfaceLp.height = WindowManager.LayoutParams.MATCH_PARENT;
            try {
                wm.updateViewLayout(root, surfaceLp);
            } catch (Throwable t) {
                Log.w(TAG, "applyDisplayGeometry surface", t);
            }
        }
        if (fabLp != null && fabLogo != null) {
            clampFabToScreen();
            try {
                wm.updateViewLayout(fabLogo, fabLp);
            } catch (Throwable ignored) {
            }
        }
        if (fabComboLp != null && fabCombo != null) {
            clampComboFabToScreen();
            try {
                wm.updateViewLayout(fabCombo, fabComboLp);
            } catch (Throwable ignored) {
            }
        }
        if (glView != null) {
            glView.requestLayout();
            glView.requestRender();
        }
    }

    private void clampFabToScreen() {
        if (fabLp == null || fabLogo == null || wm == null) return;
        int[] wh = new int[2];
        getScreenSize(wh);
        fabLogo.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED);
        int fw = fabLogo.getMeasuredWidth() > 0 ? fabLogo.getMeasuredWidth() : fabLogo.getWidth();
        int fh = fabLogo.getMeasuredHeight() > 0 ? fabLogo.getMeasuredHeight() : fabLogo.getHeight();
        if (fw <= 0) fw = dp(56);
        if (fh <= 0) fh = dp(56);
        if (fabLp.x < 0) fabLp.x = 0;
        if (fabLp.y < 0) fabLp.y = 0;
        if (fabLp.x > wh[0] - fw) fabLp.x = Math.max(0, wh[0] - fw);
        if (fabLp.y > wh[1] - fh) fabLp.y = Math.max(0, wh[1] - fh);
    }

    private void clampComboFabToScreen() {
        if (fabComboLp == null || fabCombo == null || wm == null) return;
        int[] wh = new int[2];
        getScreenSize(wh);
        fabCombo.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED);
        int fw =
                fabCombo.getMeasuredWidth() > 0 ? fabCombo.getMeasuredWidth() : fabCombo.getWidth();
        int fh =
                fabCombo.getMeasuredHeight() > 0
                        ? fabCombo.getMeasuredHeight()
                        : fabCombo.getHeight();
        if (fw <= 0) fw = dp(52);
        if (fh <= 0) fh = dp(52);
        if (fabComboLp.x < 0) fabComboLp.x = 0;
        if (fabComboLp.y < 0) fabComboLp.y = 0;
        if (fabComboLp.x > wh[0] - fw) fabComboLp.x = Math.max(0, wh[0] - fw);
        if (fabComboLp.y > wh[1] - fh) fabComboLp.y = Math.max(0, wh[1] - fh);
    }

    void setComboFabVisible(boolean want) {
        comboFabWanted = want;
        applyComboFabVisibility();
    }

    private void applyComboFabVisibility() {
        if (fabCombo == null) return;
        final boolean show = comboFabWanted && !menuInteractive && !hideWhileRecording;
        fabCombo.setVisibility(show ? View.VISIBLE : View.GONE);
        if (show && fabComboLp != null) {
            clampComboFabToScreen();
            try {
                wm.updateViewLayout(fabCombo, fabComboLp);
            } catch (Throwable ignored) {
            }
        }
    }

    private void attachWindows() {
        root = new OverlayRootLayout(this);
        glView = new GlImGuiView(this);
        glView.configureOverlaySurface();
        glView.setOnGlReadyListener(
                v -> main.post(() -> {
                    glReady = true;
                    if (!glReadyOnce) {
                        glReadyOnce = true;
                        try {
                            NativeBridge.nativeMinimizeMenu();
                        } catch (Throwable t) {
                            Log.w(TAG, "nativeMinimizeMenu", t);
                        }
                    }
                    flushPendingCmd();
                    if (overlaySecure() && glView != null) SecureLayer.apply(glView, true);
                    Log.i(TAG, "GL ready — tap Xorel icon for menu");
                }));
        root.addView(
                glView,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        final int overlayType =
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE;

        surfaceLp =
                new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.MATCH_PARENT,
                        overlayType,
                        SURFACE_FLAGS_PASS,
                        PixelFormat.TRANSLUCENT);
        surfaceLp.gravity = Gravity.TOP | Gravity.START;

        fabLogo = new ImageButton(this);
        fabLogo.setBackgroundResource(R.drawable.bg_fab_xorel);
        fabLogo.setImageResource(R.drawable.ic_xorel_logo);
        fabLogo.setScaleType(android.widget.ImageView.ScaleType.CENTER_INSIDE);
        fabLogo.setPadding(dp(10), dp(10), dp(10), dp(10));
        fabLogo.setContentDescription(getString(R.string.fab_open_menu));
        int fabSize = dp(56);
        fabLogo.setMinimumWidth(fabSize);
        fabLogo.setMinimumHeight(fabSize);

        fabLp =
                new WindowManager.LayoutParams(
                        fabSize,
                        fabSize,
                        overlayType,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
        /* TOP|START so x,y are unambiguous for dragging */
        fabLp.gravity = Gravity.TOP | Gravity.START;
        int[] wh = new int[2];
        getScreenSize(wh);
        fabLp.x = wh[0] - fabSize - dp(16);
        fabLp.y = dp(52);

        final int dragSlopPx = dp(10);
        fabLogo.setOnTouchListener(
                (v, event) -> {
                    switch (event.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:
                            fabTouchDownRawX = event.getRawX();
                            fabTouchDownRawY = event.getRawY();
                            fabTouchLastRawX = fabTouchDownRawX;
                            fabTouchLastRawY = fabTouchDownRawY;
                            fabDragging = false;
                            fabHoldExitTriggered = false;
                            scheduleFabHoldExit();
                            return true;
                        case MotionEvent.ACTION_MOVE:
                            float dx = event.getRawX() - fabTouchLastRawX;
                            float dy = event.getRawY() - fabTouchLastRawY;
                            fabTouchLastRawX = event.getRawX();
                            fabTouchLastRawY = event.getRawY();
                            float dist =
                                    (float) Math.hypot(
                                            event.getRawX() - fabTouchDownRawX,
                                            event.getRawY() - fabTouchDownRawY);
                            if (dist > dragSlopPx) {
                                fabDragging = true;
                                cancelFabHoldExit();
                            }
                            fabLp.x += (int) dx;
                            fabLp.y += (int) dy;
                            clampFabToScreen();
                            try {
                                wm.updateViewLayout(fabLogo, fabLp);
                            } catch (Throwable ignored) {
                            }
                            return fabDragging;
                        case MotionEvent.ACTION_UP:
                            cancelFabHoldExit();
                            if (fabHoldExitTriggered) return true;
                            if (!fabDragging) {
                                applyDisplayGeometry();
                                setMenuTouch(true);
                                NativeBridge.nativeOpenMenu();
                                if (glView != null) {
                                    glView.requestRender();
                                    main.post(glView::requestRender);
                                    main.postDelayed(glView::requestRender, 32);
                                }
                            }
                            return true;
                        case MotionEvent.ACTION_CANCEL:
                            cancelFabHoldExit();
                            return true;
                        default:
                            return false;
                    }
                });

        root.setClickable(false);
        root.setFocusable(false);
        root.setFocusableInTouchMode(false);
        glView.setClickable(false);
        glView.setFocusable(false);
        glView.setFocusableInTouchMode(false);

        fabCombo = new Button(this);
        fabCombo.setText("C");
        fabCombo.setAllCaps(false);
        fabCombo.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        fabCombo.setBackgroundResource(R.drawable.bg_fab_esp_start);
        fabCombo.setPadding(dp(4), dp(2), dp(4), dp(2));
        fabCombo.setContentDescription(getString(R.string.fab_hero_combo));
        final int comboSize = dp(52);
        fabCombo.setMinimumWidth(comboSize);
        fabCombo.setMinimumHeight(comboSize);
        fabCombo.setOnClickListener(
                v -> {
                    try {
                        NativeBridge.nativeHeroComboPulse();
                    } catch (Throwable t) {
                        Log.w(TAG, "combo pulse", t);
                    }
                    if (glView != null) glView.requestRender();
                });

        fabComboLp =
                new WindowManager.LayoutParams(
                        comboSize,
                        comboSize,
                        overlayType,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
        fabComboLp.gravity = Gravity.TOP | Gravity.START;
        fabComboLp.x = dp(12);
        fabComboLp.y = wh[1] / 2 - comboSize / 2;
        fabCombo.setVisibility(View.GONE);

        menuVideo = new MenuVideoPanel(this);
        menuVideoLp =
                new WindowManager.LayoutParams(
                        1,
                        1,
                        overlayType,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
        menuVideoLp.gravity = Gravity.TOP | Gravity.START;

        menuBg = new MenuBgPanel(this);
        menuBgLp =
                new WindowManager.LayoutParams(
                        1,
                        1,
                        overlayType,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
        menuBgLp.gravity = Gravity.TOP | Gravity.START;

        wm.addView(menuBg, menuBgLp);
        wm.addView(root, surfaceLp);
        wm.addView(menuVideo, menuVideoLp);
        wm.addView(fabCombo, fabComboLp);
        wm.addView(fabLogo, fabLp);
        menuVideo.setVisibility(View.GONE);
        menuBg.setVisibility(View.GONE);
        hideMenuVideoWindow();
        hideMenuBgWindow();
        menuInteractive = false;
        applyMenuTouchFlags(false);
        main.post(
                () -> {
                    try {
                        NativeBridge.nativeMinimizeMenu();
                    } catch (Throwable t) {
                        Log.w(TAG, "post-attach minimize", t);
                    }
                });
    }

    private void scheduleFabHoldExit() {
        cancelFabHoldExit();
        fabHoldExitTask =
                () -> {
                    fabHoldExitTriggered = true;
                    fabDragging = true;
                    exitOverlayFromLogoHold();
                };
        main.postDelayed(fabHoldExitTask, FAB_HOLD_EXIT_MS);
    }

    private void cancelFabHoldExit() {
        if (fabHoldExitTask != null) {
            main.removeCallbacks(fabHoldExitTask);
            fabHoldExitTask = null;
        }
    }

    private void exitOverlayFromLogoHold() {
        cancelFabHoldExit();
        Log.i(TAG, "FAB hold — stopping overlay");
        try {
            NativeBridge.nativeStopEsp();
            NativeBridge.nativeMinimizeMenu();
        } catch (Throwable t) {
            Log.w(TAG, "exitOverlayFromLogoHold", t);
        }
        stopSelf();
    }

    private void flushPendingCmd() {
        if (!glReady || pendingCmd == null) return;
        String c = pendingCmd;
        pendingCmd = null;
        if (CMD_START_ESP.equals(c)) {
            NativeBridge.nativeStartEsp();
            if (glView != null) glView.requestRender();
        } else if (CMD_STOP_ESP.equals(c)) {
            NativeBridge.nativeStopEsp();
        }
    }

    boolean isMenuInteractive() {
        return menuInteractive;
    }

    void setMenuTouch(boolean interactive) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            applyMenuTouchFlags(interactive);
        } else {
            main.post(() -> applyMenuTouchFlags(interactive));
        }
    }

    void setHideWhileRecording(boolean on) {
        Runnable apply =
                () -> {
                    hideWhileRecording = on;
                    main.removeCallbacks(recordingPoll);
                    setRecordingSecure(on);
                    applyComboFabVisibility();
                    syncRecordedOverlay();
                };
        if (Looper.myLooper() == Looper.getMainLooper()) apply.run();
        else main.post(apply);
    }

    private void ensureScreenRecordingCallback() {
        if (screenRecordingCallback != null || Build.VERSION.SDK_INT < 35 || wm == null) return;
        screenRecordingCallback =
                ScreenRecordingWatch.register(
                        wm,
                        main,
                        recording -> {
                            api35Recording = recording;
                            main.post(this::refreshRecordingConceal);
                        });
    }

    private void refreshRecordingConceal() {
        if (!hideWhileRecording) return;
        applyRecordingConceal(detectScreenRecording());
    }

    private boolean detectScreenRecording() {
        if (api35Recording) return true;
        if (displayManager == null) return false;
        Display[] displays = displayManager.getDisplays();
        if (displays == null) return false;
        for (Display display : displays) {
            if (display == null || display.getDisplayId() == Display.DEFAULT_DISPLAY) continue;
            String name = display.getName();
            if (name == null) continue;
            String n = name.toLowerCase(Locale.US);
            if (n.contains("record")
                    || n.contains("capture")
                    || n.contains("projection")
                    || n.contains("cast")) {
                return true;
            }
        }
        return false;
    }

    private void applyRecordingConceal(boolean recording) {
        if (!recording && Build.VERSION.SDK_INT >= 35) return;
        setRecordingSecure(recording);
    }

    /** ESP stays on screen. FLAG_SECURE keeps it out of the recording. */
    private void setRecordingSecure(boolean on) {
        if (on == concealForRecording) return;
        concealForRecording = on;
        try {
            NativeBridge.nativeSetRecordingHide(on);
        } catch (Throwable t) {
            Log.w(TAG, "recording secure", t);
        }
        refreshSurfaceLpFlags();
        pushSecureWindow(root, surfaceLp);
        pushSecureWindow(menuVideo, menuVideoLp);
        pushSecureWindow(menuBg, menuBgLp);
        pushSecureWindow(fabLogo, fabLp);
        pushSecureWindow(fabCombo, fabComboLp);
        applyGlCaptureProtect();
    }

    /** ESP stays on the phone. The GL layer itself is marked secure so the recording skips it. */
    private void applyGlCaptureProtect() {
        boolean want = overlaySecure();
        if (glView == null) return;
        glSurfaceSecure = want;
        SecureLayer.apply(glView, want);
    }

    private boolean overlaySecure() {
        return antiCaptureSecure || concealForRecording;
    }

    private void pushSecureWindow(View view, WindowManager.LayoutParams lp) {
        if (view == null || lp == null || wm == null) return;
        int next = lp.flags;
        if (overlaySecure()) next |= WindowManager.LayoutParams.FLAG_SECURE;
        else next &= ~WindowManager.LayoutParams.FLAG_SECURE;
        if (next == lp.flags) return;
        lp.flags = next;
        try {
            wm.updateViewLayout(view, lp);
        } catch (Throwable ignored) {
        }
    }

    void applyAntiCaptureSecure(boolean on) {
        antiCaptureSecure = on;
        Runnable apply =
                () -> {
                    refreshSurfaceLpFlags();
                    if (surfaceLp != null && wm != null && root != null) {
                        try {
                            wm.updateViewLayout(root, surfaceLp);
                        } catch (Throwable t) {
                            Log.w(TAG, "applyAntiCaptureSecure surface", t);
                        }
                    }
                    if (menuVideo != null && menuVideoLp != null && wm != null) {
                        try {
                            wm.updateViewLayout(menuVideo, menuVideoLp);
                        } catch (Throwable ignored) {
                        }
                    }
                    if (menuBg != null && menuBgLp != null && wm != null) {
                        try {
                            wm.updateViewLayout(menuBg, menuBgLp);
                        } catch (Throwable ignored) {
                        }
                    }
                    pushSecureWindow(fabLogo, fabLp);
                    pushSecureWindow(fabCombo, fabComboLp);
                    applyGlCaptureProtect();
                };
        if (Looper.myLooper() == Looper.getMainLooper()) apply.run();
        else main.post(apply);
    }

    private void refreshSurfaceLpFlags() {
        if (surfaceLp == null) return;
        int flags = menuInteractive ? SURFACE_FLAGS_MENU : SURFACE_FLAGS_PASS;
        if (overlaySecure()) flags |= WindowManager.LayoutParams.FLAG_SECURE;
        surfaceLp.flags = flags;
        if (menuVideoLp != null) {
            int vf =
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
            if (overlaySecure()) vf |= WindowManager.LayoutParams.FLAG_SECURE;
            menuVideoLp.flags = vf;
        }
        if (menuBgLp != null) {
            int bf =
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
            if (overlaySecure()) bf |= WindowManager.LayoutParams.FLAG_SECURE;
            menuBgLp.flags = bf;
        }
    }

    private void applyMenuTouchFlags(boolean interactive) {
        if (surfaceLp == null || wm == null || root == null) return;
        menuInteractive = interactive;
        if (!interactive) {
            try {
                if (NativeBridge.nativeIsMenuInteractive()) {
                    NativeBridge.nativeMinimizeMenu();
                } else if (menuVideo != null) {
                    menuVideo.onMenuVisibleChanged(false);
                }
            } catch (Throwable t) {
                Log.w(TAG, "nativeMinimizeMenu", t);
            }
        }
        refreshSurfaceLpFlags();
        root.setClickable(interactive);
        root.setFocusable(interactive);
        root.setFocusableInTouchMode(interactive);
        if (glView != null) {
            glView.setClickable(interactive);
            glView.setFocusable(interactive);
            glView.setFocusableInTouchMode(interactive);
        }
        try {
            wm.updateViewLayout(root, surfaceLp);
        } catch (Throwable t) {
            Log.w(TAG, "applyMenuTouchFlags surface", t);
        }
        if (fabLogo != null) {
            fabLogo.setVisibility(interactive ? View.GONE : View.VISIBLE);
            if (!interactive && fabLp != null) {
                clampFabToScreen();
                try {
                    wm.updateViewLayout(fabLogo, fabLp);
                } catch (Throwable ignored) {
                }
            }
        }
        applyComboFabVisibility();
        syncRecordedOverlay();
        if (!interactive) {
            hideMenuVideoWindow();
            hideMenuBgWindow();
        }
        if (glView != null) {
            glView.requestRender();
            if (!interactive) {
                main.postDelayed(glView::requestRender, 33);
                main.postDelayed(glView::requestRender, 66);
                main.postDelayed(glView::requestRender, 120);
            }
        }
    }

    private void syncRecordedOverlay() {
        if (glView != null) glView.requestRender();
    }

    private void hideMenuVideoWindow() {
        if (menuVideo == null || menuVideoLp == null || wm == null) return;
        menuVideoLp.x = 0;
        menuVideoLp.y = 0;
        menuVideoLp.width = 1;
        menuVideoLp.height = 1;
        menuVideoLp.flags =
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | (overlaySecure() ? WindowManager.LayoutParams.FLAG_SECURE : 0);
        try {
            wm.updateViewLayout(menuVideo, menuVideoLp);
        } catch (Throwable ignored) {
        }
        menuVideo.onMenuVisibleChanged(false);
    }

    private void hideMenuBgWindow() {
        if (menuBg == null || menuBgLp == null || wm == null) return;
        menuBgLp.x = 0;
        menuBgLp.y = 0;
        menuBgLp.width = 1;
        menuBgLp.height = 1;
        menuBgLp.flags =
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | (overlaySecure() ? WindowManager.LayoutParams.FLAG_SECURE : 0);
        try {
            wm.updateViewLayout(menuBg, menuBgLp);
        } catch (Throwable ignored) {
        }
        menuBg.onMenuVisibleChanged(false);
    }

    void updateMenuBgLayout(boolean visible, int x, int y, int w, int h) {
        if (menuBg == null || menuBgLp == null || wm == null) return;
        Runnable apply =
                () -> {
                    if (!menuInteractive || !visible || w < 12 || h < 12) {
                        hideMenuBgWindow();
                        return;
                    }
                    int bf =
                            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
                    if (overlaySecure()) bf |= WindowManager.LayoutParams.FLAG_SECURE;
                    menuBgLp.flags = bf;
                    final int[] origin = new int[2];
                    if (root != null) root.getLocationOnScreen(origin);
                    menuBgLp.x = origin[0] + x;
                    menuBgLp.y = origin[1] + y;
                    menuBgLp.width = w;
                    menuBgLp.height = h;
                    try {
                        wm.updateViewLayout(menuBg, menuBgLp);
                    } catch (Throwable t) {
                        Log.w(TAG, "updateMenuBgLayout", t);
                    }
                    menuBg.onMenuVisibleChanged(true);
                };
        if (Looper.myLooper() == Looper.getMainLooper()) apply.run();
        else main.post(apply);
    }

    void updateMenuVideoLayout(boolean visible, int x, int y, int w, int h) {
        if (menuVideo == null || menuVideoLp == null || wm == null) return;
        Runnable apply =
                () -> {
                    if (!menuInteractive || !visible || w < 12 || h < 12) {
                        hideMenuVideoWindow();
                        return;
                    }
                    int vf =
                            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
                    if (overlaySecure()) vf |= WindowManager.LayoutParams.FLAG_SECURE;
                    menuVideoLp.flags = vf;
                    final int[] origin = new int[2];
                    if (root != null) root.getLocationOnScreen(origin);
                    menuVideoLp.x = origin[0] + x;
                    menuVideoLp.y = origin[1] + y;
                    menuVideoLp.width = w;
                    menuVideoLp.height = h;
                    try {
                        wm.updateViewLayout(menuVideo, menuVideoLp);
                    } catch (Throwable t) {
                        Log.w(TAG, "updateMenuVideoLayout", t);
                    }
                    menuVideo.onMenuVisibleChanged(true);
                };
        if (Looper.myLooper() == Looper.getMainLooper()) apply.run();
        else main.post(apply);
    }

    void applyMenuVideoMuted(boolean muted) {
        if (menuVideo == null) return;
        Runnable apply = () -> menuVideo.setMuted(muted, false);
        if (Looper.myLooper() == Looper.getMainLooper()) apply.run();
        else main.post(apply);
    }

    private void startForegroundNotif() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                    new NotificationChannel(
                            "esp", getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW));
            Notification n =
                    new Notification.Builder(this, "esp")
                            .setContentTitle(getString(R.string.notif_title))
                            .setContentText(getString(R.string.notif_text))
                            .setSmallIcon(android.R.drawable.ic_menu_compass)
                            .build();
            try {
                if (Build.VERSION.SDK_INT >= 34) {
                    startForeground(
                            7,
                            n,
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
                                    | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
                } else {
                    startForeground(7, n);
                }
            } catch (Throwable t) {
                startForeground(7, n);
            }
        } else {
            @SuppressWarnings("deprecation")
            Notification n =
                    new Notification.Builder(this)
                            .setContentTitle(getString(R.string.notif_title))
                            .setSmallIcon(android.R.drawable.ic_menu_compass)
                            .build();
            startForeground(7, n);
        }
    }

    @Override
    public void onDestroy() {
        main.removeCallbacks(recordingPoll);
        if (wm != null && screenRecordingCallback != null) {
            ScreenRecordingWatch.unregister(wm, screenRecordingCallback);
            screenRecordingCallback = null;
        }
        if (displayManager != null && displayListener != null) {
            try {
                displayManager.unregisterDisplayListener(displayListener);
            } catch (Throwable ignored) {
            }
        }
        try {
            if (glView != null) glView.releaseGl();
        } catch (Throwable t) {
            Log.e(TAG, "releaseGl", t);
        }
        try {
            if (fabLogo != null) wm.removeView(fabLogo);
        } catch (Throwable ignored) {
        }
        try {
            if (fabCombo != null) wm.removeView(fabCombo);
        } catch (Throwable ignored) {
        }
        try {
            if (menuVideo != null) {
                menuVideo.releaseAll();
                wm.removeView(menuVideo);
            }
        } catch (Throwable ignored) {
        }
        try {
            if (menuBg != null) {
                menuBg.releaseAll();
                wm.removeView(menuBg);
            }
        } catch (Throwable ignored) {
        }
        try {
            if (root != null) wm.removeView(root);
        } catch (Throwable ignored) {
        }
        MusicController.stop();
        instance = null;
        super.onDestroy();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            String cmd = intent.getStringExtra(EXTRA_CMD);
            if (cmd != null) {
                pendingCmd = cmd;
                flushPendingCmd();
            }
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
