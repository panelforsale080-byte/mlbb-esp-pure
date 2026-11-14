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
static pid_t g_baselinePid = 0;
static bool g_baselineSaved = false;

static constexpr char kSpoofFile[] = "device_spoof.cfg";

static bool read_il2_str(pid_t pid, uintptr_t strObj, char *out, size_t outSz);

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

static bool spoof_value_placeholder(const char *v) {
    if (!v || !v[0]) return true;
    if (std::strcmp(v, "(empty)") == 0 || std::strcmp(v, "—") == 0) return true;
    if (std::strcmp(v, "none") == 0 || std::strcmp(v, "NONE") == 0) return true;
    return false;
}

/** Literal field name before game downloads JSON from server. */
static bool is_gsc_json_placeholder_label(const char *v) {
    if (!v || !v[0]) return true;
    return std::strcmp(v, "JsonDeviceID") == 0;
}

/** Real payload after server/config download. */
static bool is_gsc_json_from_server(const char *v) {
    if (!v || !v[0] || is_gsc_json_placeholder_label(v)) return false;
    if (v[0] == '{' || v[0] == '[') return true;
    return std::strlen(v) >= 48;
}

static char g_lastServerJsonSaved[1024] = {};
static char g_lastRawJsonFile[1024] = {};

/** Exact GameServerConfig._strJsonDeviceID from live read — no Apply, every Refresh when value changes. */
static void persist_gsc_json_device_id_raw(const EspDeviceIdSnap &snap) {
    const char *j = snap.gameServerJsonId;
    if (!j || !j[0]) return;
    if (std::strncmp(g_lastRawJsonFile, j, sizeof(g_lastRawJsonFile)) == 0) return;

    static const char *const kRaw[] = {"/storage/emulated/0/gsc_json_device_id.txt",
                                       "/sdcard/gsc_json_device_id.txt", nullptr};
    const char *writtenPath = nullptr;
    for (unsigned i = 0; kRaw[i]; ++i) {
        if (android_write_file_path(kRaw[i], j)) {
            writtenPath = kRaw[i];
            break;
        }
    }
    if (!writtenPath) {
        static uint64_t s_lastFailMs = 0;
        const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        if (nowMs - s_lastFailMs >= 15000) {
            ui_log("[device] gsc_json_device_id.txt FAIL — allow All files access");
            s_lastFailMs = nowMs;
        }
        return;
    }

    std::snprintf(g_lastRawJsonFile, sizeof(g_lastRawJsonFile), "%s", j);
    const size_t n = std::strlen(j);
    const bool fromServer = is_gsc_json_from_server(j);
    char preview[96];
    preview[0] = '\0';
    if (n > 0) {
        size_t clip = n < sizeof(preview) - 1 ? n : sizeof(preview) - 4;
        std::memcpy(preview, j, clip);
        preview[clip] = '\0';
        if (n > clip) std::strcat(preview, "...");
    }
    ui_log("[device] GSC JsonDeviceID file -> %s | %zu chars | server=%s | %s", writtenPath, n,
           fromServer ? "yes" : "no", preview[0] ? preview : "(empty)");
}

static void spoof_fill_from_snap(const EspDeviceIdSnap &s) {
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
    if (s.sdkDeviceId[0] && !spoof_value_placeholder(s.sdkDeviceId))
        g_spoof.sdkDeviceLocked = true;
}

