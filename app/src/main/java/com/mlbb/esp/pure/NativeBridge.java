package com.mlbb.esp.pure;

public final class NativeBridge {
    static {
        System.loadLibrary("MlbbEsp");
    }

    public static native void nativeInit();
    public static native void nativeSetContext(Object context);
    public static native void nativeSetDisplaySize(int width, int height);
    public static native void nativeSetW2sAgentPath(String absolutePath);

    /** Writable dir for inject .so copy when /data/local/tmp is denied. */
    public static native void nativeSetInjectScratchDir(String absolutePath);

    public static native void nativeGlSurfaceCreated();
    public static native void nativeGlSurfaceChanged(int w, int h);
    public static native void nativeGlSurfaceDestroyed();
    public static native void nativeGlDrawFrame();

    public static native void nativeTouch(int action, float x, float y);
    public static native void nativeOpenMenu();
    public static native void nativeToggleMenu();
    public static native boolean nativeIsMenuInteractive();
    public static native void nativeStartEsp();
    public static native void nativeStopEsp();

    public static void applyMenuTouch(boolean menuInteractive) {
        OverlayService svc = OverlayService.instance;
        if (svc != null) svc.setMenuTouch(menuInteractive);
    }
}
