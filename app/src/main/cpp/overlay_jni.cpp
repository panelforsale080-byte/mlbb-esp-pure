#include "ui_core.hpp"
#include "clip_android.hpp"
#include "esp_w2s_bridge.hpp"
#include "esp_inject.hpp"
#include <jni.h>

static JavaVM *g_vm = nullptr;
static jclass g_bridgeCls = nullptr;
static jmethodID g_setMenuTouch = nullptr;

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
    ui_esp_pipeline_idle();
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
Java_com_mlbb_esp_pure_NativeBridge_nativeSetW2sAgentPath(JNIEnv *env, jclass, jstring path) {
    if (!path) return;
    const char *utf = env->GetStringUTFChars(path, nullptr);
    if (utf) {
        esp_w2s_bridge_set_agent_path(utf);
        env->ReleaseStringUTFChars(path, utf);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeSetInjectScratchDir(JNIEnv *env, jclass, jstring dir) {
    if (!dir) return;
    const char *utf = env->GetStringUTFChars(dir, nullptr);
    if (utf) {
        esp_inject_set_scratch_dir(utf);
        env->ReleaseStringUTFChars(dir, utf);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlSurfaceCreated(JNIEnv *, jclass) {
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

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeGlDrawFrame(JNIEnv *, jclass) {
    ui_gl_draw_frame();
    static bool lastMenu = false;
    const bool now = ui_menu_interactive();
    if (now != lastMenu) {
        lastMenu = now;
        notify_menu_touch(now);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeTouch(JNIEnv *, jclass, jint action, jfloat x, jfloat y) {
    ui_touch((int)action, (float)x, (float)y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeOpenMenu(JNIEnv *, jclass) {
    ui_open_menu();
    notify_menu_touch(true);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_NativeBridge_nativeToggleMenu(JNIEnv *, jclass) {
    ui_toggle_menu();
    notify_menu_touch(ui_menu_interactive());
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