static bool patch_il2_string_inplace(pid_t pid, uintptr_t strObj, const char *utf8, char *why,
                                     size_t whyN) {
    if (why && whyN) why[0] = '\0';
    if (!strObj || !mem_ptr_ok(strObj) || !utf8) {
        if (why && whyN) std::snprintf(why, whyN, "bad string obj");
        return false;
    }
    int32_t oldLen = 0;
    if (!mem_read(pid, strObj + 0x10, oldLen) || oldLen <= 0 || oldLen > 512) {
        if (why && whyN) std::snprintf(why, whyN, "read len fail (old=%d)", oldLen);
        return false;
    }
    uint16_t units[512];
    const size_t newLen = utf8_to_utf16le(utf8, units, sizeof(units) / sizeof(units[0]));
    if (newLen == 0) {
        if (why && whyN) std::snprintf(why, whyN, "utf8 empty");
        return false;
    }
    if (newLen > (size_t)oldLen) {
        if (why && whyN)
            std::snprintf(why, whyN, "too long (new=%zu old=%d) need Apply lobby", newLen, oldLen);
        return false;
    }
    uint8_t blob[4 + 512 * 2] = {};
    const int32_t wlen = (int32_t)newLen;
    std::memcpy(blob, &wlen, sizeof(wlen));
    for (size_t i = 0; i < newLen; ++i) {
        blob[4 + i * 2] = (uint8_t)(units[i] & 0xFF);
        blob[4 + i * 2 + 1] = (uint8_t)(units[i] >> 8);
    }
    for (size_t i = newLen; i < (size_t)oldLen; ++i) {
        blob[4 + i * 2] = ' ';
        blob[4 + i * 2 + 1] = 0;
    }
    const size_t writeN =
        newLen < (size_t)oldLen ? 4 + (size_t)oldLen * 2 : 4 + newLen * 2;
    if (!mem_write(pid, strObj + 0x10, blob, writeN)) {
        if (why && whyN) std::snprintf(why, whyN, "mem_write fail");
        return false;
    }
    if (why && whyN) std::snprintf(why, whyN, "in-place (old=%d new=%zu)", oldLen, newLen);
    return true;
}

static bool write_managed_string_at(pid_t pid, RemoteIl2CppInfo &il2, RemoteSession *ptraceS,
                                    bool allowPtrace, uintptr_t holder, uintptr_t fieldOff,
                                    const char *utf8, char *why, size_t whyN) {
    if (why && whyN) why[0] = '\0';
    if (!holder) {
        if (why && whyN) std::snprintf(why, whyN, "holder=0");
        return false;
    }
    if (!utf8 || !utf8[0] || spoof_value_placeholder(utf8)) {
        if (why && whyN) std::snprintf(why, whyN, "skip empty/none");
        return false;
    }
    uintptr_t strObj = 0;
    mem_read(pid, holder + fieldOff, strObj);
    if (strObj && patch_il2_string_inplace(pid, strObj, utf8, why, whyN)) return true;
    if (!strObj) {
        if (why && whyN) std::snprintf(why, whyN, "string ptr null");
    }
    if (!allowPtrace) {
        if (why && whyN && why[0] == '\0') std::snprintf(why, whyN, "in-place failed; tap Apply in lobby");
        return false;
    }
    if (!ptraceS || !ptraceS->attached) {
        if (why && whyN) std::snprintf(why, whyN, "ptrace not attached");
        return false;
    }
    if (!il2.sym_string_new) {
        if (why && whyN) std::snprintf(why, whyN, "il2cpp_string_new missing");
        return false;
    }
    uintptr_t neu = 0;
    if (!conrent_il2_string_new_session(*ptraceS, pid, il2, utf8, neu) || !mem_ptr_ok(neu)) {
        if (why && whyN)
            std::snprintf(why, whyN, "string_new fail (%s)", ptraceS->err[0] ? ptraceS->err : "?");
        return false;
    }
    if (!mem_write(pid, holder + fieldOff, &neu, sizeof(neu))) {
        if (why && whyN) std::snprintf(why, whyN, "write ptr fail");
        return false;
    }
    if (why && whyN) std::snprintf(why, whyN, "new string 0x%llx", (unsigned long long)neu);
    return true;
}

