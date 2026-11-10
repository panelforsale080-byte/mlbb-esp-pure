# JNI names are baked into libMlbbEsp.so. Keep those classes and native methods.
-keep class com.mlbb.esp.pure.NativeBridge { *; }
-keep class com.mlbb.esp.pure.LicenseNative { *; }

-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# Manifest components and the view created from code.
-keep class com.mlbb.esp.pure.EspActivity { *; }
-keep class com.mlbb.esp.pure.OverlayService { *; }
-keep class com.mlbb.esp.pure.GlImGuiView { *; }
-keep class com.mlbb.esp.pure.OverlayRootLayout { *; }

-dontwarn com.mlbb.esp.pure.**
