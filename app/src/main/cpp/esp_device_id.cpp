#include "esp_device_id.hpp"
#include "clip_android.hpp"
#include "conrent_esp.hpp"
#include "esp_engine.hpp"
#include "mem.hpp"
#include "ui_log.hpp"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <mutex>

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
    alignas(4) uint8_t blob[4 + 128 * 2];
    if (!mem_read(pid, strObj + 0x10, blob, sizeof(blob))) return false;
    int32_t len = 0;
    std::memcpy(&len, blob, sizeof(len));
    if (len <= 0 || len > 128) return false;
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

void esp_device_id_draw_panel(pid_t pid) {
    ImGui::SeparatorText("Device ID (IL2)");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Refresh", ImVec2(half, 0.f))) esp_device_id_refresh(pid);
    ImGui::SameLine();
    if (ImGui::Button("Copy all", ImVec2(half, 0.f))) {
        static char buf[4096];
        esp_device_id_format_copy_all(buf, sizeof(buf));
        if (android_copy_to_clipboard(buf)) ui_log("[device] copied all IDs");
    }
    const EspDeviceIdSnap &s = esp_device_id_snapshot();
    if (s.hint[0]) ImGui::TextDisabled("%s", s.hint);
    ImGui::Spacing();
    line_or_dash("GameServerConfig._strJsonDeviceID", s.gameServerJsonId);
    line_or_dash("GameServerConfig._strDeviceIDForReport", s.gameServerReportId);
    line_or_dash("GameServerConfig._strUsrUniqueIdentifier", s.gameServerUniqueId);
    line_or_dash("GameServerConfig._strAndroidID", s.gameServerAndroidId);
    line_or_dash("GameServerConfig._strAdvertisingID", s.gameServerAdId);
    ImGui::Separator();
    line_or_dash("SDKCommon._deviceId", s.sdkDeviceId);
    line_or_dash("SDKCommon._us_id_device_uniqueid", s.sdkUniqueId);
    line_or_dash("SDKCommon._us_id_android_id", s.sdkAndroidId);
    line_or_dash("SDKCommon._us_id_advertising_id", s.sdkAdvertisingId);
    line_or_dash("SDKCommon._sOAID", s.sdkOaid);
    line_or_dash("SDKCommon._channel", s.sdkChannel);
    ImGui::Separator();
    line_or_dash("DeviceUtil._strAndroidID", s.utilAndroidId);
    line_or_dash("DeviceUtil._strAdvertisingID", s.utilAdId);
    line_or_dash("DeviceUtil._strTelephonyDeviceID", s.utilTelephonyId);
    line_or_dash("DeviceUtil._strMacAddress", s.utilMac);
}