#include "esp_device_id.hpp"
#include "clip_android.hpp"
#include "conrent_esp.hpp"
#include "esp_engine.hpp"
#include "mem.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"

#include "imgui.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace DumpOff {
constexpr uintptr_t GscJsonDeviceId = 0x118;
constexpr uintptr_t GscUniqueId = 0x120;
constexpr uintptr_t GscAndroidId = 0x128;
constexpr uintptr_t GscAdvertisingId = 0x130;
constexpr uintptr_t GscReportDeviceId = 0x140;
constexpr uintptr_t GscStaticInstance = 0xd0;

constexpr uintptr_t SdkChannel = 0x20;
constexpr uintptr_t SdkDeviceId = 0x28;
constexpr uintptr_t SdkAppUuid = 0x30;
constexpr uintptr_t SdkOaid = 0x58;
constexpr uintptr_t SdkUsUnique = 0x200;
constexpr uintptr_t SdkUsAndroid = 0x208;
constexpr uintptr_t SdkUsAdvertising = 0x210;

constexpr uintptr_t UtilAndroidId = 0x120;
constexpr uintptr_t UtilAdvertisingId = 0x128;
constexpr uintptr_t UtilRealAdvertisingId = 0x130;
constexpr uintptr_t UtilMac = 0x140;
constexpr uintptr_t UtilTelephony = 0x148;
} // namespace DumpOff

static pid_t g_pid = 0;
static uintptr_t g_gscKlass = 0;
static uintptr_t g_sdkKlass = 0;
static uintptr_t g_utilKlass = 0;
static std::mutex g_mu;
static EspDeviceIdSnap g_snap;
static EspDeviceSpoofCfg g_spoof;

static constexpr char kSpoofFile[] = "device_spoof.cfg";

EspDeviceSpoofCfg &esp_device_id_spoof_cfg() { return g_spoof; }

static void trim_line(char *s) {
    if (!s) return;
    size_t n = std::strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') ++i;
    if (i > 0) std::memmove(s, s + i, std::strlen(s + i) + 1);
}

static size_t utf8_to_utf16le(const char *utf8, uint16_t *out, size_t maxUnits) {
    if (!utf8 || !out || maxUnits == 0) return 0;
    size_t o = 0;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8);
    while (*p && o < maxUnits) {
        uint32_t c = 0;
        if (*p < 0x80) {
            c = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            c = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            c = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            c = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            ++p;
            continue;
        }
        if (c <= 0xFFFF) out[o++] = (uint16_t)c;
    }
    return o;
}

static bool patch_il2_string_inplace(pid_t pid, uintptr_t strObj, const char *utf8) {
    if (!strObj || !mem_ptr_ok(strObj) || !utf8) return false;
    int32_t oldLen = 0;
    if (!mem_read(pid, strObj + 0x10, oldLen) || oldLen <= 0 || oldLen > 384) return false;
    uint16_t units[384];
    const size_t newLen = utf8_to_utf16le(utf8, units, sizeof(units) / sizeof(units[0]));
    if (newLen == 0 || newLen > (size_t)oldLen) return false;
    uint8_t blob[4 + 384 * 2] = {};
    const int32_t wlen = (int32_t)newLen;
    std::memcpy(blob, &wlen, sizeof(wlen));
    for (size_t i = 0; i < newLen; ++i) {
        blob[4 + i * 2] = (uint8_t)(units[i] & 0xFF);
        blob[4 + i * 2 + 1] = (uint8_t)(units[i] >> 8);
    }
    return mem_write(pid, strObj + 0x10, blob, 4 + newLen * 2);
}

static bool write_managed_string_at(pid_t pid, RemoteIl2CppInfo &il2, RemoteSession *ptraceS,
                                    bool allowPtrace, uintptr_t holder, uintptr_t fieldOff,
                                    const char *utf8) {
    if (!holder || !utf8 || !utf8[0]) return false;
    uintptr_t strObj = 0;
    mem_read(pid, holder + fieldOff, strObj);
    if (strObj && patch_il2_string_inplace(pid, strObj, utf8)) return true;
    if (!allowPtrace || !ptraceS || !ptraceS->attached) return false;
    uintptr_t neu = 0;
    if (!conrent_il2_string_new_session(*ptraceS, pid, il2, utf8, neu) || !mem_ptr_ok(neu))
        return false;
    return mem_write(pid, holder + fieldOff, &neu, sizeof(neu));
}

