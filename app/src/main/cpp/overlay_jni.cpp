#include "ui_core.hpp"
#include "clip_android.hpp"
#include "esp_hero_auto.hpp"
#include "license_native.hpp"
#include <jni.h>
#include <cstdio>
#include <cstring>

static JavaVM *g_vm = nullptr;
static jclass g_bridgeCls = nullptr;
static jmethodID g_setMenuTouch = nullptr;
static jmethodID g_setComboFab = nullptr;
static jmethodID g_updateMenuVideo = nullptr;
static jmethodID g_applyMenuVideoMuted = nullptr;
static jmethodID g_applyAntiCapture = nullptr;
static jmethodID g_setHideWhileRecording = nullptr;
static bool g_lastAntiCapture = false;
static bool g_lastHideWhileRecording = false;
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
    if (!g_setHideWhileRecording)
        g_setHideWhileRecording =
            env->GetStaticMethodID(clazz, "setHideWhileRecording", "(Z)V");
    ui_esp_pipeline_idle();
}

void ui_sync_hide_while_recording(bool on) {
    if (!g_vm || !g_bridgeCls || !g_setHideWhileRecording) return;
    if (on == g_lastHideWhileRecording) return;
    g_lastHideWhileRecording = on;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_setHideWhileRecording, on ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
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
    if (!license_native_attested()) return;
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeNeedsContinuousGl(JNIEnv *, jclass) {
    return ui_needs_continuous_gl() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlFrameDelayMs(JNIEnv *, jclass) {
    const int ms = ui_gl_frame_delay_ms();
    return ms < 16 ? 16 : ms;
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

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeSetRecordingHide(JNIEnv *, jclass, jboolean hide) {
    ui_set_recording_hide(hide == JNI_TRUE);
}

static jmethodID g_musicRefresh = nullptr;
static jmethodID g_musicPlay = nullptr;
static jmethodID g_musicVolume = nullptr;
static jmethodID g_musicMuted = nullptr;
static jmethodID g_musicStop = nullptr;
static jmethodID g_musicSnapshot = nullptr;

static void ensure_music_methods(JNIEnv *env) {
    if (!g_bridgeCls || !env) return;
    if (!g_musicRefresh) g_musicRefresh = env->GetStaticMethodID(g_bridgeCls, "musicRefresh", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_musicPlay) g_musicPlay = env->GetStaticMethodID(g_bridgeCls, "musicPlay", "(I)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_musicVolume) g_musicVolume = env->GetStaticMethodID(g_bridgeCls, "musicSetVolume", "(I)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_musicMuted) g_musicMuted = env->GetStaticMethodID(g_bridgeCls, "musicSetMuted", "(Z)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_musicStop) g_musicStop = env->GetStaticMethodID(g_bridgeCls, "musicStop", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_musicSnapshot)
        g_musicSnapshot = env->GetStaticMethodID(g_bridgeCls, "musicSnapshot", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static bool music_env(JNIEnv *&env) {
    env = nullptr;
    if (!g_vm || !g_bridgeCls) return false;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    ensure_music_methods(env);
    return true;
}

void music_refresh() {
    JNIEnv *env = nullptr;
    if (!music_env(env) || !g_musicRefresh) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_musicRefresh);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void music_play(int index) {
    JNIEnv *env = nullptr;
    if (!music_env(env) || !g_musicPlay) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_musicPlay, (jint)index);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void music_set_volume(int percent) {
    JNIEnv *env = nullptr;
    if (!music_env(env) || !g_musicVolume) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_musicVolume, (jint)percent);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void music_set_muted(bool muted) {
    JNIEnv *env = nullptr;
    if (!music_env(env) || !g_musicMuted) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_musicMuted, muted ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void music_stop() {
    JNIEnv *env = nullptr;
    if (!music_env(env) || !g_musicStop) return;
    env->CallStaticVoidMethod(g_bridgeCls, g_musicStop);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void music_poll(MusicSnapshot &out) {
    out = {};
    out.progress = -1;
    out.volume = 70;
    JNIEnv *env = nullptr;
    if (!music_env(env) || !g_musicSnapshot) {
        std::snprintf(out.status, sizeof(out.status), "Music not ready");
        return;
    }
    jstring js = (jstring)env->CallStaticObjectMethod(g_bridgeCls, g_musicSnapshot);
    if (env->ExceptionCheck() || !js) {
        env->ExceptionClear();
        std::snprintf(out.status, sizeof(out.status), "Music not ready");
        return;
    }
    const char *utf = env->GetStringUTFChars(js, nullptr);
    if (!utf) {
        env->DeleteLocalRef(js);
        return;
    }
    int muted = 0, playing = 0, downloading = 0;
    char status[160] = {};
    std::sscanf(utf, "%d\t%d\t%d\t%d\t%d\t%159[^\n]", &out.progress, &out.volume, &muted, &playing,
                &downloading, status);
    out.muted = muted != 0;
    out.playing = playing != 0;
    out.downloading = downloading != 0;
    std::snprintf(out.status, sizeof(out.status), "%s", status);
    const char *line = std::strchr(utf, '\n');
    if (line) line++;
    while (line && *line && out.count < 24) {
        const char *next = std::strchr(line, '\n');
        size_t n = next ? (size_t)(next - line) : std::strlen(line);
        if (n >= sizeof(out.names[0])) n = sizeof(out.names[0]) - 1;
        if (n > 0) {
            std::memcpy(out.names[out.count], line, n);
            out.names[out.count][n] = '\0';
            out.count++;
        }
        if (!next) break;
        line = next + 1;
    }
    env->ReleaseStringUTFChars(js, utf);
    env->DeleteLocalRef(js);
}
