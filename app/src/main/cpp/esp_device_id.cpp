#include "esp_device_id.hpp"
#include "conrent_esp.hpp"
#include "esp_engine.hpp"
#include "mem.hpp"
#include "ui_log.hpp"

#include "imgui.h"

#include <cstring>
#include <mutex>

/* dump.cs #147 — GameServerConfig + SDKCommon device strings (liblogic.so offsets). */
namespace DumpOff {
constexpr uintptr_t GscJsonDeviceId = 0x118;
constexpr uintptr_t GscAndroidId = 0x128;
constexpr uintptr_t GscAdvertisingId = 0x130;
constexpr uintptr_t GscReportDeviceId = 0x140;
constexpr uintptr_t GscStaticInstance = 0xd0;

constexpr uintptr_t SdkDeviceId = 0x28;
constexpr uintptr_t SdkUsAndroidId = 0x208;
constexpr uintptr_t SdkUsAdvertisingId = 0x210;
} // namespace DumpOff

static pid_t g_pid = 0;
static uintptr_t g_gscKlass = 0;
static uintptr_t g_sdkKlass = 0;
static std::mutex g_mu;
static EspDeviceIdSnap g_snap;

void esp_device_id_on_resolve(pid_t pid, uintptr_t gameServerConfigKlass,
                              uintptr_t sdkCommonKlass) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = pid;
    g_gscKlass = gameServerConfigKlass;
    g_sdkKlass = sdkCommonKlass;
}

void esp_device_id_clear() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = 0;
    g_gscKlass = 0;
    g_sdkKlass = 0;
    g_snap = {};
}

static bool read_il2_str(pid_t pid, uintptr_t strObj, char *out, size_t outSz) {
    if (!out || outSz == 0) return false;
    out[0] = '\0';
    if (!strObj || !mem_ptr_ok(strObj)) return false;
    alignas(4) uint8_t blob[4 + 96 * 2];
    if (!mem_read(pid, strObj + 0x10, blob, sizeof(blob))) return false;
    int32_t len = 0;
    std::memcpy(&len, blob, sizeof(len));
    if (len <= 0 || len > 96) return false;
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

static bool read_static_str(pid_t pid, uintptr_t klass, uintptr_t off, char *out, size_t n) {
    const uintptr_t st = esp_bm_static_fields_ptr(pid, klass);
    if (!st) return false;
    uintptr_t p = 0;
    if (!mem_read(pid, st + off, p)) return false;
    return read_il2_str(pid, p, out, n);
}

void esp_device_id_refresh(pid_t pid) {
    EspDeviceIdSnap snap{};
    snap.refreshed = true;
    uintptr_t gscKlass = 0;
    uintptr_t sdkKlass = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        gscKlass = g_gscKlass;
        sdkKlass = g_sdkKlass;
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
    if (!gscKlass && !sdkKlass) {
        std::snprintf(snap.hint, sizeof(snap.hint), "Device classes not found — Resolve again.");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = snap;
        return;
    }

    if (sdkKlass) {
        read_static_str(pid, sdkKlass, DumpOff::SdkDeviceId, snap.sdkDeviceId,
                        sizeof(snap.sdkDeviceId));
        read_static_str(pid, sdkKlass, DumpOff::SdkUsAndroidId, snap.sdkAndroidId,
                        sizeof(snap.sdkAndroidId));
        read_static_str(pid, sdkKlass, DumpOff::SdkUsAdvertisingId, snap.sdkAdvertisingId,
                        sizeof(snap.sdkAdvertisingId));
    }

    if (gscKlass) {
        uintptr_t inst = 0;
        if (conrent_il2_static_field_ptr(pid, il2, gscKlass, "Instance", inst) && inst) {
            read_field_str(pid, inst, DumpOff::GscJsonDeviceId, snap.gameServerJsonId,
                           sizeof(snap.gameServerJsonId));
            read_field_str(pid, inst, DumpOff::GscReportDeviceId, snap.gameServerReportId,
                           sizeof(snap.gameServerReportId));
            read_field_str(pid, inst, DumpOff::GscAndroidId, snap.gameServerAndroidId,
                           sizeof(snap.gameServerAndroidId));
            read_field_str(pid, inst, DumpOff::GscAdvertisingId, snap.gameServerAdId,
                           sizeof(snap.gameServerAdId));
        } else {
            const uintptr_t st = esp_bm_static_fields_ptr(pid, gscKlass);
            if (st) mem_read(pid, st + DumpOff::GscStaticInstance, inst);
            if (inst && mem_ptr_ok(inst)) {
                read_field_str(pid, inst, DumpOff::GscJsonDeviceId, snap.gameServerJsonId,
                               sizeof(snap.gameServerJsonId));
                read_field_str(pid, inst, DumpOff::GscReportDeviceId, snap.gameServerReportId,
                               sizeof(snap.gameServerReportId));
                read_field_str(pid, inst, DumpOff::GscAndroidId, snap.gameServerAndroidId,
                               sizeof(snap.gameServerAndroidId));
                read_field_str(pid, inst, DumpOff::GscAdvertisingId, snap.gameServerAdId,
                               sizeof(snap.gameServerAdId));
            }
        }
    }

    const bool any = snap.gameServerJsonId[0] || snap.gameServerReportId[0] ||
                     snap.sdkDeviceId[0] || snap.gameServerAndroidId[0];
    if (any)
        std::snprintf(snap.hint, sizeof(snap.hint), "dump.cs GameServerConfig + SDKCommon");
    else
        std::snprintf(snap.hint, sizeof(snap.hint), "No strings yet — open game past login.");

    std::lock_guard<std::mutex> lock(g_mu);
    g_snap = snap;
}

const EspDeviceIdSnap &esp_device_id_snapshot() { return g_snap; }

static void line_or_dash(const char *label, const char *val) {
    ImGui::TextDisabled("%s", label);
    if (val && val[0]) ImGui::TextWrapped("%s", val);
    else ImGui::TextDisabled("—");
}

void esp_device_id_draw_panel(pid_t pid) {
    ImGui::SeparatorText("Device ID (IL2)");
    if (ImGui::Button("Refresh", ImVec2(-FLT_MIN, 0.f))) esp_device_id_refresh(pid);
    const EspDeviceIdSnap &s = esp_device_id_snapshot();
    if (s.hint[0]) ImGui::TextDisabled("%s", s.hint);
    ImGui::Spacing();
    line_or_dash("GameServerConfig._strJsonDeviceID", s.gameServerJsonId);
    line_or_dash("GameServerConfig._strDeviceIDForReport", s.gameServerReportId);
    line_or_dash("GameServerConfig._strAndroidID", s.gameServerAndroidId);
    line_or_dash("GameServerConfig._strAdvertisingID", s.gameServerAdId);
    ImGui::Separator();
    line_or_dash("SDKCommon._deviceId", s.sdkDeviceId);
    line_or_dash("SDKCommon._us_id_android_id", s.sdkAndroidId);
    line_or_dash("SDKCommon._us_id_advertising_id", s.sdkAdvertisingId);
}