static bool spoof_apply_kv(const char *key, const char *val) {
    if (!key || !val || spoof_value_placeholder(val)) return false;
    if (std::strcmp(key, "enabled") == 0) {
        g_spoof.enabled = std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0 ||
                            std::strcmp(val, "yes") == 0;
        return true;
    }
    if (std::strcmp(key, "auto_android_txt") == 0) {
        g_spoof.autoAndroidTxt = std::strcmp(val, "0") != 0 && std::strcmp(val, "false") != 0;
        return true;
    }
    if (std::strcmp(key, "sync_sdk_device") == 0) {
        g_spoof.syncSdkDeviceFromUnique =
            std::strcmp(val, "0") != 0 && std::strcmp(val, "false") != 0;
        return true;
    }
    if (std::strcmp(key, "auto_save_android_txt") == 0) {
        g_spoof.autoSaveAndroidTxt = std::strcmp(val, "0") != 0 && std::strcmp(val, "false") != 0;
        return true;
    }
    if (std::strcmp(key, "gsc_json") == 0) {
        std::snprintf(g_spoof.gscJson, sizeof(g_spoof.gscJson), "%s", val);
        return true;
    }
    if (std::strcmp(key, "gsc_report") == 0) {
        std::snprintf(g_spoof.gscReport, sizeof(g_spoof.gscReport), "%s", val);
        return true;
    }
    if (std::strcmp(key, "gsc_unique") == 0) {
        std::snprintf(g_spoof.gscUnique, sizeof(g_spoof.gscUnique), "%s", val);
        return true;
    }
    if (std::strcmp(key, "gsc_android") == 0) {
        std::snprintf(g_spoof.gscAndroid, sizeof(g_spoof.gscAndroid), "%s", val);
        return true;
    }
    if (std::strcmp(key, "gsc_ad") == 0) {
        std::snprintf(g_spoof.gscAd, sizeof(g_spoof.gscAd), "%s", val);
        return true;
    }
    if (std::strcmp(key, "sdk_device") == 0) {
        std::snprintf(g_spoof.sdkDeviceId, sizeof(g_spoof.sdkDeviceId), "%s", val);
        g_spoof.sdkDeviceLocked = true;
        return true;
    }
    if (std::strcmp(key, "sdk_unique") == 0) {
        std::snprintf(g_spoof.sdkUnique, sizeof(g_spoof.sdkUnique), "%s", val);
        return true;
    }
    if (std::strcmp(key, "sdk_android") == 0) {
        std::snprintf(g_spoof.sdkAndroid, sizeof(g_spoof.sdkAndroid), "%s", val);
        return true;
    }
    if (std::strcmp(key, "sdk_ad") == 0) {
        std::snprintf(g_spoof.sdkAd, sizeof(g_spoof.sdkAd), "%s", val);
        return true;
    }
    if (std::strcmp(key, "sdk_oaid") == 0) {
        std::snprintf(g_spoof.sdkOaid, sizeof(g_spoof.sdkOaid), "%s", val);
        return true;
    }
    if (std::strcmp(key, "sdk_channel") == 0) {
        std::snprintf(g_spoof.sdkChannel, sizeof(g_spoof.sdkChannel), "%s", val);
        return true;
    }
    if (std::strcmp(key, "util_android") == 0) {
        std::snprintf(g_spoof.utilAndroid, sizeof(g_spoof.utilAndroid), "%s", val);
        return true;
    }
    if (std::strcmp(key, "util_ad") == 0) {
        std::snprintf(g_spoof.utilAd, sizeof(g_spoof.utilAd), "%s", val);
        return true;
    }
    if (std::strcmp(key, "util_telephony") == 0) {
        std::snprintf(g_spoof.utilTelephony, sizeof(g_spoof.utilTelephony), "%s", val);
        return true;
    }
    if (std::strcmp(key, "util_mac") == 0) {
        std::snprintf(g_spoof.utilMac, sizeof(g_spoof.utilMac), "%s", val);
        return true;
    }
    return false;
}

