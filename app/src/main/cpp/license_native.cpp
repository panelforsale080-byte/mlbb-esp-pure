#include "license_native.hpp"
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <jni.h>
#include <string>
#include <vector>

namespace {

/** Runtime key so the pinned URLs are not stored as plain text. */
static uint8_t url_key() {
    volatile uint32_t a = 0x6C2E91A3u;
    volatile uint32_t b = 0x6C2E91F9u;
    return (uint8_t)((a ^ b) & 0xFFu);
}

static const uint8_t kLicEnc[] = {
    0x32, 0x2e, 0x2e, 0x2a, 0x29, 0x60, 0x75, 0x75, 0x28, 0x3b, 0x2d, 0x74, 0x3d, 0x33, 0x2e, 0x32,
    0x2f, 0x38, 0x2f, 0x29, 0x3f, 0x28, 0x39, 0x35, 0x34, 0x2e, 0x3f, 0x34, 0x2e, 0x74, 0x39, 0x35,
    0x37, 0x75, 0x1b, 0x22, 0x39, 0x3f, 0x36, 0x2a, 0x35, 0x3d, 0x33, 0x6b, 0x63, 0x6a, 0x75, 0x0a,
    0x3b, 0x29, 0x29, 0x2d, 0x35, 0x28, 0x3e, 0x1b, 0x34, 0x3e, 0x16, 0x33, 0x39, 0x3f, 0x34, 0x29,
    0x3f, 0x75, 0x37, 0x3b, 0x33, 0x34, 0x75, 0x16, 0x33, 0x39, 0x3f, 0x34, 0x29, 0x3f};
static const uint8_t kLicHash[] = {
    0x2e, 0xe7, 0x72, 0x71, 0x6c, 0x1f, 0xc5, 0x6e, 0xda, 0x65, 0x7d, 0xfc, 0x62, 0x89, 0x07, 0x51,
    0xe5, 0x77, 0xa3, 0x14, 0x1a, 0x3d, 0x8e, 0x03, 0x8e, 0xdc, 0x05, 0x56, 0x3f, 0xb1, 0x8b, 0x91};
static const uint8_t kTreeEnc[] = {
    0x32, 0x2e, 0x2e, 0x2a, 0x29, 0x60, 0x75, 0x75, 0x3b, 0x2a, 0x33, 0x74, 0x3d, 0x33, 0x2e, 0x32,
    0x2f, 0x38, 0x74, 0x39, 0x35, 0x37, 0x75, 0x28, 0x3f, 0x2a, 0x35, 0x29, 0x75, 0x1b, 0x22, 0x39,
    0x3f, 0x36, 0x2a, 0x35, 0x3d, 0x33, 0x6b, 0x63, 0x6a, 0x75, 0x0a, 0x3b, 0x29, 0x29, 0x2d, 0x35,
    0x28, 0x3e, 0x1b, 0x34, 0x3e, 0x16, 0x33, 0x39, 0x3f, 0x34, 0x29, 0x3f, 0x75, 0x3d, 0x33, 0x2e,
    0x75, 0x2e, 0x28, 0x3f, 0x3f, 0x29, 0x75, 0x37, 0x3b, 0x33, 0x34, 0x65, 0x28, 0x3f, 0x39, 0x2f,
    0x28, 0x29, 0x33, 0x2c, 0x3f, 0x67, 0x6b};
static const uint8_t kTreeHash[] = {
    0x61, 0xc1, 0x32, 0x6d, 0x60, 0xa7, 0x5b, 0xb6, 0x6e, 0xbe, 0xe3, 0xf1, 0x04, 0x53, 0xd5, 0x39,
    0x46, 0x44, 0xee, 0x7b, 0x1d, 0x65, 0xe2, 0xc3, 0x45, 0x03, 0xaf, 0xfd, 0x2c, 0x64, 0xe3, 0x01};
static const uint8_t kRawEnc[] = {
    0x32, 0x2e, 0x2e, 0x2a, 0x29, 0x60, 0x75, 0x75, 0x28, 0x3b, 0x2d, 0x74, 0x3d, 0x33, 0x2e, 0x32,
    0x2f, 0x38, 0x2f, 0x29, 0x3f, 0x28, 0x39, 0x35, 0x34, 0x2e, 0x3f, 0x34, 0x2e, 0x74, 0x39, 0x35,
    0x37, 0x75, 0x1b, 0x22, 0x39, 0x3f, 0x36, 0x2a, 0x35, 0x3d, 0x33, 0x6b, 0x63, 0x6a, 0x75, 0x0a,
    0x3b, 0x29, 0x29, 0x2d, 0x35, 0x28, 0x3e, 0x1b, 0x34, 0x3e, 0x16, 0x33, 0x39, 0x3f, 0x34, 0x29,
    0x3f, 0x75, 0x37, 0x3b, 0x33, 0x34, 0x75};
static const uint8_t kRawHash[] = {
    0xd6, 0xc0, 0xd1, 0x2f, 0x2d, 0xa7, 0x2c, 0x64, 0xa5, 0x18, 0x26, 0xb4, 0xcf, 0x84, 0x54, 0x6b,
    0xf1, 0x7b, 0xd0, 0x28, 0x8d, 0x67, 0x0a, 0x0b, 0x03, 0xf2, 0x36, 0x10, 0xb8, 0x53, 0x65, 0x7c};

/** Only the pinned native fetch writes this. A Java true is ignored. */
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

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    uint8_t block[64];
    size_t off = 0;
    uint64_t bits = (uint64_t)len * 8;
    auto compress = [&](const uint8_t *p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    };
    while (len - off >= 64) {
        compress(data + off);
        off += 64;
    }
    size_t rem = len - off;
    std::memset(block, 0, sizeof(block));
    if (rem) std::memcpy(block, data + off, rem);
    block[rem] = 0x80;
    if (rem >= 56) {
        compress(block);
        std::memset(block, 0, sizeof(block));
    }
    for (int i = 0; i < 8; ++i) block[63 - i] = (uint8_t)((bits >> (8 * i)) & 0xFF);
    compress(block);
    uint32_t hs[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)hs[i];
    }
}

