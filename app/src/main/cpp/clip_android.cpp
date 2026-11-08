#include "clip_android.hpp"
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
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
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    return ok;
}

bool android_read_asset(const char *name, std::vector<uint8_t> &out) {
    out.clear();
    if (!g_clipVm || !g_clipCtx || !name) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    jclass ctxCls = env->FindClass("android/content/Context");
    if (!ctxCls) return false;
    jmethodID getAssets = env->GetMethodID(ctxCls, "getAssets", "()Landroid/content/res/AssetManager;");
    if (!getAssets) return false;
    jobject assets = env->CallObjectMethod(g_clipCtx, getAssets);
    if (env->ExceptionCheck() || !assets) {
        env->ExceptionClear();
        return false;
    }
    AAssetManager *mgr = AAssetManager_fromJava(env, assets);
    if (!mgr) return false;
    AAsset *asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!asset) return false;
    const off_t len = AAsset_getLength(asset);
    if (len <= 0) {
        AAsset_close(asset);
        return false;
    }
    out.resize((size_t)len);
    const int n = AAsset_read(asset, out.data(), (size_t)len);
    AAsset_close(asset);
    return n == (int)len;
}

bool android_copy_to_clipboard(const char *text) {
    if (!g_clipVm || !g_clipCtx) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    return copy_with_context(env, g_clipCtx, text);
}