static bool spoof_apply_copyall_label(const char *label, const char *val) {
    if (!label || !val || spoof_value_placeholder(val)) return false;
    if (std::strstr(label, "_strJsonDeviceID") != nullptr) {
        std::snprintf(g_spoof.gscJson, sizeof(g_spoof.gscJson), "%s", val);
        return true;
    }
    if (std::strstr(label, "_strDeviceIDForReport") != nullptr) {
        std::snprintf(g_spoof.gscReport, sizeof(g_spoof.gscReport), "%s", val);
        return true;
    }
    if (std::strstr(label, "_strUsrUniqueIdentifier") != nullptr) {
        std::snprintf(g_spoof.gscUnique, sizeof(g_spoof.gscUnique), "%s", val);
        return true;
    }
    if (std::strstr(label, "GameServerConfig") != nullptr &&
        std::strstr(label, "_strAndroidID") != nullptr) {
        std::snprintf(g_spoof.gscAndroid, sizeof(g_spoof.gscAndroid), "%s", val);
        return true;
    }
    if (std::strstr(label, "GameServerConfig") != nullptr &&
        std::strstr(label, "_strAdvertisingID") != nullptr) {
        std::snprintf(g_spoof.gscAd, sizeof(g_spoof.gscAd), "%s", val);
        return true;
    }
    if (std::strstr(label, "SDKCommon._deviceId") != nullptr ||
        (std::strstr(label, "SDKCommon") != nullptr && std::strstr(label, "_deviceId") != nullptr)) {
        std::snprintf(g_spoof.sdkDeviceId, sizeof(g_spoof.sdkDeviceId), "%s", val);
        g_spoof.sdkDeviceLocked = true;
        return true;
    }
    if (std::strstr(label, "_us_id_device_uniqueid") != nullptr) {
        std::snprintf(g_spoof.sdkUnique, sizeof(g_spoof.sdkUnique), "%s", val);
        return true;
    }
    if (std::strstr(label, "_us_id_android_id") != nullptr) {
        std::snprintf(g_spoof.sdkAndroid, sizeof(g_spoof.sdkAndroid), "%s", val);
        return true;
    }
    if (std::strstr(label, "_us_id_advertising_id") != nullptr) {
        std::snprintf(g_spoof.sdkAd, sizeof(g_spoof.sdkAd), "%s", val);
        return true;
    }
    if (std::strstr(label, "_sOAID") != nullptr) {
        std::snprintf(g_spoof.sdkOaid, sizeof(g_spoof.sdkOaid), "%s", val);
        return true;
    }
    if (std::strstr(label, "_channel") != nullptr && std::strstr(label, "SDKCommon") != nullptr) {
        std::snprintf(g_spoof.sdkChannel, sizeof(g_spoof.sdkChannel), "%s", val);
        return true;
    }
    if (std::strstr(label, "DeviceUtil") != nullptr &&
        std::strstr(label, "_strAndroidID") != nullptr) {
        std::snprintf(g_spoof.utilAndroid, sizeof(g_spoof.utilAndroid), "%s", val);
        return true;
    }
    if (std::strstr(label, "DeviceUtil") != nullptr &&
        std::strstr(label, "_strAdvertisingID") != nullptr) {
        std::snprintf(g_spoof.utilAd, sizeof(g_spoof.utilAd), "%s", val);
        return true;
    }
    if (std::strstr(label, "_strTelephonyDeviceID") != nullptr) {
        std::snprintf(g_spoof.utilTelephony, sizeof(g_spoof.utilTelephony), "%s", val);
        return true;
    }
    if (std::strstr(label, "_strMacAddress") != nullptr) {
        std::snprintf(g_spoof.utilMac, sizeof(g_spoof.utilMac), "%s", val);
        return true;
    }
    return false;
}

static int spoof_parse_text_buffer(char *buf) {
    int applied = 0;
    char *save = nullptr;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        trim_line(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = std::strchr(line, '=');
        char *colon = std::strchr(line, ':');
        if (eq && (!colon || eq < colon)) {
            *eq = '\0';
            char *val = eq + 1;
            trim_line(line);
            trim_line(val);
            if (spoof_apply_kv(line, val)) ++applied;
        } else if (colon) {
            *colon = '\0';
            char *val = colon + 1;
            trim_line(line);
            trim_line(val);
            if (val[0] && val[0] != ' ') {
                while (val[0] == ' ') ++val;
            }
            if (val[0] == '\0') continue;
            if (spoof_apply_kv(line, val) || spoof_apply_copyall_label(line, val)) ++applied;
        }
    }
    return applied;
}

static void spoof_load_from_disk() {
    std::vector<uint8_t> raw;
    if (!android_app_file_read(kSpoofFile, raw)) {
        g_spoof.loaded = true;
        return;
    }
    raw.push_back(0);
    spoof_parse_text_buffer(reinterpret_cast<char *>(raw.data()));
    g_spoof.loaded = true;
}

