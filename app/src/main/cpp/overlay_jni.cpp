#include "ui_core.hpp"
#include "clip_android.hpp"
#include "esp_hero_auto.hpp"
#include <jni.h>

static JavaVM *g_vm = nullptr;
static jclass g_bridgeCls = nullptr;
static jmethodID g_setMenuTouch = nullptr;
static jmethodID g_setComboFab = nullptr;
static jmethodID g_updateMenuVideo = nullptr;
static jmethodID g_applyMenuVideoMuted = nullptr;
static jmethodID g_applyAntiCapture = nullptr;
static bool g_lastAntiCapture = false;
static bool g_menuTouchNotified = false;
static bool g_lastMenuVideoVisible = false;
static int g_lastMenuVideoX = -1;
static int g_lastMenuVideoY = -1;
static int g_lastMenuVideoW = -1;
static int g_lastMenuVideoH = -1;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

static void notify_menu_touch(bool on) {
    if (!g_vm || !g_bridgeCls || !g_setMenuTouch) return;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_setMenuTouch, on ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeInit(JNIEnv *env, jclass clazz) {
    if (!g_bridgeCls) g_bridgeCls = (jclass)env->NewGlobalRef(clazz);
    if (!g_setMenuTouch)
        g_setMenuTouch = env->GetStaticMethodID(clazz, "applyMenuTouch", "(Z)V");
    if (!g_setComboFab)
        g_setComboFab = env->GetStaticMethodID(clazz, "setComboFabVisible", "(Z)V");
    if (!g_updateMenuVideo)
        g_updateMenuVideo =
            env->GetStaticMethodID(clazz, "updateMenuVideoLayout", "(ZIIII)V");
    if (!g_applyMenuVideoMuted)
        g_applyMenuVideoMuted = env->GetStaticMethodID(clazz, "applyMenuVideoMuted", "(Z)V");
    if (!g_applyAntiCapture)
        g_applyAntiCapture = env->GetStaticMethodID(clazz, "applyAntiCaptureSecure", "(Z)V");
    ui_esp_pipeline_idle();
}

void ui_sync_anti_capture(bool on) {
    if (!g_vm || !g_bridgeCls || !g_applyAntiCapture) return;
    if (on == g_lastAntiCapture) return;
    g_lastAntiCapture = on;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_applyAntiCapture, on ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void ui_menu_video_sync(bool visible, int x, int y, int w, int h) {
    if (!g_vm || !g_bridgeCls || !g_updateMenuVideo) return;
    if (!visible) {
        if (!g_lastMenuVideoVisible) return;
        g_lastMenuVideoVisible = false;
        g_lastMenuVideoX = g_lastMenuVideoY = g_lastMenuVideoW = g_lastMenuVideoH = -1;
    } else if (visible && g_lastMenuVideoVisible && g_lastMenuVideoX == x && g_lastMenuVideoY == y
               && g_lastMenuVideoW == w && g_lastMenuVideoH == h) {
        return;
    } else {
        g_lastMenuVideoVisible = true;
        g_lastMenuVideoX = x;
        g_lastMenuVideoY = y;
        g_lastMenuVideoW = w;
        g_lastMenuVideoH = h;
    }
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_updateMenuVideo, visible ? JNI_TRUE : JNI_FALSE, x, y,
                              w, h);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void ui_menu_video_set_muted(bool muted) {
    if (!g_vm || !g_bridgeCls || !g_applyMenuVideoMuted) return;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_applyMenuVideoMuted, muted ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeSetMenuVideoMuted(JNIEnv *, jclass, jboolean muted) {
    ui_menu_video_apply_muted_from_java(muted == JNI_TRUE);
}

void ui_sync_combo_fab_visible(bool show) {
    if (!g_vm || !g_bridgeCls || !g_setComboFab) return;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_setComboFab, show ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeSetContext(JNIEnv *env, jclass, jobject ctx) {
    android_clip_set_context(g_vm, ctx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeSetDisplaySize(JNIEnv *, jclass, jint w, jint h) {
    ui_set_display_size((int)w, (int)h);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlSurfaceCreated(JNIEnv *, jclass) {
    /* Stale g_menuTouchNotified can skip JNI; force applyMenuTouch after EGL recreate. */
    g_menuTouchNotified = true;
    ui_gl_surface_created();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlSurfaceChanged(JNIEnv *, jclass, jint w, jint h) {
    ui_gl_surface_changed((int)w, (int)h);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlSurfaceDestroyed(JNIEnv *, jclass) {
    ui_gl_surface_destroyed();
}

void ui_sync_menu_touch_state() {
    const bool now = ui_menu_interactive();
    if (now == g_menuTouchNotified) return;
    g_menuTouchNotified = now;
    notify_menu_touch(now);
}

void ui_force_menu_pass_through() {
    ui_menu_video_sync(false, 0, 0, 0, 0);
    g_lastMenuVideoVisible = false;
    g_lastMenuVideoX = g_lastMenuVideoY = g_lastMenuVideoW = g_lastMenuVideoH = -1;
    g_menuTouchNotified = true;
    notify_menu_touch(false);
    g_menuTouchNotified = false;
    ui_mark_menu_dismissed();
}

static void sync_menu_touch_to_java() { ui_sync_menu_touch_state(); }

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlDrawFrame(JNIEnv *, jclass) {
    ui_gl_draw_frame();
    sync_menu_touch_to_java();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeTouch(JNIEnv *, jclass, jint action, jfloat x, jfloat y) {
    ui_touch((int)action, (float)x, (float)y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeOpenMenu(JNIEnv *, jclass) {
    ui_open_menu();
    g_menuTouchNotified = false;
    notify_menu_touch(true);
    g_menuTouchNotified = true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeToggleMenu(JNIEnv *, jclass) {
    ui_toggle_menu();
    g_menuTouchNotified = ui_menu_interactive();
    notify_menu_touch(g_menuTouchNotified);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeMinimizeMenu(JNIEnv *, jclass) {
    ui_minimize_menu();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeIsMenuInteractive(JNIEnv *, jclass) {
    return ui_menu_interactive() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeStartEsp(JNIEnv *, jclass) {
    ui_esp_start();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeStopEsp(JNIEnv *, jclass) {
    ui_esp_stop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeHeroComboPulse(JNIEnv *, jclass) {
    esp_hero_auto_combo_pulse();
}
