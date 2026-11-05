#include "il2_lib_globals.hpp"
#include "il2_layout.hpp"
#include "ui_log.hpp"
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kGlobalCamChainA = 0xa8;
constexpr uintptr_t kGlobalCamChainB = 0x08;

/** Legacy external RVAs sit ~here; scan a small window only (do not walk whole .so). */
constexpr uintptr_t kGlobalWinLo = 0x6300000ULL;
constexpr uintptr_t kGlobalWinHi = 0x6380000ULL;
constexpr size_t kMaxScanBytes = 512 * 1024;
constexpr size_t kMaxCamChainProbes = 48;

static bool map_contains(uintptr_t addr, const MapsReg &m) {
    return m.start <= addr && addr < m.end;
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

static void scan_window(pid_t pid, uintptr_t libBase, uintptr_t winStart, uintptr_t winEnd,
                        uintptr_t bmKlass, uintptr_t bmStatics, uintptr_t expectedSmoothFollow,
                        uintptr_t &outBmRva, uintptr_t &outCamRva) {
    constexpr size_t kChunk = 64 * 1024;
    std::vector<uint8_t> buf(kChunk);
    size_t scanned = 0;
    size_t camProbes = 0;

    for (uintptr_t cur = winStart; cur + sizeof(uintptr_t) <= winEnd; cur += kChunk - sizeof(uintptr_t)) {
        if (scanned >= kMaxScanBytes) break;
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
                klass_static_fields_match(pid, val, bmStatics)) {
                outBmRva = rva;
            }

            if (!outCamRva && expectedSmoothFollow && camProbes < kMaxCamChainProbes &&
                mem_ptr_ok(val) && val != bmKlass) {
                ++camProbes;
                const uintptr_t cam3 = follow_main_camera_root(pid, val);
                if (cam3 && cam3 == expectedSmoothFollow) outCamRva = rva;
            }
        }
    }
}

} // namespace

void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve) {
    resolve.il2_lib_base = il2LibBase;
    resolve.auto_rva_battle_manager_typeinfo = 0;
    resolve.auto_rva_main_camera_root = 0;
    if (pid <= 0 || !il2LibBase) return;

    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) {
        ui_log("[resolve] lib RVA scan skipped (no maps)");
        return;
    }

    const MapsReg *mod = nullptr;
    for (const MapsReg &m : maps) {
        if (map_contains(il2LibBase, m)) {
            mod = &m;
            break;
        }
    }
    if (!mod) {
        ui_log("[resolve] lib RVA scan skipped (no map for il2 base 0x%llx)",
               (unsigned long long)il2LibBase);
        return;
    }

    uintptr_t winStart = il2LibBase + kGlobalWinLo;
    uintptr_t winEnd = il2LibBase + kGlobalWinHi;
    if (winStart < mod->start) winStart = mod->start;
    if (winEnd > mod->end) winEnd = mod->end;
    if (winEnd <= winStart) {
        ui_log("[resolve] lib RVA scan skipped (window outside module)");
        return;
    }

    uintptr_t expectedSmooth = 0;
    if (resolve.game_method_statics && resolve.runtime_fields.gm_mainCamera)
        mem_read(pid, resolve.game_method_statics + resolve.runtime_fields.gm_mainCamera,
                 expectedSmooth);

    ui_log("[resolve] lib RVA quick scan (%zu KB max, lib+0x%llx..0x%llx)...",
           kMaxScanBytes / 1024, (unsigned long long)kGlobalWinLo,
           (unsigned long long)kGlobalWinHi);

    uintptr_t bmRva = 0;
    uintptr_t camRva = 0;
    scan_window(pid, il2LibBase, winStart, winEnd, resolve.battle_manager_klass,
                resolve.battle_manager_statics, expectedSmooth, bmRva, camRva);

    resolve.auto_rva_battle_manager_typeinfo = bmRva;
    resolve.auto_rva_main_camera_root = camRva;

    ui_log("[resolve] auto lib global RVAs (optional, IL2CPP path is primary):");
    if (bmRva)
        ui_log("  BattleManager TypeInfo* slot: lib+0x%llx", (unsigned long long)bmRva);
    else
        ui_log("  BattleManager TypeInfo* slot: not in scan window");
    if (camRva)
        ui_log("  MainCamera root slot: lib+0x%llx", (unsigned long long)camRva);
    else
        ui_log("  MainCamera root slot: not in scan window");
}