static void snap_to_export_lines(std::string &body, const EspDeviceIdSnap &s) {
    auto line = [&](const char *key, const char *val) {
        if (!val || !val[0] || spoof_value_placeholder(val)) return;
        spoof_put_line(body, key, val);
    };
    line("gsc_json", s.gameServerJsonId);
    line("gsc_report", s.gameServerReportId);
    line("gsc_unique", s.gameServerUniqueId);
    line("gsc_android", s.gameServerAndroidId);
    line("gsc_ad", s.gameServerAdId);
    line("sdk_device", s.sdkDeviceId);
    line("sdk_unique", s.sdkUniqueId);
    line("sdk_android", s.sdkAndroidId);
    line("sdk_ad", s.sdkAdvertisingId);
    line("sdk_oaid", s.sdkOaid);
    line("sdk_channel", s.sdkChannel);
    line("util_android", s.utilAndroidId);
    line("util_ad", s.utilAdId);
    line("util_telephony", s.utilTelephonyId);
    line("util_mac", s.utilMac);
}

static bool save_snap_to_path(const EspDeviceIdSnap &s, const char *absPath, const char *header) {
    if (!absPath || !s.gameServerJsonId[0]) return false;
    std::string body;
    body += header ? header : "# Xorel device IDs\n";
    spoof_put_line(body, "auto_android_txt", "0");
    spoof_put_line(body, "sync_sdk_device", "1");
    snap_to_export_lines(body, s);
    return android_write_file_path(absPath, body.c_str());
}

/** When server JSON arrives in GSC._strJsonDeviceID, save full snap to sdcard then fill spoof from it. */
static void try_autosave_server_json_device_id(pid_t pid, const EspDeviceIdSnap &snap) {
    if (!g_spoof.autoSaveAndroidTxt) return;
    const char *j = snap.gameServerJsonId;
    if (!is_gsc_json_from_server(j)) {
        static uint64_t s_lastWaitLogMs = 0;
        const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        if (nowMs - s_lastWaitLogMs >= 20000) {
            ui_log("[device] waiting server JsonDeviceID (current=%s)", j && j[0] ? j : "?");
            s_lastWaitLogMs = nowMs;
        }
        return;
    }
    if (g_baselinePid == pid && g_baselineSaved &&
        std::strncmp(g_lastServerJsonSaved, j, sizeof(g_lastServerJsonSaved)) == 0)
        return;

    std::string body;
    body += "# GameServerConfig._strJsonDeviceID from server (auto, before Apply)\n";
    spoof_put_line(body, "auto_android_txt", "1");
    spoof_put_line(body, "auto_save_android_txt", "1");
    spoof_put_line(body, "sync_sdk_device", "1");
    snap_to_export_lines(body, snap);

    static const char *const kTxt[] = {"/storage/emulated/0/android.txt", "/sdcard/android.txt",
                                       nullptr};
    bool wrote = false;
    for (unsigned i = 0; kTxt[i]; ++i) {
        if (android_write_file_path(kTxt[i], body.c_str())) {
            ui_log("[device] server JSON saved -> %s (%zu bytes)", kTxt[i], std::strlen(j));
            wrote = true;
            break;
        }
    }
    static const char *const kOrig[] = {"/storage/emulated/0/android_original.txt",
                                        "/sdcard/android_original.txt", nullptr};
    for (unsigned i = 0; kOrig[i]; ++i) {
        if (android_write_file_path(kOrig[i], body.c_str())) break;
    }

    if (!wrote) {
        ui_log("[device] server JSON save FAIL — All files access?");
        return;
    }

    spoof_fill_from_snap(snap);
    std::snprintf(g_lastServerJsonSaved, sizeof(g_lastServerJsonSaved), "%s", j);
    g_baselineSaved = true;
    g_baselinePid = pid;
    esp_device_id_spoof_save();
}

bool esp_device_id_save_android_txt(const EspDeviceIdSnap *snapOpt) {
    const EspDeviceIdSnap &s = snapOpt ? *snapOpt : esp_device_id_snapshot();
    if (!s.gameServerJsonId[0]) {
        ui_log("[device] save android.txt: no JsonDeviceID yet");
        return false;
    }
    static const char *const kPaths[] = {"/storage/emulated/0/android.txt", "/sdcard/android.txt",
                                         nullptr};
    for (unsigned i = 0; kPaths[i]; ++i) {
        if (save_snap_to_path(s, kPaths[i], "# Manual export — edit then Reload txt / Apply\n")) {
            ui_log("[device] saved %s (JsonDeviceID=%s)", kPaths[i], s.gameServerJsonId);
            return true;
        }
    }
    ui_log("[device] save android.txt FAIL — All files access?");
    return false;
}

