#include "clip_android.hpp"
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cstdio>
#include <cstring>
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

static bool files_dir_abs(JNIEnv *env, char *out, size_t outCap) {
    if (!env || !g_clipCtx || !out || outCap == 0) return false;
    jclass ctxCls = env->FindClass("android/content/Context");
    jclass fileCls = env->FindClass("java/io/File");
    if (!ctxCls || !fileCls) return false;
    jmethodID getFilesDir = env->GetMethodID(ctxCls, "getFilesDir", "()Ljava/io/File;");
    jmethodID getAbsPath = env->GetMethodID(fileCls, "getAbsolutePath", "()Ljava/lang/String;");
    if (!getFilesDir || !getAbsPath) return false;
    jobject dir = env->CallObjectMethod(g_clipCtx, getFilesDir);
    if (env->ExceptionCheck() || !dir) {
        env->ExceptionClear();
        return false;
    }
    jstring path = (jstring)env->CallObjectMethod(dir, getAbsPath);
    if (!path) return false;
    const char *utf = env->GetStringUTFChars(path, nullptr);
    if (!utf) return false;
    std::snprintf(out, outCap, "%s", utf);
    env->ReleaseStringUTFChars(path, utf);
    return out[0] != '\0';
}

bool android_app_file_read(const char *relName, std::vector<uint8_t> &out) {
    out.clear();
    if (!g_clipVm || !g_clipCtx || !relName) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    char dir[512] = {};
    if (!files_dir_abs(env, dir, sizeof(dir))) return false;
    char path[640] = {};
    std::snprintf(path, sizeof(path), "%s/%s", dir, relName);
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 64 * 1024) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)len);
    const size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return n == out.size();
}

bool android_app_file_write(const char *relName, const char *text) {
    if (!g_clipVm || !g_clipCtx || !relName || !text) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    char dir[512] = {};
    if (!files_dir_abs(env, dir, sizeof(dir))) return false;
    char path[640] = {};
    std::snprintf(path, sizeof(path), "%s/%s", dir, relName);
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t n = std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
    return n == std::strlen(text);
}

bool android_write_file_path(const char *absPath, const char *text) {
    if (!absPath || !text) return false;
    FILE *f = std::fopen(absPath, "wb");
    if (!f) return false;
    const size_t n = std::strlen(text);
    const size_t w = n > 0 ? std::fwrite(text, 1, n, f) : 0;
    std::fclose(f);
    return w == n;
}

bool android_read_file_path(const char *absPath, std::vector<uint8_t> &out) {
    out.clear();
    if (!absPath || !absPath[0]) return false;
    FILE *f = std::fopen(absPath, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 256 * 1024) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)len);
    const size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return n == out.size();
}

bool android_read_clipboard(char *out, size_t outSz) {
    if (!out || outSz == 0) return false;
    out[0] = '\0';
    if (!g_clipVm || !g_clipCtx) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;

    jclass ctxCls = env->FindClass("android/content/Context");
    jclass clipMgrCls = env->FindClass("android/content/ClipboardManager");
    jclass clipDataCls = env->FindClass("android/content/ClipData");
    jclass charSeqCls = env->FindClass("java/lang/CharSequence");
    if (!ctxCls || !clipMgrCls || !clipDataCls || !charSeqCls) return false;

    jfieldID clipSvc = env->GetStaticFieldID(ctxCls, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
    jmethodID getSvc = env->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID hasClip = env->GetMethodID(clipMgrCls, "hasPrimaryClip", "()Z");
    jmethodID getClip = env->GetMethodID(clipMgrCls, "getPrimaryClip", "()Landroid/content/ClipData;");
    jmethodID getItem = env->GetMethodID(clipDataCls, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    if (!clipSvc || !getSvc || !hasClip || !getClip || !getItem) return false;

    jstring svcName = (jstring)env->GetStaticObjectField(ctxCls, clipSvc);
    jobject clipMgr = env->CallObjectMethod(g_clipCtx, getSvc, svcName);
    if (env->ExceptionCheck() || !clipMgr) {
        env->ExceptionClear();
        return false;
    }
    if (!env->CallBooleanMethod(clipMgr, hasClip)) return false;
    jobject clip = env->CallObjectMethod(clipMgr, getClip);
    if (env->ExceptionCheck() || !clip) {
        env->ExceptionClear();
        return false;
    }
    jobject item = env->CallObjectMethod(clip, getItem, 0);
    if (!item) return false;
    jclass itemCls = env->FindClass("android/content/ClipData$Item");
    if (!itemCls) return false;
    jmethodID itemCoerce =
        env->GetMethodID(itemCls, "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;");
    if (!itemCoerce) return false;
    jobject textObj = env->CallObjectMethod(item, itemCoerce, g_clipCtx);
    if (env->ExceptionCheck() || !textObj) {
        env->ExceptionClear();
        return false;
    }
    jmethodID toStr = env->GetMethodID(charSeqCls, "toString", "()Ljava/lang/String;");
    if (!toStr) return false;
    jstring jtext = (jstring)env->CallObjectMethod(textObj, toStr);
    if (!jtext) return false;
    const char *utf = env->GetStringUTFChars(jtext, nullptr);
    if (!utf) return false;
    std::snprintf(out, outSz, "%s", utf);
    env->ReleaseStringUTFChars(jtext, utf);
    while (out[0]) {
        const size_t n = std::strlen(out);
        if (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[n - 1] = '\0';
        else break;
    }
    return out[0] != '\0';
}

bool android_copy_to_clipboard(const char *text) {
    if (!g_clipVm || !g_clipCtx) return false;
    JNIEnv *env = nullptr;
    if (g_clipVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    return copy_with_context(env, g_clipCtx, text);
}
