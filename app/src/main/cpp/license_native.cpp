#include "license_native.hpp"
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <jni.h>
#include <vector>

namespace {

static const uint8_t kPepperEnc[] = {
    0x22, 0x35, 0x28, 0x3f, 0x36, 0x74, 0x2a, 0x3f, 0x2a, 0x2a, 0x3f, 0x28, 0x74, 0x2c, 0x68, 0x74,
    0x62, 0x3c, 0x69, 0x39};
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

/** Only nativeSubmitLicense writes this. A Java true is ignored. */
static std::atomic<uint32_t> g_licenseToken{0};
constexpr uint32_t kLicenseOk = 0xC3A15E27u;

static void clear_license() { g_licenseToken.store(0, std::memory_order_release); }

static bool grant_license() {
    g_licenseToken.store(kLicenseOk, std::memory_order_release);
    return true;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    return s;
}

static bool read_int(const char *&s, int *out) {
    if (*s < '0' || *s > '9') return false;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    *out = v;
    return true;
}

/** PASSWORD YYYY/MM/DD with an optional ignored trailing token. */
static bool parse_license_line(const char *line, char *pass, size_t passCap, int *y, int *mo,
                               int *d) {
    if (!line || !pass || passCap < 2) return false;
    line = skip_ws(line);
    size_t n = 0;
    while (*line && *line != ' ' && *line != '\t' && n + 1 < passCap) pass[n++] = *line++;
    pass[n] = '\0';
    if (n == 0) return false;
    line = skip_ws(line);
    if (!read_int(line, y) || *y < 2000 || *y > 2099) return false;
    if (*line != '/' && *line != '-') return false;
    ++line;
    if (!read_int(line, mo) || *mo < 1 || *mo > 12) return false;
    if (*line != '/' && *line != '-') return false;
    ++line;
    if (!read_int(line, d) || *d < 1 || *d > 31) return false;
    return true;
}

static bool same_text(const char *a, const char *b) {
    if (!a || !b) return false;
    const size_t na = std::strlen(a);
    const size_t nb = std::strlen(b);
    if (na != nb) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < na; ++i) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static bool expired_utc(int y, int mo, int d) {
    std::tm end{};
    end.tm_year = y - 1900;
    end.tm_mon = mo - 1;
    end.tm_mday = d;
    end.tm_hour = 23;
    end.tm_min = 59;
    end.tm_sec = 59;
    const time_t endSec = timegm(&end);
    if (endSec == (time_t)-1) return true;
    return std::time(nullptr) > endSec;
}

static const char *judge_license(const char *userPass, const char *bodyLine) {
    clear_license();
    if (!userPass || !userPass[0]) return "password";
    char remote[128];
    int y = 0, mo = 0, d = 0;
    if (!parse_license_line(bodyLine, remote, sizeof(remote), &y, &mo, &d)) return "invalid";
    if (!same_text(userPass, remote)) return "password";
    if (expired_utc(y, mo, d)) return "expired";
    grant_license();
    return "ok";
}

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
    char pepper[48];
    xor_decode(kPepperEnc, sizeof(kPepperEnc), 0x5a, pepper, sizeof(pepper));
    return env->NewStringUTF(pepper);
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
    if (ok != JNI_TRUE) clear_license();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeSubmitLicense(JNIEnv *env, jclass, jstring password,
                                                         jstring bodyLine) {
    const char *pass = password ? env->GetStringUTFChars(password, nullptr) : "";
    const char *body = bodyLine ? env->GetStringUTFChars(bodyLine, nullptr) : "";
    const char *msg = judge_license(pass ? pass : "", body ? body : "");
    if (password) env->ReleaseStringUTFChars(password, pass);
    if (bodyLine) env->ReleaseStringUTFChars(bodyLine, body);
    return env->NewStringUTF(msg);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeIsLicenseAttested(JNIEnv *, jclass) {
    return g_licenseToken.load(std::memory_order_acquire) == kLicenseOk ? JNI_TRUE : JNI_FALSE;
}

bool license_native_attested() {
    return g_licenseToken.load(std::memory_order_acquire) == kLicenseOk;
}
