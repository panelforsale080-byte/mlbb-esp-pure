#include "license_native.hpp"
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <jni.h>
#include <vector>

namespace {

constexpr char kPepper[] = "xorel.pepper.v2.8f3c";
constexpr char kPasteId[] = "PasswordAndLicense";
/** Obfuscated GitHub raw License file (xor 0x5A). */
static const uint8_t kUrlEnc[] = {
    0x32, 0x2e, 0x2e, 0x2a, 0x29, 0x60, 0x75, 0x75, 0x28, 0x3b, 0x2d, 0x74, 0x3d, 0x33, 0x2e, 0x32,
    0x2f, 0x38, 0x2f, 0x29, 0x3f, 0x28, 0x39, 0x35, 0x34, 0x2e, 0x3f, 0x34, 0x2e, 0x74, 0x39, 0x35,
    0x37, 0x75, 0x1b, 0x22, 0x39, 0x3f, 0x36, 0x2a, 0x35, 0x3d, 0x33, 0x6b, 0x63, 0x6a, 0x75, 0x0a,
    0x3b, 0x29, 0x29, 0x2d, 0x35, 0x28, 0x3e, 0x1b, 0x34, 0x3e, 0x16, 0x33, 0x39, 0x3f, 0x34, 0x29,
    0x3f, 0x75, 0x37, 0x3b, 0x33, 0x34, 0x75, 0x16, 0x33, 0x39, 0x3f, 0x34, 0x29, 0x3f};
static const uint8_t kKeyXor[] = {
    0x0b, 0xe7, 0x4a, 0xa8, 0x06, 0x0f, 0xcb, 0xbd, 0x99, 0x08, 0x13, 0x09, 0xf8, 0x37, 0x31, 0x23,
    0x41, 0x92, 0x69, 0xe3, 0xeb, 0xda, 0x54, 0x64, 0x94, 0x3a, 0x0a, 0x01, 0x96, 0x96, 0x79, 0x49};

static std::atomic<bool> g_licenseAttested{false};

static void xor_decode(const uint8_t *enc, size_t n, uint8_t xorByte, char *out, size_t outCap) {
    size_t i = 0;
    for (; i < n && i + 1 < outCap; ++i) out[i] = (char)(enc[i] ^ xorByte);
    out[i] = '\0';
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeGetFetchUrl(JNIEnv *env, jclass) {
    char url[192];
    xor_decode(kUrlEnc, sizeof(kUrlEnc), 0x5a, url, sizeof(url));
    return env->NewStringUTF(url);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeGetHmacPepper(JNIEnv *env, jclass) {
    return env->NewStringUTF(kPepper);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeGetBoundPasteId(JNIEnv *env, jclass) {
    return env->NewStringUTF(kPasteId);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeGetHmacKey(JNIEnv *env, jclass) {
    std::vector<jbyte> key(sizeof(kKeyXor));
    for (size_t i = 0; i < sizeof(kKeyXor); ++i) key[i] = (jbyte)(kKeyXor[i] ^ 0xa7);
    jbyteArray arr = env->NewByteArray((jsize)key.size());
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)key.size(), key.data());
    return arr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeSetLicenseAttested(JNIEnv *, jclass, jboolean ok) {
    g_licenseAttested.store(ok == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeIsLicenseAttested(JNIEnv *, jclass) {
    return g_licenseAttested.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

bool license_native_attested() {
    return g_licenseAttested.load(std::memory_order_acquire);
}