bool esp_device_id_load_android_txt(bool force) {
    if (!g_spoof.autoAndroidTxt && !force) return false;
    static uint64_t s_lastMs = 0;
    const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (!force && nowMs - s_lastMs < 2500) return false;

    static const char *const kPaths[] = {"/storage/emulated/0/android.txt", "/sdcard/android.txt",
                                         nullptr};
    std::vector<uint8_t> raw;
    const char *used = nullptr;
    for (unsigned i = 0; kPaths[i]; ++i) {
        if (android_read_file_path(kPaths[i], raw) && !raw.empty()) {
            used = kPaths[i];
            break;
        }
    }
    s_lastMs = nowMs;
    if (!used) {
        if (force) ui_log("[device] android.txt missing — allow All files access");
        return false;
    }
    raw.push_back(0);
    g_spoof.sdkDeviceLocked = false;
    const int n = spoof_parse_text_buffer(reinterpret_cast<char *>(raw.data()));
    if (n > 0) {
        ui_log("[device] loaded %s (%d fields)", used, n);
        esp_device_id_spoof_save();
    } else if (force) {
        ui_log("[device] android.txt: no known keys (use Copy all format or gsc_android=)");
    }
    return n > 0;
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
    spoof_put_line(body, "auto_android_txt", g_spoof.autoAndroidTxt ? "1" : "0");
    spoof_put_line(body, "sync_sdk_device", g_spoof.syncSdkDeviceFromUnique ? "1" : "0");
    spoof_put_line(body, "auto_save_android_txt", g_spoof.autoSaveAndroidTxt ? "1" : "0");
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
    spoof_fill_from_snap(esp_device_id_snapshot());
}

bool esp_device_id_waiting_server_json() {
    if (!g_spoof.autoSaveAndroidTxt) return false;
    return !is_gsc_json_from_server(esp_device_id_snapshot().gameServerJsonId);
}

static void spoof_derive_before_apply() {
    if (!g_spoof.syncSdkDeviceFromUnique || g_spoof.sdkDeviceLocked) return;
    if (spoof_value_placeholder(g_spoof.gscUnique)) return;
    char built[384];
    std::snprintf(built, sizeof(built), "and_%s", g_spoof.gscUnique);
    if (std::strcmp(g_spoof.sdkDeviceId, built) != 0) {
        std::snprintf(g_spoof.sdkDeviceId, sizeof(g_spoof.sdkDeviceId), "%s", built);
        ui_log("[device] derived SDK deviceId: %s", built);
    }
}

static void verify_string_field(pid_t pid, uintptr_t holder, uintptr_t off, const char *expect,
                                const char *label) {
    if (!holder || !expect || spoof_value_placeholder(expect)) return;
    uintptr_t strObj = 0;
    if (!mem_read(pid, holder + off, strObj) || !strObj) {
        ui_log("[device] verify %s: no string ptr", label);
        return;
    }
    char got[192];
    if (!read_il2_str(pid, strObj, got, sizeof(got))) {
        ui_log("[device] verify %s: read fail", label);
        return;
    }
    if (std::strcmp(got, expect) == 0)
        ui_log("[device] verify %s OK (%s)", label, got);
    else
        ui_log("[device] verify %s MISMATCH want=%s got=%s", label, expect, got);
}

static bool resolve_gsc_instance(pid_t pid, uintptr_t gscKlass, uintptr_t &inst) {
    inst = 0;
    if (!gscKlass) return false;
    const uintptr_t st = esp_bm_static_fields_ptr(pid, gscKlass);
    if (st) mem_read(pid, st + DumpOff::GscStaticInstance, inst);
    return inst != 0 && mem_ptr_ok(inst);
}

