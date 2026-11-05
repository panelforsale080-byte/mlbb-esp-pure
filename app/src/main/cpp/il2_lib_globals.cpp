#include "il2_lib_globals.hpp"
#include "il2_layout.hpp"
#include "ui_log.hpp"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kGlobalCamChainA = 0xa8;
constexpr uintptr_t kGlobalCamChainB = 0x08;
constexpr uintptr_t kLegacyBmTypeInfoRva = 0x635b290ULL;
constexpr uintptr_t kLegacyMainCameraRva = 0x635b620ULL;
constexpr uintptr_t kScanMinRva = 0x400000ULL;
constexpr size_t kMaxScanBytesPerMod = 2 * 1024 * 1024;
constexpr size_t kMaxCamChainProbes = 512;
constexpr uintptr_t kLegacyCamWinLo = 0x6340000ULL;
constexpr uintptr_t kLegacyCamWinHi = 0x6370000ULL;

struct BattleCameraMatricesRaw {
    float worldToCamera[16];
    float projection[16];
};

static bool matrix4_sane(const float m[16]) {
    if (!std::isfinite(m[0]) || !std::isfinite(m[15])) return false;
    const float mag =
        std::fabs(m[0]) + std::fabs(m[5]) + std::fabs(m[10]) + std::fabs(m[15]);
    return mag > 1e-4f && mag < 1e8f;
}

static bool vp_sane_at_holder(pid_t pid, uintptr_t holder) {
    if (!mem_ptr_ok(holder)) return false;
    BattleCameraMatricesRaw cam{};
    if (!mem_read(pid, holder + 0x5C, cam)) return false;
    return matrix4_sane(cam.worldToCamera) && matrix4_sane(cam.projection);
}

static bool vp_from_cam3(pid_t pid, uintptr_t cam3) {
    if (!mem_ptr_ok(cam3)) return false;
    uintptr_t inner = 0;
    mem_read(pid, cam3 + 0x10, inner);
    if (mem_ptr_ok(inner) && vp_sane_at_holder(pid, inner)) return true;
    return vp_sane_at_holder(pid, cam3);
}

static bool main_camera_live_matches(pid_t pid, uintptr_t cam3, uintptr_t expectedLive) {
    if (!expectedLive) return true;
    if (cam3 == expectedLive) return true;
    uintptr_t inner = 0;
    if (mem_read(pid, cam3 + 0x10, inner) && inner == expectedLive) return true;
    return vp_sane_at_holder(pid, expectedLive);
}

static uintptr_t klass_static_fields_match(pid_t pid, uintptr_t klass, uintptr_t expectedStatics) {
    if (!mem_ptr_ok(klass) || !expectedStatics) return 0;
    for (uintptr_t off : Il2::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!mem_read(pid, klass + off, st) || !mem_ptr_ok(st)) continue;
        if (st == expectedStatics) return st;
    }
    return 0;
}

static uintptr_t follow_main_camera_root(pid_t pid, uintptr_t root) {
    if (!mem_ptr_ok(root)) return 0;
    uintptr_t p = 0;
    if (!mem_read(pid, root + kGlobalCamChainA, p) || !mem_ptr_ok(p)) return 0;
    if (!mem_read(pid, p + kGlobalCamChainB, p) || !mem_ptr_ok(p)) return 0;
    return p;
}

static bool validate_main_camera_rva(pid_t pid, uintptr_t libBase, uintptr_t rva,
                                     uintptr_t expectedLive) {
    if (!libBase || !rva) return false;
    uintptr_t root = 0;
    if (!mem_read(pid, libBase + rva, root) || !mem_ptr_ok(root)) return false;
    const uintptr_t cam3 = follow_main_camera_root(pid, root);
    if (!cam3 || !vp_from_cam3(pid, cam3)) return false;
    return main_camera_live_matches(pid, cam3, expectedLive);
}

static bool validate_bm_typeinfo_rva(pid_t pid, uintptr_t libBase, uintptr_t rva, uintptr_t bmKlass,
                                     uintptr_t bmStatics) {
    if (!libBase || !rva || !bmKlass) return false;
    uintptr_t p = 0;
    if (!mem_read(pid, libBase + rva, p) || p != bmKlass) return false;
    return klass_static_fields_match(pid, p, bmStatics) != 0;
}