static void spoof_load_from_disk() {
    std::vector<uint8_t> raw;
    if (!android_app_file_read(kSpoofFile, raw)) {
        g_spoof.loaded = true;
        return;
    }
    raw.push_back(0);
    char *buf = reinterpret_cast<char *>(raw.data());
    char *save = nullptr;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        trim_line(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        char *val = eq + 1;
        trim_line(const_cast<char *>(key));
        trim_line(val);
        if (std::strcmp(key, "enabled") == 0)
            g_spoof.enabled = std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0 ||
                              std::strcmp(val, "yes") == 0;
        else if (std::strcmp(key, "gsc_json") == 0)
            std::snprintf(g_spoof.gscJson, sizeof(g_spoof.gscJson), "%s", val);
        else if (std::strcmp(key, "gsc_report") == 0)
            std::snprintf(g_spoof.gscReport, sizeof(g_spoof.gscReport), "%s", val);
        else if (std::strcmp(key, "gsc_unique") == 0)
            std::snprintf(g_spoof.gscUnique, sizeof(g_spoof.gscUnique), "%s", val);
        else if (std::strcmp(key, "gsc_android") == 0)
            std::snprintf(g_spoof.gscAndroid, sizeof(g_spoof.gscAndroid), "%s", val);
        else if (std::strcmp(key, "gsc_ad") == 0)
            std::snprintf(g_spoof.gscAd, sizeof(g_spoof.gscAd), "%s", val);
        else if (std::strcmp(key, "sdk_device") == 0)
            std::snprintf(g_spoof.sdkDeviceId, sizeof(g_spoof.sdkDeviceId), "%s", val);
        else if (std::strcmp(key, "sdk_unique") == 0)
            std::snprintf(g_spoof.sdkUnique, sizeof(g_spoof.sdkUnique), "%s", val);
        else if (std::strcmp(key, "sdk_android") == 0)
            std::snprintf(g_spoof.sdkAndroid, sizeof(g_spoof.sdkAndroid), "%s", val);
        else if (std::strcmp(key, "sdk_ad") == 0)
            std::snprintf(g_spoof.sdkAd, sizeof(g_spoof.sdkAd), "%s", val);
        else if (std::strcmp(key, "sdk_oaid") == 0)
            std::snprintf(g_spoof.sdkOaid, sizeof(g_spoof.sdkOaid), "%s", val);
        else if (std::strcmp(key, "sdk_channel") == 0)
            std::snprintf(g_spoof.sdkChannel, sizeof(g_spoof.sdkChannel), "%s", val);
        else if (std::strcmp(key, "util_android") == 0)
            std::snprintf(g_spoof.utilAndroid, sizeof(g_spoof.utilAndroid), "%s", val);
        else if (std::strcmp(key, "util_ad") == 0)
            std::snprintf(g_spoof.utilAd, sizeof(g_spoof.utilAd), "%s", val);
        else if (std::strcmp(key, "util_telephony") == 0)
            std::snprintf(g_spoof.utilTelephony, sizeof(g_spoof.utilTelephony), "%s", val);
        else if (std::strcmp(key, "util_mac") == 0)
            std::snprintf(g_spoof.utilMac, sizeof(g_spoof.utilMac), "%s", val);
    }
    g_spoof.loaded = true;
}

static void spoof_put_line(std::string &body, const char *key, const char *val) {
    body += key;
    body += '=';
    body += val ? val : "";
    body += '\n';
}

bool esp_device_id_spoof_save() {
    std::string body;
    body += "# Spoof IDs — overlay app only; empty value = skip on apply\n";
    spoof_put_line(body, "enabled", g_spoof.enabled ? "1" : "0");
    spoof_put_line(body, "gsc_json", g_spoof.gscJson);
    spoof_put_line(body, "gsc_report", g_spoof.gscReport);
    spoof_put_line(body, "gsc_unique", g_spoof.gscUnique);
    spoof_put_line(body, "gsc_android", g_spoof.gscAndroid);
    spoof_put_line(body, "gsc_ad", g_spoof.gscAd);
    spoof_put_line(body, "sdk_device", g_spoof.sdkDeviceId);
    spoof_put_line(body, "sdk_unique", g_spoof.sdkUnique);
    spoof_put_line(body, "sdk_android", g_spoof.sdkAndroid);
    spoof_put_line(body, "sdk_ad", g_spoof.sdkAd);
    spoof_put_line(body, "sdk_oaid", g_spoof.sdkOaid);
    spoof_put_line(body, "sdk_channel", g_spoof.sdkChannel);
    spoof_put_line(body, "util_android", g_spoof.utilAndroid);
    spoof_put_line(body, "util_ad", g_spoof.utilAd);
    spoof_put_line(body, "util_telephony", g_spoof.utilTelephony);
    spoof_put_line(body, "util_mac", g_spoof.utilMac);
    return android_app_file_write(kSpoofFile, body.c_str());
}