bool esp_device_id_apply_spoof(pid_t pid, bool requireEnabled, bool allowPtrace) {
    if (pid <= 0) {
        ui_log("[device] apply FAIL: pid=0 (Find pid)");
        return false;
    }
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

    ui_log("[device] apply start pid=%d ptrace=%d GSC=0x%llx SDK=0x%llx Util=0x%llx", (int)pid,
           allowPtrace ? 1 : 0, (unsigned long long)gscKlass, (unsigned long long)sdkKlass,
           (unsigned long long)utilKlass);

    if (!gscKlass && !sdkKlass && !utilKlass) {
        ui_log("[device] apply FAIL: Resolve first (no device klass)");
        return false;
    }

    RemoteIl2CppInfo il2{};
    if (!esp_resolved_il2(il2)) {
        ui_log("[device] apply FAIL: IL2 not resolved");
        return false;
    }

    RemoteSession ptraceS{};
    RemoteSession *ptracePtr = nullptr;
    if (allowPtrace && il2.sym_string_new) {
        if (remote_session_begin_try(pid, ptraceS))
            ptracePtr = &ptraceS;
        else if (remote_session_begin(pid, ptraceS)) {
            ptracePtr = &ptraceS;
            ui_log("[device] ptrace (full attach for string_new)");
        } else
            ui_log("[device] WARN: ptrace failed (%s) — in-place only", ptraceS.err);
    } else if (allowPtrace && !il2.sym_string_new) {
        ui_log("[device] WARN: il2cpp_string_new=0 — long IDs need same length");
    }

    spoof_derive_before_apply();

    int ok = 0;
    int fail = 0;
    int skip = 0;

    auto applyStr = [&](const char *label, uintptr_t holder, uintptr_t off, const char *val) {
        if (spoof_value_placeholder(val)) {
            ++skip;
            return;
        }
        if (!holder) {
            ui_log("[device] %s FAIL: holder=0", label);
            ++fail;
            return;
        }
        char why[128];
        const bool w =
            write_managed_string_at(pid, il2, ptracePtr, allowPtrace, holder, off, val, why,
                                    sizeof(why));
        if (w) {
            ++ok;
            ui_log("[device] %s OK: %s = %s", label, why, val);
        } else {
            ++fail;
            ui_log("[device] %s FAIL: %s", label, why[0] ? why : "unknown");
        }
    };

    uintptr_t gscInst = 0;
    if (gscKlass) resolve_gsc_instance(pid, gscKlass, gscInst);
    if (!gscInst && gscKlass)
        ui_log("[device] GSC Instance=0 — open lobby/login then Apply");
    if (gscInst) {
        applyStr("GSC JsonDeviceID", gscInst, DumpOff::GscJsonDeviceId, g_spoof.gscJson);
        applyStr("GSC Report", gscInst, DumpOff::GscReportDeviceId, g_spoof.gscReport);
        applyStr("GSC Unique", gscInst, DumpOff::GscUniqueId, g_spoof.gscUnique);
        applyStr("GSC Android", gscInst, DumpOff::GscAndroidId, g_spoof.gscAndroid);
        applyStr("GSC Ad", gscInst, DumpOff::GscAdvertisingId, g_spoof.gscAd);
    }

    const char *sdkAnd =
        !spoof_value_placeholder(g_spoof.sdkAndroid)
            ? g_spoof.sdkAndroid
            : (!spoof_value_placeholder(g_spoof.gscAndroid) ? g_spoof.gscAndroid
                                                              : (!spoof_value_placeholder(
                                                                     g_spoof.utilAndroid)
                                                                     ? g_spoof.utilAndroid
                                                                     : nullptr));

    if (sdkKlass) {
        const uintptr_t st = esp_bm_static_fields_ptr(pid, sdkKlass);
        if (!st)
            ui_log("[device] SDKCommon statics=0");
        if (st) {
            applyStr("SDK deviceId", st, DumpOff::SdkDeviceId, g_spoof.sdkDeviceId);
            applyStr("SDK us unique", st, DumpOff::SdkUsUnique, g_spoof.sdkUnique);
            if (sdkAnd) applyStr("SDK us android", st, DumpOff::SdkUsAndroid, sdkAnd);
            applyStr("SDK us ad", st, DumpOff::SdkUsAdvertising, g_spoof.sdkAd);
            applyStr("SDK OAID", st, DumpOff::SdkOaid, g_spoof.sdkOaid);
            applyStr("SDK channel", st, DumpOff::SdkChannel, g_spoof.sdkChannel);
        }
    }

    if (utilKlass) {
        const uintptr_t st = esp_bm_static_fields_ptr(pid, utilKlass);
        if (!st)
            ui_log("[device] DeviceUtil statics=0");
        if (st) {
            if (!spoof_value_placeholder(g_spoof.utilAndroid))
                applyStr("Util Android", st, DumpOff::UtilAndroidId, g_spoof.utilAndroid);
            else if (sdkAnd)
                applyStr("Util Android", st, DumpOff::UtilAndroidId, sdkAnd);
            if (!spoof_value_placeholder(g_spoof.utilAd)) {
                applyStr("Util Ad", st, DumpOff::UtilAdvertisingId, g_spoof.utilAd);
                applyStr("Util realAd", st, DumpOff::UtilRealAdvertisingId, g_spoof.utilAd);
            }
            applyStr("Util Telephony", st, DumpOff::UtilTelephony, g_spoof.utilTelephony);
            applyStr("Util MAC", st, DumpOff::UtilMac, g_spoof.utilMac);
        }
    }

    if (ptracePtr) remote_session_end(ptraceS);

    ui_log("[device] apply done OK=%d FAIL=%d skip=%d%s", ok, fail, skip,
           allowPtrace ? "" : " (in-place only)");

    if (ok > 0 && gscInst) {
        verify_string_field(pid, gscInst, DumpOff::GscAndroidId, g_spoof.gscAndroid, "GSC Android");
        verify_string_field(pid, gscInst, DumpOff::GscUniqueId, g_spoof.gscUnique, "GSC Unique");
    }
    if (ok > 0 && utilKlass) {
        const uintptr_t ut = esp_bm_static_fields_ptr(pid, utilKlass);
        if (ut) {
            const char *a = !spoof_value_placeholder(g_spoof.utilAndroid) ? g_spoof.utilAndroid
                                                                          : sdkAnd;
            if (a) verify_string_field(pid, ut, DumpOff::UtilAndroidId, a, "Util Android");
        }
    }
    if (ok > 0 && sdkKlass) {
        const uintptr_t st = esp_bm_static_fields_ptr(pid, sdkKlass);
        if (st && sdkAnd)
            verify_string_field(pid, st, DumpOff::SdkUsAndroid, sdkAnd, "SDK us android");
    }

    return ok > 0;
}

