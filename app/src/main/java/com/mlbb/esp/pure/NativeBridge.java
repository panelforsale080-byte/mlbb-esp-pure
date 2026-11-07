package com.mlbb.esp.pure;

public final class NativeBridge {
    static {
        System.loadLibrary("MlbbEsp");
    }

    public static native void nativeInit();
    public static native void nativeSetContext(Object context);
    public static native void nativeSetDisplaySize(int width, int height);

    public static native void nativeGlSurfaceCreated();
    public static native void nativeGlSurfaceChanged(int w, int h);
    public static native void nativeGlSurfaceDestroyed();
    public static native void nativeGlDrawFrame();

    public static native void nativeTouch(int action, float x, float y);
    public static native void nativeOpenMenu();
    public static native void nativeToggleMenu();
    /** ImGui menu off + overlay pass-through (FABs visible). */
    public static native void nativeMinimizeMenu();
    public static native boolean nativeIsMenuInteractive();
    public static native void nativeStartEsp();
    public static native void nativeStopEsp();

    public static void applyMenuTouch(boolean menuInteractive) {
        OverlayService svc = OverlayService.instance;
        if (svc != null) svc.setMenuTouch(menuInteractive);
    }

    /** Called from native auto-retri (EXTERNAL-MLBB Touch_Tap). */
    public static void dispatchRetriTap(float x, float y) {
        if (RetriGestureService.dispatchTap(x, y)) return;
        try {
            Runtime.getRuntime()
                    .exec(new String[] {"sh", "-c", "input tap " + (int) x + " " + (int) y});
        } catch (Throwable ignored) {
        }
    }
}