/** Decode a pinned URL. A swapped repo name fails the digest and returns nothing. */
static bool pinned_url(char *out, size_t cap, const uint8_t *enc, size_t n, const uint8_t expect[32]) {
    if (!out || n + 1 > cap) return false;
    const uint8_t key = url_key();
    for (size_t i = 0; i < n; ++i) out[i] = (char)(enc[i] ^ key);
    out[n] = '\0';
    uint8_t dig[32];
    sha256(reinterpret_cast<const uint8_t *>(out), n, dig);
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (uint8_t)(dig[i] ^ expect[i]);
    if (diff != 0) {
        std::memset(out, 0, cap);
        return false;
    }
    return true;
}

static bool jni_failed(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

static bool http_first_line(JNIEnv *env, const char *url, char *line, size_t cap) {
    if (!url || !line || cap < 2) return false;
    line[0] = '\0';
    jclass urlCls = env->FindClass("java/net/URL");
    if (!urlCls || jni_failed(env)) return false;
    jmethodID urlInit = env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V");
    jstring jurl = env->NewStringUTF(url);
    jobject urlObj = env->NewObject(urlCls, urlInit, jurl);
    if (jni_failed(env) || !urlObj) return false;
    jmethodID open = env->GetMethodID(urlCls, "openConnection", "()Ljava/net/URLConnection;");
    jobject conn = env->CallObjectMethod(urlObj, open);
    if (jni_failed(env) || !conn) return false;
    jclass httpCls = env->FindClass("java/net/HttpURLConnection");
    jmethodID setConn = env->GetMethodID(httpCls, "setConnectTimeout", "(I)V");
    jmethodID setRead = env->GetMethodID(httpCls, "setReadTimeout", "(I)V");
    jmethodID setMethod = env->GetMethodID(httpCls, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID setProp = env->GetMethodID(httpCls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID setFollow = env->GetMethodID(httpCls, "setInstanceFollowRedirects", "(Z)V");
    jmethodID getCode = env->GetMethodID(httpCls, "getResponseCode", "()I");
    jmethodID getStream = env->GetMethodID(httpCls, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID disconnect = env->GetMethodID(httpCls, "disconnect", "()V");
    env->CallVoidMethod(conn, setConn, 12000);
    env->CallVoidMethod(conn, setRead, 12000);
    jstring method = env->NewStringUTF("GET");
    env->CallVoidMethod(conn, setMethod, method);
    jstring uaKey = env->NewStringUTF("User-Agent");
    jstring uaVal = env->NewStringUTF("Xorel/2.0 (Android)");
    env->CallVoidMethod(conn, setProp, uaKey, uaVal);
    env->CallVoidMethod(conn, setFollow, JNI_TRUE);
    if (jni_failed(env)) {
        env->CallVoidMethod(conn, disconnect);
        env->ExceptionClear();
        return false;
    }
    jint code = env->CallIntMethod(conn, getCode);
    if (jni_failed(env) || code != 200) {
        env->CallVoidMethod(conn, disconnect);
        env->ExceptionClear();
        return false;
    }
    jobject stream = env->CallObjectMethod(conn, getStream);
    if (jni_failed(env) || !stream) {
        env->CallVoidMethod(conn, disconnect);
        env->ExceptionClear();
        return false;
    }
    jclass inCls = env->FindClass("java/io/InputStream");
    jmethodID read = env->GetMethodID(inCls, "read", "([B)I");
    jmethodID close = env->GetMethodID(inCls, "close", "()V");
    jbyteArray buf = env->NewByteArray(512);
    std::string acc;
    while (acc.size() < 4096) {
        jint n = env->CallIntMethod(stream, read, buf);
        if (jni_failed(env) || n <= 0) break;
        std::vector<jbyte> tmp((size_t)n);
        env->GetByteArrayRegion(buf, 0, n, tmp.data());
        acc.append(reinterpret_cast<char *>(tmp.data()), (size_t)n);
        if (acc.find('\n') != std::string::npos) break;
    }
    env->CallVoidMethod(stream, close);
    env->ExceptionClear();
    env->CallVoidMethod(conn, disconnect);
    env->ExceptionClear();
    size_t begin = 0;
    while (begin < acc.size() && (acc[begin] == ' ' || acc[begin] == '\t' || acc[begin] == '\r' ||
                                  acc[begin] == '\n'))
        ++begin;
    size_t end = begin;
    while (end < acc.size() && acc[end] != '\n' && acc[end] != '\r') ++end;
    while (end > begin && (acc[end - 1] == ' ' || acc[end - 1] == '\t')) --end;
    if (end <= begin) return false;
    size_t n = end - begin;
    if (n + 1 > cap) n = cap - 1;
    std::memcpy(line, acc.data() + begin, n);
    line[n] = '\0';
    return true;
}

static jstring pinned_jstring(JNIEnv *env, const uint8_t *enc, size_t n, const uint8_t expect[32]) {
    char url[256];
    if (!pinned_url(url, sizeof(url), enc, n, expect)) return nullptr;
    jstring out = env->NewStringUTF(url);
    std::memset(url, 0, sizeof(url));
    return out;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeVerifyLicense(JNIEnv *env, jclass, jstring password) {
    const char *pass = password ? env->GetStringUTFChars(password, nullptr) : "";
    const char *msg = "password";
    char url[256];
    char line[512];
    url[0] = '\0';
    if (pass && pass[0] && pinned_url(url, sizeof(url), kLicEnc, sizeof(kLicEnc), kLicHash)) {
        if (!http_first_line(env, url, line, sizeof(line))) {
            clear_license();
            msg = "network";
        } else {
            msg = judge_license(pass, line);
        }
    } else if (pass && pass[0]) {
        clear_license();
        msg = "invalid";
    } else {
        clear_license();
    }
    std::memset(url, 0, sizeof(url));
    std::memset(line, 0, sizeof(line));
    if (password) env->ReleaseStringUTFChars(password, pass);
    return env->NewStringUTF(msg);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeMusicTreeUrl(JNIEnv *env, jclass) {
    return pinned_jstring(env, kTreeEnc, sizeof(kTreeEnc), kTreeHash);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeMusicRawBase(JNIEnv *env, jclass) {
    return pinned_jstring(env, kRawEnc, sizeof(kRawEnc), kRawHash);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeSetLicenseAttested(JNIEnv *, jclass, jboolean ok) {
    if (ok != JNI_TRUE) clear_license();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mlbb_esp_pure_LicenseNative_nativeIsLicenseAttested(JNIEnv *, jclass) {
    return g_licenseToken.load(std::memory_order_acquire) == kLicenseOk ? JNI_TRUE : JNI_FALSE;
}

bool license_native_attested() {
    return g_licenseToken.load(std::memory_order_acquire) == kLicenseOk;
}