void esp_device_id_spoof_fill_from_snapshot() {
    const EspDeviceIdSnap &s = esp_device_id_snapshot();
    std::snprintf(g_spoof.gscJson, sizeof(g_spoof.gscJson), "%s", s.gameServerJsonId);
    std::snprintf(g_spoof.gscReport, sizeof(g_spoof.gscReport), "%s", s.gameServerReportId);
    std::snprintf(g_spoof.gscUnique, sizeof(g_spoof.gscUnique), "%s", s.gameServerUniqueId);
    std::snprintf(g_spoof.gscAndroid, sizeof(g_spoof.gscAndroid), "%s", s.gameServerAndroidId);
    std::snprintf(g_spoof.gscAd, sizeof(g_spoof.gscAd), "%s", s.gameServerAdId);
    std::snprintf(g_spoof.sdkDeviceId, sizeof(g_spoof.sdkDeviceId), "%s", s.sdkDeviceId);
    std::snprintf(g_spoof.sdkUnique, sizeof(g_spoof.sdkUnique), "%s", s.sdkUniqueId);
    std::snprintf(g_spoof.sdkAndroid, sizeof(g_spoof.sdkAndroid), "%s", s.sdkAndroidId);
    std::snprintf(g_spoof.sdkAd, sizeof(g_spoof.sdkAd), "%s", s.sdkAdvertisingId);
    std::snprintf(g_spoof.sdkOaid, sizeof(g_spoof.sdkOaid), "%s", s.sdkOaid);
    std::snprintf(g_spoof.sdkChannel, sizeof(g_spoof.sdkChannel), "%s", s.sdkChannel);
    std::snprintf(g_spoof.utilAndroid, sizeof(g_spoof.utilAndroid), "%s", s.utilAndroidId);
    std::snprintf(g_spoof.utilAd, sizeof(g_spoof.utilAd), "%s", s.utilAdId);
    std::snprintf(g_spoof.utilTelephony, sizeof(g_spoof.utilTelephony), "%s", s.utilTelephonyId);
    std::snprintf(g_spoof.utilMac, sizeof(g_spoof.utilMac), "%s", s.utilMac);
}

static bool resolve_gsc_instance(pid_t pid, uintptr_t gscKlass, uintptr_t &inst) {
    inst = 0;
    if (!gscKlass) return false;
    const uintptr_t st = esp_bm_static_fields_ptr(pid, gscKlass);
    if (st) mem_read(pid, st + DumpOff::GscStaticInstance, inst);
    return inst != 0 && mem_ptr_ok(inst);
}

