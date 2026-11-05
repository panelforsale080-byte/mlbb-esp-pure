#include "il2_lib_globals.hpp"
#include "camera_lib_chain.hpp"
#include "il2_layout.hpp"
#include "ui_log.hpp"
#include <cstring>
#include <string>
#include <vector>

namespace {

/** Bootstrap hints only — ESP uses computed slot_va + rva after discover. */
constexpr uintptr_t kHintBmTypeInfoRva = 0x635b290ULL;
constexpr uintptr_t kHintMainCameraRootRva = 0x635b620ULL;
constexpr uintptr_t kNarrowSweepLo = 0x6358000ULL;
constexpr uintptr_t kNarrowSweepHi = 0x635c000ULL;
constexpr uintptr_t kNarrowSweepStep = 8ULL;
constexpr size_t kMaxRwProbeSteps = 8192;
constexpr size_t kMaxRwSpanBytes = 512 * 1024;
constexpr size_t kGmStaticsWalkBytes = 0x800;

struct MemSpan {
    uintptr_t start = 0;
    uintptr_t end = 0;
};

static void collect_module_rw_spans(const std::vector<MapsReg> &maps, const char *needle,
                                    std::vector<MemSpan> &out) {
    for (const MapsReg &m : maps) {
        if (m.path.find(needle) == std::string::npos) continue;
        if (!std::strchr(m.perms, 'w')) continue;
        if (m.end <= m.start) continue;
        if (static_cast<size_t>(m.end - m.start) > 8 * 1024 * 1024) continue;
        out.push_back({m.start, m.end});
    }
}

static uintptr_t scan_step_for_span(uintptr_t spanLen) {
    if (spanLen <= kMaxRwProbeSteps * 8) return 8;
    uintptr_t step = spanLen / kMaxRwProbeSteps;
    step = (step / 8) * 8;
    return step < 8 ? 8 : step;
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

static bool try_bm_slot(pid_t pid, uintptr_t slotVa, uintptr_t bmKlass, uintptr_t bmStatics,
                        uintptr_t &outKlass) {
    outKlass = 0;
    if (!slotVa) return false;
    if (!mem_read(pid, slotVa, outKlass)) return false;
    outKlass = mem_ptr_normalize(outKlass);
    if (!outKlass || !bmKlass || outKlass != bmKlass) return false;
    return klass_static_fields_match(pid, outKlass, bmStatics) != 0;
}

static bool try_main_cam_slot(pid_t pid, uintptr_t slotVa, LibMainCamChain &chain) {
    chain = {};
    if (!slotVa) return false;
    uintptr_t root = 0;
    if (!mem_read(pid, slotVa, root)) return false;
    chain = camera_resolve_lib_main_cam_chain(pid, root);
    return chain.cam3 && chain.vpOk;
}

static void append_unique_slot(std::vector<uintptr_t> &slots, uintptr_t va) {
    if (!va) return;
    for (uintptr_t s : slots) {
        if (s == va) return;
    }
    slots.push_back(va);
}

struct DiscoverPick {
    uintptr_t slotVa = 0;
    uintptr_t libBase = 0;
    uintptr_t computedRva = 0;
    const char *via = "";
};

static void append_gm_statics_cam_candidates(uintptr_t gmStatics, std::vector<uintptr_t> &slots) {
    if (!gmStatics) return;
    static const int kRel[] = {-0x138, -0x140, -0x130, -0x148, -0x128, -0x150, -0x120, -0x160};
    for (int rel : kRel) append_unique_slot(slots, gmStatics + rel);
    const uintptr_t lo = gmStatics - kGmStaticsWalkBytes;
    const uintptr_t hi = gmStatics + 0x100;
    for (uintptr_t slot = lo; slot < hi; slot += 8) append_unique_slot(slots, slot);
}

static bool discover_bm_rw_auto(pid_t pid, const std::vector<MapsReg> &maps,
                                uintptr_t libBase, uintptr_t bmKlass, uintptr_t bmStatics,
                                DiscoverPick &pick) {
    if (!libBase || !bmKlass) return false;
    std::vector<MemSpan> spans;
    collect_module_rw_spans(maps, "libcsharp.so", spans);
    size_t probes = 0;
    for (const MemSpan &sp : spans) {
        uintptr_t cur = sp.start;
        uintptr_t end = sp.end;
        const uintptr_t len = end - cur;
        if (len > kMaxRwSpanBytes) {
            cur = libBase + kHintBmTypeInfoRva;
            if (cur < kMaxRwSpanBytes / 2) cur = sp.start;
            else cur -= kMaxRwSpanBytes / 2;
            if (cur < sp.start) cur = sp.start;
            end = cur + kMaxRwSpanBytes;
            if (end > sp.end) end = sp.end;
        }
        const uintptr_t step = scan_step_for_span(end - cur);
        for (; cur + sizeof(uintptr_t) <= end; cur += step) {
            if (probes++ >= kMaxRwProbeSteps) return false;
            uintptr_t klass = 0;
            if (!try_bm_slot(pid, cur, bmKlass, bmStatics, klass)) continue;
            pick.slotVa = cur;
            pick.libBase = libBase;
            pick.computedRva = cur - libBase;
            pick.via = "rw-auto";
            ui_log("  [rw-auto] BM slot_va=0x%llx rva=0x%llx", (unsigned long long)cur,
                   (unsigned long long)pick.computedRva);
            return true;
        }
    }
    return false;
}

static bool discover_main_cam_rw_auto(pid_t pid, const std::vector<MapsReg> &maps,
                                      uintptr_t gmStatics, uintptr_t expectedLive,
                                      DiscoverPick &pick, bool wantLive) {
    const char *needles[] = {"libcsharp.so", "liblogic.so"};
    size_t probes = 0;
    for (const char *needle : needles) {
        const uintptr_t modBase = module_base_for_path(maps, needle);
        if (!modBase) continue;
        std::vector<MemSpan> spans;
        collect_module_rw_spans(maps, needle, spans);
        for (const MemSpan &sp : spans) {
            uintptr_t cur = sp.start;
            uintptr_t end = sp.end;
            const uintptr_t len = end - cur;
            if (len > kMaxRwSpanBytes) {
                uintptr_t anchor = gmStatics;
                if (!anchor || anchor < sp.start || anchor >= sp.end)
                    anchor = modBase + kHintMainCameraRootRva;
                cur = anchor;
                if (cur < kMaxRwSpanBytes / 2) cur = sp.start;
                else cur -= kMaxRwSpanBytes / 2;
                if (cur < sp.start) cur = sp.start;
                end = cur + kMaxRwSpanBytes;
                if (end > sp.end) end = sp.end;
            }
            const uintptr_t step = scan_step_for_span(end - cur);
            for (; cur + sizeof(uintptr_t) <= end; cur += step) {
                if (probes++ >= kMaxRwProbeSteps) return pick.slotVa != 0;
                LibMainCamChain chain{};
                if (!try_main_cam_slot(pid, cur, chain)) continue;
                const bool liveOk =
                    camera_main_camera_live_matches(pid, chain.cam3, expectedLive);
                if (wantLive && !liveOk) continue;
                pick.slotVa = cur;
                pick.libBase = modBase;
                pick.computedRva = cur - modBase;
                pick.via = "rw-auto";
                ui_log("  [rw-auto] Cam slot_va=0x%llx rva=0x%llx chain=%s live=%s",
                       (unsigned long long)cur, (unsigned long long)pick.computedRva, chain.variant,
                       liveOk ? "OK" : "no");
                if (liveOk || !wantLive) return true;
            }
        }
    }
    return pick.slotVa != 0;
}

static void append_narrow_cam_slots(uintptr_t libBase, std::vector<uintptr_t> &slots) {
    if (!libBase) return;
    for (uintptr_t rva = kNarrowSweepLo; rva <= kNarrowSweepHi; rva += kNarrowSweepStep)
        append_unique_slot(slots, libBase + rva);
}

} // namespace

void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve) {
    resolve.il2_lib_base = il2LibBase;
    resolve.auto_rva_battle_manager_typeinfo = 0;
    resolve.auto_rva_main_camera_root = 0;
    resolve.main_camera_matrix_lib_base = 0;
    resolve.bm_global_slot_va = 0;
    resolve.main_camera_global_slot_va = 0;
    if (pid <= 0) return;

    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) {
        ui_log("[resolve] global slot discover skipped (no maps)");
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

    ui_log("[resolve] GameMethod mainCamera live=0x%llx", (unsigned long long)expectedLive);
    ui_log("[resolve] discover lib globals (auto RVA update: hint -> statics -> narrow -> rw-auto):");

    std::vector<uintptr_t> bases;
    collect_unique_bases(baseCsharp, baseLogic, baseIl2cpp, il2LibBase, bases);

    DiscoverPick bmPick{};
    DiscoverPick camPick{};

    for (uintptr_t libBase : bases) {
        const char *label =
            module_label(libBase, baseCsharp, baseLogic, baseIl2cpp, il2LibBase);

        std::vector<uintptr_t> bmSlots;
        append_unique_slot(bmSlots, libBase + kHintBmTypeInfoRva);
        if (libBase == baseCsharp) {
            for (uintptr_t rva = kNarrowSweepLo; rva <= kNarrowSweepHi; rva += kNarrowSweepStep)
                append_unique_slot(bmSlots, libBase + rva);
        }

        if (!bmPick.slotVa) {
            for (uintptr_t slot : bmSlots) {
                uintptr_t klass = 0;
                if (!try_bm_slot(pid, slot, resolve.battle_manager_klass,
                                 resolve.battle_manager_statics, klass))
                    continue;
                bmPick.slotVa = slot;
                bmPick.libBase = libBase;
                bmPick.computedRva = slot - libBase;
                bmPick.via = (slot == libBase + kHintBmTypeInfoRva) ? "hint" : "narrow";
                ui_log("  [%s] BM slot_va=0x%llx rva=0x%llx klass=0x%llx via=%s", label,
                       (unsigned long long)slot, (unsigned long long)bmPick.computedRva,
                       (unsigned long long)klass, bmPick.via);
                break;
            }
        }

        if (camPick.slotVa) continue;

        std::vector<uintptr_t> camSlots;
        append_unique_slot(camSlots, libBase + kHintMainCameraRootRva);
        append_gm_statics_cam_candidates(resolve.game_method_statics, camSlots);
        if (libBase == baseCsharp) append_narrow_cam_slots(libBase, camSlots);

        bool camLocked = false;
        for (uintptr_t slot : camSlots) {
            LibMainCamChain chain{};
            if (!try_main_cam_slot(pid, slot, chain)) continue;
            const bool liveOk = camera_main_camera_live_matches(pid, chain.cam3, expectedLive);
            ui_log("  [%s] Cam slot_va=0x%llx rva=0x%llx chain=%s vp=OK live=%s", label,
                   (unsigned long long)slot, (unsigned long long)(slot - libBase),
                   chain.variant, liveOk ? "OK" : "no");

            auto setPick = [&]() {
                camPick.slotVa = slot;
                camPick.libBase = libBase;
                camPick.computedRva = slot - libBase;
                if (slot == libBase + kHintMainCameraRootRva)
                    camPick.via = "hint";
                else if (resolve.game_method_statics &&
                         slot >= resolve.game_method_statics - 0x200 &&
                         slot <= resolve.game_method_statics + 0x40)
                    camPick.via = "GameMethod-statics-rel";
                else
                    camPick.via = "narrow";
            };

            if (liveOk) {
                setPick();
                camLocked = true;
                break;
            }
            if (!camPick.slotVa) setPick();
        }
        if (camLocked) break;
    }

    if (!bmPick.slotVa && baseCsharp) {
        ui_log("[resolve] auto-update: RW scan BattleManager TypeInfo (libcsharp)...");
        discover_bm_rw_auto(pid, maps, baseCsharp, resolve.battle_manager_klass,
                            resolve.battle_manager_statics, bmPick);
    }

    if (!camPick.slotVa && baseCsharp) {
        ui_log("[resolve] auto-update: RW scan MainCamera global (libcsharp/liblogic)...");
        discover_main_cam_rw_auto(pid, maps, resolve.game_method_statics, expectedLive, camPick,
                                  true);
        if (!camPick.slotVa)
            discover_main_cam_rw_auto(pid, maps, resolve.game_method_statics, expectedLive,
                                      camPick, false);
    }

    if (bmPick.slotVa) {
        resolve.bm_global_slot_va = bmPick.slotVa;
        resolve.auto_rva_battle_manager_typeinfo = bmPick.computedRva;
        ui_log("[resolve] BM STORED slot_va=0x%llx base=0x%llx rva=0x%llx (%s)",
               (unsigned long long)bmPick.slotVa, (unsigned long long)bmPick.libBase,
               (unsigned long long)bmPick.computedRva, bmPick.via);
    }

    if (camPick.slotVa) {
        resolve.main_camera_global_slot_va = camPick.slotVa;
        resolve.main_camera_matrix_lib_base = camPick.libBase;
        resolve.auto_rva_main_camera_root = camPick.computedRva;
        ui_log("[resolve] MainCamera STORED slot_va=0x%llx base=0x%llx rva=0x%llx (%s) ESP",
               (unsigned long long)camPick.slotVa, (unsigned long long)camPick.libBase,
               (unsigned long long)camPick.computedRva, camPick.via);
    } else {
        ui_log("[resolve] MainCamera global not found — GameMethod matrix fallback");
    }

    ui_log("[resolve] ESP matrix source (no hardcoded Offsets.h at draw):");
    if (resolve.main_camera_global_slot_va)
        ui_log("  MainCamera *slot @ 0x%llx (lib+0x%llx)", 
               (unsigned long long)resolve.main_camera_global_slot_va,
               (unsigned long long)resolve.auto_rva_main_camera_root);
}