static void scan_window(pid_t pid, uintptr_t libBase, uintptr_t winStart, uintptr_t winEnd,
                        uintptr_t bmKlass, uintptr_t bmStatics, uintptr_t expectedSmoothFollow,
                        bool requireLiveForCam, uintptr_t &outBmRva, uintptr_t &outCamRva) {
    constexpr size_t kChunk = 64 * 1024;
    std::vector<uint8_t> buf(kChunk);
    size_t scanned = 0;
    size_t camProbes = 0;

    for (uintptr_t cur = winStart; cur + sizeof(uintptr_t) <= winEnd; cur += kChunk - sizeof(uintptr_t)) {
        if (scanned >= kMaxScanBytesPerMod) break;
        if (outBmRva && outCamRva) break;

        const size_t remain = static_cast<size_t>(winEnd - cur);
        const size_t n = remain < kChunk ? remain : kChunk;
        if (!mem_read(pid, cur, buf.data(), n)) continue;
        scanned += n;

        for (size_t i = 0; i + sizeof(uintptr_t) <= n; i += sizeof(uintptr_t)) {
            uintptr_t val = 0;
            std::memcpy(&val, buf.data() + i, sizeof(val));
            if (!val) continue;
            const uintptr_t rva = (cur + i) - libBase;

            if (!outBmRva && bmKlass && val == bmKlass &&
                klass_static_fields_match(pid, val, bmStatics))
                outBmRva = rva;

            if (!outCamRva && mem_ptr_ok(val) && val != bmKlass) {
                const uintptr_t cam3 = follow_main_camera_root(pid, val);
                if (!cam3 || !vp_from_cam3(pid, cam3)) continue;
                if (requireLiveForCam && !expectedSmoothFollow) continue;
                if (expectedSmoothFollow &&
                    !main_camera_live_matches(pid, cam3, expectedSmoothFollow))
                    continue;
                if (camProbes < kMaxCamChainProbes) ++camProbes;
                outCamRva = rva;
            }
        }
    }
}

static uintptr_t module_base_for_path(const std::vector<MapsReg> &maps, const char *needle) {
    uintptr_t base = 0;
    for (const MapsReg &m : maps) {
        if (m.path.find(needle) == std::string::npos) continue;
        if (!base || m.start < base) base = m.start;
    }
    return base;
}

static void module_span(const std::vector<MapsReg> &maps, uintptr_t modBase, uintptr_t &outStart,
                        uintptr_t &outEnd) {
    outStart = 0;
    outEnd = 0;
    for (const MapsReg &m : maps) {
        if (m.start < modBase || m.end <= modBase) continue;
        if (m.path.find("libcsharp") == std::string::npos &&
            m.path.find("libil2cpp") == std::string::npos &&
            m.path.find("liblogic") == std::string::npos)
            continue;
        if (!outStart || m.start < outStart) outStart = m.start;
        if (m.end > outEnd) outEnd = m.end;
    }
}

} // namespace

