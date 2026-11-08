package com.mlbb.esp.pure;

/** License URL + HMAC secrets live in native (not a single Java URL constant). */
public final class LicenseNative {
    static {
        System.loadLibrary("MlbbEsp");
    }

    private LicenseNative() {}

    public static native String nativeGetFetchUrl();

    public static native String nativeGetHmacPepper();

    public static native String nativeGetBoundPasteId();

    public static native byte[] nativeGetHmacKey();

    public static native void nativeSetLicenseAttested(boolean ok);

    public static native boolean nativeIsLicenseAttested();
}
