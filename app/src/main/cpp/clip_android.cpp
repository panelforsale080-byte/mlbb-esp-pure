#include "clip_android.hpp"
#include <android_native_app_glue.h>
#include <jni.h>

static JavaVM *g_clipVm = nullptr;
static jobject g_clipCtx = nullptr;

void android_clip_set_context(JavaVM *vm, jobject context) {
    g_clipVm = vm;
    if (!vm || !context) return;
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    }
    if (g_clipCtx) env->DeleteGlobalRef(g_clipCtx);
    g_clipCtx = env->NewGlobalRef(context);
}

static bool copy_with_context(JNIEnv *env, jobject contextObj, const char *text) {
    if (!env || !contextObj || !text || !text[0]) return false;
    bool ok = false;
    jclass ctxCls = env->FindClass("android/content/Context");
    jclass clipMgrCls = env->FindClass("android/content/ClipboardManager");
    jclass clipDataCls = env->FindClass("android/content/ClipData");
    if (!ctxCls || !clipMgrCls || !clipDataCls) return false;

    jfieldID clipSvc = env->GetStaticFieldID(ctxCls, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
    jmethodID getSvc = env->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!clipSvc || !getSvc) return false;

    jstring svcName = (jstring)env->GetStaticObjectField(ctxCls, clipSvc);
    jobject clipMgr = env->CallObjectMethod(contextObj, getSvc, svcName);
    if (env->ExceptionCheck() || !clipMgr) {
        env->ExceptionClear();
        return false;
    }

    jmethodID newPlain = env->GetStaticMethodID(
        clipDataCls, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    jmethodID setClip = env->GetMethodID(clipMgrCls, "setPrimaryClip", "(Landroid/content/ClipData;)V");
    if (!newPlain || !setClip) return false;

    jstring label = env->NewStringUTF("MLBB ESP logs");
    jstring data = env->NewStringUTF(text);
    jobject clip = env->CallStaticObjectMethod(clipDataCls, newPlain, label, data);
    if (!clip) return false;
    env->CallVoidMethod(clipMgr, setClip, clip);
    ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    return ok;
}

bool android_copy_to_clipboard(const char *text) {
    if (!g_clipVm || !g_clipCtx) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    const bool ok = copy_with_context(env, g_clipCtx, text);
    return ok;
}

bool android_copy_to_clipboard(android_app *app, const char *text) {
    if (app && app->activity) {
        JavaVM *vm = app->activity->vm;
        JNIEnv *env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
        return copy_with_context(env, app->activity->clazz, text);
    }
    return android_copy_to_clipboard(text);
}