void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve) {
    resolve.il2_lib_base = il2LibBase;
    resolve.auto_rva_battle_manager_typeinfo = 0;
    resolve.auto_rva_main_camera_root = 0;
    resolve.main_camera_matrix_lib_base = 0;
    if (pid <= 0) return;

    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) {
        ui_log("[resolve] lib RVA scan skipped (no maps)");
        return;
    }

    uintptr_t expectedSmooth = 0;
    if (resolve.game_method_statics && resolve.runtime_fields.gm_mainCamera)
        mem_read(pid, resolve.game_method_statics + resolve.runtime_fields.gm_mainCamera,
                 expectedSmooth);

    ui_log("[resolve] GameMethod statics=0x%llx mainCamera live=0x%llx",
           (unsigned long long)resolve.game_method_statics,
           (unsigned long long)expectedSmooth);
    ui_log("[resolve] lib RVA scan (libcsharp/liblogic/libil2cpp)...");

    uintptr_t bestBmRva = 0;
    uintptr_t bestBmBase = 0;
    uintptr_t bestCamRva = 0;
    uintptr_t bestCamBase = 0;

    const uintptr_t bases[] = {
        module_base_for_path(maps, "libcsharp.so"),
        module_base_for_path(maps, "liblogic.so"),
        module_base_for_path(maps, "libil2cpp.so"),
        il2LibBase,
    };

    for (uintptr_t libBase : bases) {
        if (!libBase) continue;
        uintptr_t modStart = 0;
        uintptr_t modEnd = 0;
        module_span(maps, libBase, modStart, modEnd);
        if (modEnd <= modStart) continue;

        uintptr_t winStart = libBase + kScanMinRva;
        if (winStart < modStart) winStart = modStart;
        uintptr_t winEnd = modEnd;
        if (winEnd <= winStart) continue;

        uintptr_t bmRva = 0;
        uintptr_t camRva = 0;
        scan_window(pid, libBase, winStart, winEnd, resolve.battle_manager_klass,
                    resolve.battle_manager_statics, expectedSmooth, true, bmRva, camRva);
        if (bmRva && !bestBmRva) {
            bestBmRva = bmRva;
            bestBmBase = libBase;
        }
        if (camRva && !bestCamRva) {
            bestCamRva = camRva;
            bestCamBase = libBase;
        }
    }

    const uintptr_t legacyBases[] = {
        module_base_for_path(maps, "libcsharp.so"),
        module_base_for_path(maps, "liblogic.so"),
        module_base_for_path(maps, "libil2cpp.so"),
        il2LibBase,
    };
    for (uintptr_t lb : legacyBases) {
        if (!lb) continue;
        if (!bestCamRva) {
            uintptr_t legBm = 0;
            uintptr_t legCam = 0;
            scan_window(pid, lb, lb + kLegacyCamWinLo, lb + kLegacyCamWinHi, 0, 0, expectedSmooth,
                        false, legBm, legCam);
            if (legCam) {
                bestCamRva = legCam;
                bestCamBase = lb;
                ui_log("[resolve] MainCamera RVA in legacy window lib+0x%llx (base=0x%llx)",
                       (unsigned long long)legCam, (unsigned long long)lb);
            }
        }
        if (!bestCamRva && validate_main_camera_rva(pid, lb, kLegacyMainCameraRva, expectedSmooth)) {
            bestCamRva = kLegacyMainCameraRva;
            bestCamBase = lb;
            ui_log("[resolve] MainCamera RVA validated at legacy lib+0x%llx (base=0x%llx)",
                   (unsigned long long)kLegacyMainCameraRva, (unsigned long long)lb);
        }
        if (!bestBmRva &&
            validate_bm_typeinfo_rva(pid, lb, kLegacyBmTypeInfoRva, resolve.battle_manager_klass,
                                     resolve.battle_manager_statics)) {
            bestBmRva = kLegacyBmTypeInfoRva;
            bestBmBase = lb;
            ui_log("[resolve] BattleManager RVA validated at legacy lib+0x%llx (base=0x%llx)",
                   (unsigned long long)kLegacyBmTypeInfoRva, (unsigned long long)lb);
        }
    }

    resolve.auto_rva_battle_manager_typeinfo = bestBmRva;
    resolve.auto_rva_main_camera_root = bestCamRva;
    resolve.main_camera_matrix_lib_base = bestCamBase;

    ui_log("[resolve] matrix RVAs for ESP (vm_read, accurate path):");
    if (bestBmRva)
        ui_log("  BattleManager TypeInfo: base=0x%llx + 0x%llx",
               (unsigned long long)bestBmBase, (unsigned long long)bestBmRva);
    else
        ui_log("  BattleManager TypeInfo: not found (IL2CPP Instance OK)");
    if (bestCamRva)
        ui_log("  MainCamera root: base=0x%llx + 0x%llx",
               (unsigned long long)bestCamBase, (unsigned long long)bestCamRva);
    else
        ui_log("  MainCamera root: not found — will try GameMethod.mainCamera + 0x5C");
}