void esp_device_id_on_resolve(pid_t pid, uintptr_t gameServerConfigKlass, uintptr_t sdkCommonKlass,
                              uintptr_t deviceUtilKlass) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_baselinePid != pid) {
        g_baselinePid = pid;
        g_baselineSaved = false;
        g_lastServerJsonSaved[0] = '\0';
    }
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
    g_baselinePid = 0;
    g_baselineSaved = false;
    g_lastServerJsonSaved[0] = '\0';
    g_lastRawJsonFile[0] = '\0';
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

    persist_gsc_json_device_id_raw(snap);
    try_autosave_server_json_device_id(pid, snap);

    if (!is_gsc_json_from_server(snap.gameServerJsonId) && g_spoof.autoAndroidTxt)
        esp_device_id_load_android_txt(false);

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

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = snap;
    }
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
    if (g_spoof.autoAndroidTxt) esp_device_id_load_android_txt(false);

    ImGui::SeparatorText("Spoof (saved) — empty = skip");
    ImGui::Checkbox("Auto load /sdcard/android.txt", &g_spoof.autoAndroidTxt);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload txt")) esp_device_id_load_android_txt(true);
    ImGui::Checkbox("SDK deviceId = and_<gsc_unique>", &g_spoof.syncSdkDeviceFromUnique);
    ImGui::Checkbox("Auto-save when server JsonDeviceID ready", &g_spoof.autoSaveAndroidTxt);
    ImGui::SameLine();
    if (ImGui::SmallButton("Export txt"))
        esp_device_id_save_android_txt(nullptr);
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
        else ui_log("[device] apply: see log above (Resolve + lobby + field length)");
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
        ImGui::TextWrapped(
            "Refresh only (no Apply): exact _strJsonDeviceID -> /sdcard/gsc_json_device_id.txt");
        ImGui::TextWrapped(
            "When server JSON ready (+ checkbox): all IDs also -> /sdcard/android.txt");
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