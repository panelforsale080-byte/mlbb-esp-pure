package com.mlbb.esp.pure;

/** License decision stays in native. Java cannot pass a copied license file. */
public final class LicenseNative {
    static {
        System.loadLibrary("MlbbEsp");
    }

    private LicenseNative() {}

    public static native String nativeVerifyLicense(String password);

    public static native String nativeMusicTreeUrl();

    public static native String nativeMusicRawBase();

    public static native void nativeSetLicenseAttested(boolean ok);

    public static native boolean nativeIsLicenseAttested();
}
