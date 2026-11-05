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
/** EXTERNAL-MLBB Offsets.h — fixed; no memory scan. */
constexpr uintptr_t kRvaBattleManagerTypeInfo = 0x635b290ULL;
constexpr uintptr_t kRvaMainCameraRoot = 0x635b620ULL;

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

static uintptr_t module_base_for_path(const std::vector<MapsReg> &maps, const char *needle) {
    uintptr_t base = 0;
    for (const MapsReg &m : maps) {
        if (m.path.find(needle) == std::string::npos) continue;
        if (!base || m.start < base) base = m.start;
    }
    return base;
}

static const char *module_label(uintptr_t base, uintptr_t csharp, uintptr_t logic, uintptr_t il2cpp,
                                uintptr_t il2Detected) {
    if (base == csharp) return "libcsharp.so";
    if (base == logic) return "liblogic.so";
    if (base == il2cpp) return "libil2cpp.so";
    if (base == il2Detected) return "il2-detect-base";
    return "unknown-so";
}

static void collect_unique_bases(uintptr_t csharp, uintptr_t logic, uintptr_t il2cpp,
                                   uintptr_t il2Detected, std::vector<uintptr_t> &out) {
    const uintptr_t cand[] = {csharp, logic, il2cpp, il2Detected};
    for (uintptr_t b : cand) {
        if (!b) continue;
        bool dup = false;
        for (uintptr_t x : out) {
            if (x == b) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(b);
    }
}

struct SpyBmResult {
    uintptr_t slot = 0;
    bool readOk = false;
    bool klassMatch = false;
    bool staticsMatch = false;
};

static SpyBmResult spy_battle_manager_rva(pid_t pid, uintptr_t libBase, uintptr_t bmKlass,
                                          uintptr_t bmStatics) {
    SpyBmResult r{};
    const uintptr_t addr = libBase + kRvaBattleManagerTypeInfo;
    r.readOk = mem_read(pid, addr, r.slot);
    if (!r.readOk || !mem_ptr_ok(r.slot)) return r;
    r.klassMatch = bmKlass && r.slot == bmKlass;
    r.staticsMatch = klass_static_fields_match(pid, r.slot, bmStatics) != 0;
    return r;
}

struct SpyCamResult {
    uintptr_t root = 0;
    uintptr_t afterA8 = 0;
    uintptr_t cam3 = 0;
    uintptr_t inner10 = 0;
    bool readRoot = false;
    bool chainOk = false;
    bool vpOk = false;
    bool liveOk = false;
};

static SpyCamResult spy_main_camera_rva(pid_t pid, uintptr_t libBase, uintptr_t expectedLive) {
    SpyCamResult r{};
    const uintptr_t addr = libBase + kRvaMainCameraRoot;
    r.readRoot = mem_read(pid, addr, r.root);
    if (!r.readRoot || !mem_ptr_ok(r.root)) return r;
    if (!mem_read(pid, r.root + kGlobalCamChainA, r.afterA8)) return r;
    r.cam3 = follow_main_camera_root(pid, r.root);
    r.chainOk = mem_ptr_ok(r.cam3);
    if (r.chainOk) mem_read(pid, r.cam3 + 0x10, r.inner10);
    if (r.chainOk) r.vpOk = vp_from_cam3(pid, r.cam3);
    if (r.chainOk && r.vpOk) r.liveOk = main_camera_live_matches(pid, r.cam3, expectedLive);
    return r;
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
        ui_log("[resolve] RVA spy skipped (no maps)");
        return;
    }

    uintptr_t expectedLive = 0;
    if (resolve.game_method_statics && resolve.runtime_fields.gm_mainCamera)
        mem_read(pid, resolve.game_method_statics + resolve.runtime_fields.gm_mainCamera,
                 expectedLive);

    const uintptr_t baseCsharp = module_base_for_path(maps, "libcsharp.so");
    const uintptr_t baseLogic = module_base_for_path(maps, "liblogic.so");
    const uintptr_t baseIl2cpp = module_base_for_path(maps, "libil2cpp.so");

    ui_log("[resolve] GameMethod statics=0x%llx mainCamera live=0x%llx",
           (unsigned long long)resolve.game_method_statics,
           (unsigned long long)expectedLive);
    ui_log("[resolve] RVA spy (fixed EXTERNAL offsets, no scan):");
    ui_log("  BM TypeInfo slot  lib+0x%llx", (unsigned long long)kRvaBattleManagerTypeInfo);
    ui_log("  MainCamera root   lib+0x%llx", (unsigned long long)kRvaMainCameraRoot);

    std::vector<uintptr_t> bases;
    collect_unique_bases(baseCsharp, baseLogic, baseIl2cpp, il2LibBase, bases);

    uintptr_t pickBmBase = 0;
    uintptr_t pickCamBase = 0;

    for (uintptr_t libBase : bases) {
        const char *label =
            module_label(libBase, baseCsharp, baseLogic, baseIl2cpp, il2LibBase);

        const SpyBmResult bm = spy_battle_manager_rva(pid, libBase, resolve.battle_manager_klass,
                                                      resolve.battle_manager_statics);
        ui_log("  [%s] base=0x%llx BM lib+0x%llx -> *slot=0x%llx klass=%s statics=%s",
               label, (unsigned long long)libBase, (unsigned long long)kRvaBattleManagerTypeInfo,
               (unsigned long long)bm.slot, bm.klassMatch ? "MATCH" : "no",
               bm.staticsMatch ? "MATCH" : "no");

        if (!pickBmBase && bm.klassMatch && bm.staticsMatch) pickBmBase = libBase;

        const SpyCamResult cam = spy_main_camera_rva(pid, libBase, expectedLive);
        ui_log("  [%s] Cam lib+0x%llx -> root=0x%llx +0xA8=0x%llx cam3=0x%llx +0x10=0x%llx "
               "chain=%s vp=%s live=%s",
               label, (unsigned long long)kRvaMainCameraRoot, (unsigned long long)cam.root,
               (unsigned long long)cam.afterA8, (unsigned long long)cam.cam3,
               (unsigned long long)cam.inner10, cam.chainOk ? "OK" : "fail",
               cam.vpOk ? "OK" : "fail", cam.liveOk ? "OK" : "no");

        if (!pickCamBase && cam.chainOk && cam.vpOk) pickCamBase = libBase;
    }

    if (pickBmBase) {
        resolve.auto_rva_battle_manager_typeinfo = kRvaBattleManagerTypeInfo;
        ui_log("[resolve] BM RVA pick: %s base=0x%llx + 0x%llx",
               module_label(pickBmBase, baseCsharp, baseLogic, baseIl2cpp, il2LibBase),
               (unsigned long long)pickBmBase, (unsigned long long)kRvaBattleManagerTypeInfo);
    }

    if (pickCamBase) {
        resolve.auto_rva_main_camera_root = kRvaMainCameraRoot;
        resolve.main_camera_matrix_lib_base = pickCamBase;
        ui_log("[resolve] MainCamera RVA pick: %s base=0x%llx + 0x%llx (ESP vm_read)",
               module_label(pickCamBase, baseCsharp, baseLogic, baseIl2cpp, il2LibBase),
               (unsigned long long)pickCamBase, (unsigned long long)kRvaMainCameraRoot);
    } else {
        ui_log("[resolve] MainCamera fixed RVA not usable on any module — GameMethod + 0x5C fallback");
    }

    ui_log("[resolve] matrix RVAs for ESP:");
    if (resolve.auto_rva_battle_manager_typeinfo)
        ui_log("  BattleManager: base=0x%llx + 0x%llx", (unsigned long long)pickBmBase,
               (unsigned long long)kRvaBattleManagerTypeInfo);
    if (resolve.auto_rva_main_camera_root)
        ui_log("  MainCamera: base=0x%llx + 0x%llx",
               (unsigned long long)resolve.main_camera_matrix_lib_base,
               (unsigned long long)kRvaMainCameraRoot);
}