bool esp_device_id_apply_spoof(pid_t pid, bool requireEnabled, bool allowPtrace) {
    if (pid <= 0) return false;
    if (requireEnabled && !g_spoof.enabled) return false;

    uintptr_t gscKlass = 0;
    uintptr_t sdkKlass = 0;
    uintptr_t utilKlass = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        gscKlass = g_gscKlass;
        sdkKlass = g_sdkKlass;
        utilKlass = g_utilKlass;
    }
    RemoteIl2CppInfo il2{};
    if (!esp_resolved_il2(il2)) return false;

    RemoteSession ptraceS{};
    RemoteSession *ptracePtr = nullptr;
    if (allowPtrace && il2.sym_string_new && remote_session_begin_try(pid, ptraceS))
        ptracePtr = &ptraceS;

    int ok = 0;
    auto writeBump = [&](bool w) {
        if (w) ++ok;
    };
    auto wStr = [&](uintptr_t holder, uintptr_t off, const char *val) {
        return write_managed_string_at(pid, il2, ptracePtr, allowPtrace, holder, off, val);
    };

    uintptr_t gscInst = 0;
    if (gscKlass) resolve_gsc_instance(pid, gscKlass, gscInst);
    if (gscInst) {
        if (g_spoof.gscJson[0])
            writeBump(wStr(gscInst, DumpOff::GscJsonDeviceId, g_spoof.gscJson));
        if (g_spoof.gscReport[0])
            writeBump(wStr(gscInst, DumpOff::GscReportDeviceId, g_spoof.gscReport));
        if (g_spoof.gscUnique[0])
            writeBump(wStr(gscInst, DumpOff::GscUniqueId, g_spoof.gscUnique));
        if (g_spoof.gscAndroid[0])
            writeBump(wStr(gscInst, DumpOff::GscAndroidId, g_spoof.gscAndroid));
        if (g_spoof.gscAd[0])
            writeBump(wStr(gscInst, DumpOff::GscAdvertisingId, g_spoof.gscAd));
    }

    if (sdkKlass) {
        const uintptr_t st = esp_bm_static_fields_ptr(pid, sdkKlass);
        if (st) {
            if (g_spoof.sdkDeviceId[0])
                writeBump(wStr(st, DumpOff::SdkDeviceId, g_spoof.sdkDeviceId));
            if (g_spoof.sdkUnique[0])
                writeBump(wStr(st, DumpOff::SdkUsUnique, g_spoof.sdkUnique));
            if (g_spoof.sdkAndroid[0])
                writeBump(wStr(st, DumpOff::SdkUsAndroid, g_spoof.sdkAndroid));
            if (g_spoof.sdkAd[0])
                writeBump(wStr(st, DumpOff::SdkUsAdvertising, g_spoof.sdkAd));
            if (g_spoof.sdkOaid[0])
                writeBump(wStr(st, DumpOff::SdkOaid, g_spoof.sdkOaid));
            if (g_spoof.sdkChannel[0])
                writeBump(wStr(st, DumpOff::SdkChannel, g_spoof.sdkChannel));
        }
    }

    if (utilKlass) {
        const uintptr_t st = esp_bm_static_fields_ptr(pid, utilKlass);
        if (st) {
            if (g_spoof.utilAndroid[0])
                writeBump(wStr(st, DumpOff::UtilAndroidId, g_spoof.utilAndroid));
            if (g_spoof.utilAd[0]) {
                writeBump(wStr(st, DumpOff::UtilAdvertisingId, g_spoof.utilAd));
                writeBump(wStr(st, DumpOff::UtilRealAdvertisingId, g_spoof.utilAd));
            }
            if (g_spoof.utilTelephony[0])
                writeBump(wStr(st, DumpOff::UtilTelephony, g_spoof.utilTelephony));
            if (g_spoof.utilMac[0])
                writeBump(wStr(st, DumpOff::UtilMac, g_spoof.utilMac));
        }
    }

    if (ptracePtr) remote_session_end(ptraceS);

    if (ok > 0)
        ui_log("[device] spoof applied (%d writes)%s", ok,
               allowPtrace ? "" : " (in-place only)");
    return ok > 0;
}

void esp_device_id_on_resolve(pid_t pid, uintptr_t gameServerConfigKlass, uintptr_t sdkCommonKlass,
                              uintptr_t deviceUtilKlass) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = pid;
    g_gscKlass = gameServerConfigKlass;
    g_sdkKlass = sdkCommonKlass;
    g_utilKlass = deviceUtilKlass;
}

void esp_device_id_clear() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = 0;
    g_gscKlass = 0;
    g_sdkKlass = 0;
    g_utilKlass = 0;
    g_snap = {};
}

