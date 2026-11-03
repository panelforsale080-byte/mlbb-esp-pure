#include "clip_android.hpp"
#include <android_native_app_glue.h>
#include <jni.h>

bool android_copy_to_clipboard(android_app *app, const char *text) {
    if (!app || !app->activity || !text || !text[0]) return false;
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;

    bool ok = false;
    do {
        jclass ctxCls = env->FindClass("android/content/Context");
        jclass clipMgrCls = env->FindClass("android/content/ClipboardManager");
        jclass clipDataCls = env->FindClass("android/content/ClipData");
        if (!ctxCls || !clipMgrCls || !clipDataCls) break;

        jfieldID clipSvc = env->GetStaticFieldID(ctxCls, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
        jmethodID getSvc = env->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        if (!clipSvc || !getSvc) break;

        jstring svcName = (jstring)env->GetStaticObjectField(ctxCls, clipSvc);
        jobject clipMgr = env->CallObjectMethod(app->activity->clazz, getSvc, svcName);
        if (env->ExceptionCheck() || !clipMgr) {
            env->ExceptionClear();
            break;
        }

        jmethodID newPlain = env->GetStaticMethodID(
            clipDataCls, "newPlainText",
            "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
        jmethodID setClip = env->GetMethodID(clipMgrCls, "setPrimaryClip", "(Landroid/content/ClipData;)V");
        if (!newPlain || !setClip) break;

        jstring label = env->NewStringUTF("MLBB ESP logs");
        jstring data = env->NewStringUTF(text);
        jobject clip = env->CallStaticObjectMethod(clipDataCls, newPlain, label, data);
        if (!clip) break;
        env->CallVoidMethod(clipMgr, setClip, clip);
        ok = !env->ExceptionCheck();
        if (!ok) env->ExceptionClear();
    } while (false);

    vm->DetachCurrentThread();
    return ok;
}
