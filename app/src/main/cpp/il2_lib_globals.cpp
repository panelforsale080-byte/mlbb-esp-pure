#include "il2_lib_globals.hpp"
#include "camera_lib_chain.hpp"
#include "il2_layout.hpp"
#include "ui_log.hpp"
#include <string>
#include <vector>

namespace {

/** EXTERNAL-MLBB Offsets.h — fixed; no memory scan. */
constexpr uintptr_t kRvaBattleManagerTypeInfo = 0x635b290ULL;
constexpr uintptr_t kRvaMainCameraRoot = 0x635b620ULL;

static uintptr_t klass_static_fields_match(pid_t pid, uintptr_t klass, uintptr_t expectedStatics) {
    if (!mem_ptr_ok(klass) || !expectedStatics) return 0;
    for (uintptr_t off : Il2::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!mem_read(pid, klass + off, st) || !mem_ptr_ok(st)) continue;
        if (st == expectedStatics) return st;
    }
    return 0;
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
    bool klassMatch = false;
    bool staticsMatch = false;
};

static SpyBmResult spy_battle_manager_rva(pid_t pid, uintptr_t libBase, uintptr_t bmKlass,
                                          uintptr_t bmStatics) {
    SpyBmResult r{};
    const uintptr_t addr = libBase + kRvaBattleManagerTypeInfo;
    if (!mem_read(pid, addr, r.slot)) return r;
    r.slot = mem_ptr_normalize(r.slot);
    if (!r.slot) return r;
    r.klassMatch = bmKlass && r.slot == bmKlass;
    r.staticsMatch = klass_static_fields_match(pid, r.slot, bmStatics) != 0;
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

    uintptr_t expectedLive = mem_ptr_normalize(resolve.game_method_main_camera_obj);
    if (!expectedLive && resolve.game_method_statics && resolve.runtime_fields.gm_mainCamera) {
        uintptr_t raw = 0;
        mem_read(pid, resolve.game_method_statics + resolve.runtime_fields.gm_mainCamera, raw);
        expectedLive = mem_ptr_normalize(raw);
    }

    const uintptr_t baseCsharp = module_base_for_path(maps, "libcsharp.so");
    const uintptr_t baseLogic = module_base_for_path(maps, "liblogic.so");
    const uintptr_t baseIl2cpp = module_base_for_path(maps, "libil2cpp.so");

    ui_log("[resolve] GameMethod mainCamera live=0x%llx (IL2=0x%llx statics+0x10 raw norm)",
           (unsigned long long)expectedLive,
           (unsigned long long)resolve.game_method_main_camera_obj);
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

        uintptr_t rootRaw = 0;
        mem_read(pid, libBase + kRvaMainCameraRoot, rootRaw);
        const LibMainCamChain chain =
            camera_resolve_lib_main_cam_chain(pid, mem_ptr_normalize(rootRaw));
        const bool liveOk =
            chain.cam3 && camera_main_camera_live_matches(pid, chain.cam3, expectedLive);

        ui_log("  [%s] Cam lib+0x%llx -> root=0x%llx +0xA8=0x%llx +0x08_raw=0x%llx "
               "cam3=0x%llx chain=%s vp=%s live=%s",
               label, (unsigned long long)kRvaMainCameraRoot, (unsigned long long)chain.root,
               (unsigned long long)chain.afterA8, (unsigned long long)chain.rawAtA8Plus8,
               (unsigned long long)chain.cam3, chain.variant, chain.vpOk ? "OK" : "fail",
               liveOk ? "OK" : "no");

        if (!pickCamBase && chain.cam3 && chain.vpOk) pickCamBase = libBase;
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