static bool read_il2_str(pid_t pid, uintptr_t strObj, char *out, size_t outSz) {
    if (!out || outSz == 0) return false;
    out[0] = '\0';
    if (!strObj || !mem_ptr_ok(strObj)) return false;
    alignas(4) uint8_t blob[4 + 384 * 2];
    if (!mem_read(pid, strObj + 0x10, blob, sizeof(blob))) return false;
    int32_t len = 0;
    std::memcpy(&len, blob, sizeof(len));
    if (len <= 0 || len > 384) return false;
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 4 < outSz; ++i) {
        uint16_t cu = 0;
        std::memcpy(&cu, blob + 4 + (size_t)i * 2, sizeof(cu));
        if (cu < 0x80) out[o++] = (char)cu;
        else if (cu < 0x800) {
            out[o++] = (char)(0xC0 | (cu >> 6));
            out[o++] = (char)(0x80 | (cu & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (cu >> 12));
            out[o++] = (char)(0x80 | ((cu >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cu & 0x3F));
        }
    }
    out[o < outSz ? o : outSz - 1] = '\0';
    return out[0] != '\0';
}

static bool read_field_str(pid_t pid, uintptr_t inst, uintptr_t off, char *out, size_t n) {
    uintptr_t p = 0;
    if (!inst || !off || !mem_read(pid, inst + off, p)) return false;
    return read_il2_str(pid, p, out, n);
}

static bool read_static_off_str(pid_t pid, uintptr_t klass, uintptr_t fieldOff, char *out,
                                size_t n) {
    if (!klass || !fieldOff || !out || n == 0) return false;
    const uintptr_t st = esp_bm_static_fields_ptr(pid, klass);
    if (!st) return false;
    uintptr_t strObj = 0;
    if (!mem_read(pid, st + fieldOff, strObj) || !strObj) return false;
    return read_il2_str(pid, strObj, out, n);
}

static void fill_static_off_if_empty(pid_t pid, uintptr_t klass, uintptr_t off, char *out,
                                     size_t n) {
    if (!out || out[0] || !klass) return;
    read_static_off_str(pid, klass, off, out, n);
}

static bool read_static_named(pid_t pid, RemoteIl2CppInfo &il2, uintptr_t klass,
                              const char *fieldName, uintptr_t fieldOff, char *out, size_t n) {
    if (!klass || !fieldName || !out || n == 0) return false;
    out[0] = '\0';
    uintptr_t strObj = 0;
    if (conrent_il2_static_field_ptr(pid, il2, klass, fieldName, strObj) && strObj)
        return read_il2_str(pid, strObj, out, n);
    if (fieldOff) return read_static_off_str(pid, klass, fieldOff, out, n);
    return false;
}

static void try_static_names(pid_t pid, RemoteIl2CppInfo &il2, uintptr_t klass,
                             const char *const *names, size_t count, uintptr_t fieldOff, char *out,
                             size_t n) {
    if (!klass || !out || n == 0) return;
    for (size_t i = 0; i < count; ++i) {
        if (out[0]) return;
        read_static_named(pid, il2, klass, names[i], fieldOff, out, n);
    }
    fill_static_off_if_empty(pid, klass, fieldOff, out, n);
}

void esp_device_id_refresh(pid_t pid) {
    EspDeviceIdSnap snap{};
    snap.refreshed = true;
    uintptr_t gscKlass = 0;
    uintptr_t sdkKlass = 0;
    uintptr_t utilKlass = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        gscKlass = g_gscKlass;
        sdkKlass = g_sdkKlass;
        utilKlass = g_utilKlass;
    }
    if (pid <= 0) {
        std::snprintf(snap.hint, sizeof(snap.hint), "Attach game pid first.");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = snap;
        return;
    }
    RemoteIl2CppInfo il2{};
    if (!esp_resolved_il2(il2)) {
        std::snprintf(snap.hint, sizeof(snap.hint), "Tap Resolve (game running).");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = snap;
        return;
    }
    if (!gscKlass && !sdkKlass && !utilKlass) {
        std::snprintf(snap.hint, sizeof(snap.hint), "Device classes not found — Resolve again.");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = snap;
        return;
    }

    if (g_spoof.enabled) {
        static uint64_t s_lastInplaceMs = 0;
        const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        if (nowMs - s_lastInplaceMs >= 15000) {
            esp_device_id_apply_spoof(pid, true, false);
            s_lastInplaceMs = nowMs;
        }
    }

    if (sdkKlass) {
        static const char *kDevId[] = {"_deviceId", "<_deviceId>k__BackingField"};
        static const char *kUnique[] = {"_us_id_device_uniqueid", "_sAppUUId"};
        static const char *kAnd[] = {"_us_id_android_id", "_strAndroidID"};
        static const char *kAd[] = {"_us_id_advertising_id", "_strAdvertisingID"};
        static const char *kOaid[] = {"_sOAID", "_us_id_appset_id"};
        static const char *kChan[] = {"_channel"};
        try_static_names(pid, il2, sdkKlass, kDevId, sizeof(kDevId) / sizeof(kDevId[0]),
                         DumpOff::SdkDeviceId, snap.sdkDeviceId, sizeof(snap.sdkDeviceId));
        try_static_names(pid, il2, sdkKlass, kUnique, sizeof(kUnique) / sizeof(kUnique[0]),
                         DumpOff::SdkUsUnique, snap.sdkUniqueId, sizeof(snap.sdkUniqueId));
        if (!snap.sdkUniqueId[0])
            fill_static_off_if_empty(pid, sdkKlass, DumpOff::SdkAppUuid, snap.sdkUniqueId,
                                     sizeof(snap.sdkUniqueId));
        try_static_names(pid, il2, sdkKlass, kAnd, sizeof(kAnd) / sizeof(kAnd[0]),
                         DumpOff::SdkUsAndroid, snap.sdkAndroidId, sizeof(snap.sdkAndroidId));
        try_static_names(pid, il2, sdkKlass, kAd, sizeof(kAd) / sizeof(kAd[0]),
                         DumpOff::SdkUsAdvertising, snap.sdkAdvertisingId,
                         sizeof(snap.sdkAdvertisingId));
        try_static_names(pid, il2, sdkKlass, kOaid, sizeof(kOaid) / sizeof(kOaid[0]),
                         DumpOff::SdkOaid, snap.sdkOaid, sizeof(snap.sdkOaid));
        try_static_names(pid, il2, sdkKlass, kChan, sizeof(kChan) / sizeof(kChan[0]),
                         DumpOff::SdkChannel, snap.sdkChannel, sizeof(snap.sdkChannel));
    }

    if (utilKlass) {
        read_static_named(pid, il2, utilKlass, "_strAndroidID", DumpOff::UtilAndroidId,
                          snap.utilAndroidId, sizeof(snap.utilAndroidId));
        read_static_named(pid, il2, utilKlass, "_strAdvertisingID", DumpOff::UtilAdvertisingId,
                          snap.utilAdId, sizeof(snap.utilAdId));
        read_static_named(pid, il2, utilKlass, "_strTelephonyDeviceID", DumpOff::UtilTelephony,
                          snap.utilTelephonyId, sizeof(snap.utilTelephonyId));
        read_static_named(pid, il2, utilKlass, "_strMacAddress", DumpOff::UtilMac, snap.utilMac,
                          sizeof(snap.utilMac));
        if (!snap.utilAdId[0])
            read_static_named(pid, il2, utilKlass, "_realAdvertisingID",
                              DumpOff::UtilRealAdvertisingId, snap.utilAdId, sizeof(snap.utilAdId));
    }

    if (gscKlass) {
        uintptr_t inst = 0;
        if (!conrent_il2_static_field_ptr(pid, il2, gscKlass, "Instance", inst) || !inst) {
            const uintptr_t st = esp_bm_static_fields_ptr(pid, gscKlass);
            if (st) mem_read(pid, st + DumpOff::GscStaticInstance, inst);
        }
        if (inst && mem_ptr_ok(inst)) {
            read_field_str(pid, inst, DumpOff::GscJsonDeviceId, snap.gameServerJsonId,
                           sizeof(snap.gameServerJsonId));
            read_field_str(pid, inst, DumpOff::GscReportDeviceId, snap.gameServerReportId,
                           sizeof(snap.gameServerReportId));
            read_field_str(pid, inst, DumpOff::GscUniqueId, snap.gameServerUniqueId,
                           sizeof(snap.gameServerUniqueId));
            read_field_str(pid, inst, DumpOff::GscAndroidId, snap.gameServerAndroidId,
                           sizeof(snap.gameServerAndroidId));
            read_field_str(pid, inst, DumpOff::GscAdvertisingId, snap.gameServerAdId,
                           sizeof(snap.gameServerAdId));
        }
    }

    std::snprintf(snap.hint, sizeof(snap.hint),
                  "GSC instance; SDK/DeviceUtil static (IL2 + vm_read offsets)");

    std::lock_guard<std::mutex> lock(g_mu);
    g_snap = snap;
}

const EspDeviceIdSnap &esp_device_id_snapshot() { return g_snap; }

static void append_line(char *out, size_t n, size_t *pos, const char *label, const char *val) {
    if (!out || !pos || !label) return;
    int w = 0;
    if (val && val[0])
        w = std::snprintf(out + *pos, n > *pos ? n - *pos : 0, "%s: %s\n", label, val);
    else
        w = std::snprintf(out + *pos, n > *pos ? n - *pos : 0, "%s: (empty)\n", label);
    if (w > 0) *pos += (size_t)w;
}

void esp_device_id_format_copy_all(char *out, size_t outSz) {
    if (!out || outSz == 0) return;
    out[0] = '\0';
    const EspDeviceIdSnap &s = esp_device_id_snapshot();
    size_t pos = 0;
    append_line(out, outSz, &pos, "GameServerConfig._strJsonDeviceID", s.gameServerJsonId);
    append_line(out, outSz, &pos, "GameServerConfig._strDeviceIDForReport", s.gameServerReportId);
    append_line(out, outSz, &pos, "GameServerConfig._strUsrUniqueIdentifier", s.gameServerUniqueId);
    append_line(out, outSz, &pos, "GameServerConfig._strAndroidID", s.gameServerAndroidId);
    append_line(out, outSz, &pos, "GameServerConfig._strAdvertisingID", s.gameServerAdId);
    append_line(out, outSz, &pos, "SDKCommon._deviceId", s.sdkDeviceId);
    append_line(out, outSz, &pos, "SDKCommon._us_id_device_uniqueid", s.sdkUniqueId);
    append_line(out, outSz, &pos, "SDKCommon._us_id_android_id", s.sdkAndroidId);
    append_line(out, outSz, &pos, "SDKCommon._us_id_advertising_id", s.sdkAdvertisingId);
    append_line(out, outSz, &pos, "SDKCommon._sOAID", s.sdkOaid);
    append_line(out, outSz, &pos, "SDKCommon._channel", s.sdkChannel);
    append_line(out, outSz, &pos, "DeviceUtil._strAndroidID", s.utilAndroidId);
    append_line(out, outSz, &pos, "DeviceUtil._strAdvertisingID", s.utilAdId);
    append_line(out, outSz, &pos, "DeviceUtil._strTelephonyDeviceID", s.utilTelephonyId);
    append_line(out, outSz, &pos, "DeviceUtil._strMacAddress", s.utilMac);
}

static void line_or_dash(const char *label, const char *val) {
    ImGui::TextDisabled("%s", label);
    if (val && val[0]) ImGui::TextWrapped("%s", val);
    else ImGui::TextDisabled("—");
}

static void paste_into_buf(char *buf, size_t bufSz) {
    if (!buf || bufSz == 0) return;
    char clip[4096];
    if (!android_read_clipboard(clip, sizeof(clip))) {
        ui_log("[device] clipboard empty");
        return;
    }
    std::snprintf(buf, bufSz, "%s", clip);
    ui_log("[device] pasted");
}

static bool spoof_text_field(const char *label, char *buf, size_t bufSz, float multilineH = 0.f) {
    if (!label || !buf) return false;
    ImGui::PushID(label);
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float pasteW = ImGui::CalcTextSize("Paste").x + ImGui::GetStyle().FramePadding.x * 2.f + 6.f;
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(rowW - pasteW);
    if (ImGui::Button("Paste")) paste_into_buf(buf, bufSz);
    if (multilineH > 0.f) {
        ImGui::InputTextMultiline("##val", buf, bufSz, ImVec2(-1.f, multilineH));
    } else {
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputText("##val", buf, bufSz, ImGuiInputTextFlags_None);
    }
    ImGui::PopID();
    return true;
}

void esp_device_id_draw_panel(pid_t pid) {
    if (!g_spoof.loaded) spoof_load_from_disk();

    ImGui::SeparatorText("Spoof (saved) — empty = skip");
    ImGui::Checkbox("In-place spoof on Refresh (15s, safe)", &g_spoof.enabled);
    if (ImGui::Button("Fill from game")) {
        esp_device_id_spoof_fill_from_snapshot();
        ui_log("[device] spoof fields filled from live read");
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste into Json")) paste_into_buf(g_spoof.gscJson, sizeof(g_spoof.gscJson));
    ImGui::BeginChild("device_spoof_edit", ImVec2(0.f, 260.f), true);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 4.f));
    ImGui::TextDisabled("GameServerConfig");
    spoof_text_field("JsonDeviceID", g_spoof.gscJson, sizeof(g_spoof.gscJson), 44.f);
    spoof_text_field("Report ID", g_spoof.gscReport, sizeof(g_spoof.gscReport));
    spoof_text_field("Unique ID", g_spoof.gscUnique, sizeof(g_spoof.gscUnique));
    spoof_text_field("Android ID", g_spoof.gscAndroid, sizeof(g_spoof.gscAndroid));
    spoof_text_field("Ad ID", g_spoof.gscAd, sizeof(g_spoof.gscAd));
    ImGui::Separator();
    ImGui::TextDisabled("SDKCommon");
    spoof_text_field("deviceId", g_spoof.sdkDeviceId, sizeof(g_spoof.sdkDeviceId));
    spoof_text_field("us unique", g_spoof.sdkUnique, sizeof(g_spoof.sdkUnique));
    spoof_text_field("us android", g_spoof.sdkAndroid, sizeof(g_spoof.sdkAndroid));
    spoof_text_field("us ad", g_spoof.sdkAd, sizeof(g_spoof.sdkAd));
    spoof_text_field("OAID", g_spoof.sdkOaid, sizeof(g_spoof.sdkOaid));
    spoof_text_field("channel", g_spoof.sdkChannel, sizeof(g_spoof.sdkChannel));
    ImGui::Separator();
    ImGui::TextDisabled("DeviceUtil");
    spoof_text_field("Android ID", g_spoof.utilAndroid, sizeof(g_spoof.utilAndroid));
    spoof_text_field("Ad ID", g_spoof.utilAd, sizeof(g_spoof.utilAd));
    spoof_text_field("Telephony", g_spoof.utilTelephony, sizeof(g_spoof.utilTelephony));
    spoof_text_field("MAC", g_spoof.utilMac, sizeof(g_spoof.utilMac));
    ImGui::PopStyleVar();
    ImGui::EndChild();
    const float spoofBtn =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.33f;
    if (ImGui::Button("Save", ImVec2(spoofBtn, 0.f))) {
        if (esp_device_id_spoof_save()) ui_log("[device] spoof saved");
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply now", ImVec2(spoofBtn, 0.f))) {
        esp_device_id_spoof_save();
        if (esp_device_id_apply_spoof(pid, false, true)) esp_device_id_refresh(pid);
        else
            ui_log("[device] apply failed — lobby only; use same/shorter length");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear spoof fields", ImVec2(spoofBtn, 0.f))) {
        g_spoof = {};
        g_spoof.loaded = true;
        esp_device_id_spoof_save();
    }
    ImGui::TextDisabled("Apply now = lobby/menu only (ptrace). Refresh = read only + optional in-place.");

    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Refresh", ImVec2(half, 0.f))) esp_device_id_refresh(pid);
    ImGui::SameLine();
    if (ImGui::Button("Copy all", ImVec2(half, 0.f))) {
        static char buf[8192];
        esp_device_id_format_copy_all(buf, sizeof(buf));
        if (android_copy_to_clipboard(buf)) ui_log("[device] copied all IDs");
    }
    const EspDeviceIdSnap &s = esp_device_id_snapshot();
    if (s.hint[0]) ImGui::TextDisabled("%s", s.hint);
    if (ImGui::CollapsingHeader("Live read (game)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        line_or_dash("GSC JsonDeviceID", s.gameServerJsonId);
        line_or_dash("GSC Report", s.gameServerReportId);
        line_or_dash("GSC Unique", s.gameServerUniqueId);
        line_or_dash("GSC Android", s.gameServerAndroidId);
        line_or_dash("GSC Ad", s.gameServerAdId);
        ImGui::Separator();
        line_or_dash("SDK deviceId", s.sdkDeviceId);
        line_or_dash("SDK us unique", s.sdkUniqueId);
        line_or_dash("SDK us android", s.sdkAndroidId);
        line_or_dash("SDK us ad", s.sdkAdvertisingId);
        line_or_dash("SDK OAID", s.sdkOaid);
        line_or_dash("SDK channel", s.sdkChannel);
        ImGui::Separator();
        line_or_dash("Util Android", s.utilAndroidId);
        line_or_dash("Util Ad", s.utilAdId);
        line_or_dash("Util Telephony", s.utilTelephonyId);
        line_or_dash("Util MAC", s.utilMac);
    }
}