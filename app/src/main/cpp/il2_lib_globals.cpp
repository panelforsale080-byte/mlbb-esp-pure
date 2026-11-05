#include "il2_lib_globals.hpp"
#include "il2_layout.hpp"
#include "ui_log.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** EXTERNAL-MLBB pointer chain after reading root pointer from lib global (not an MLBB field). */
constexpr uintptr_t kGlobalCamChainA = 0xa8;
constexpr uintptr_t kGlobalCamChainB = 0x08;

static bool path_has_il2_lib(const std::string &path) {
    return path.find("libcsharp.so") != std::string::npos ||
           path.find("libil2cpp.so") != std::string::npos;
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

static void scan_range(pid_t pid, uintptr_t libBase, uintptr_t scanStart, uintptr_t scanEnd,
                       uintptr_t bmKlass, uintptr_t bmStatics, uintptr_t expectedSmoothFollow,
                       uintptr_t &outBmRva, uintptr_t &outCamRva) {
    constexpr size_t kChunk = 256 * 1024;
    std::vector<uint8_t> buf(kChunk);
    scanStart = std::max(scanStart, libBase + 0x400000ULL);

    for (uintptr_t cur = scanStart; cur + sizeof(uintptr_t) <= scanEnd && (!outBmRva || !outCamRva);
         cur += kChunk - sizeof(uintptr_t)) {
        const size_t n = std::min(kChunk, scanEnd - cur);
        if (!mem_read(pid, cur, buf.data(), n)) continue;
        for (size_t i = 0; i + sizeof(uintptr_t) <= n; i += sizeof(uintptr_t)) {
            uintptr_t val = 0;
            std::memcpy(&val, buf.data() + i, sizeof(val));
            if (!val) continue;
            const uintptr_t slot = cur + i;
            const uintptr_t rva = slot - libBase;

            if (!outBmRva && bmKlass && val == bmKlass &&
                klass_static_fields_match(pid, val, bmStatics))
                outBmRva = rva;

            if (!outCamRva && expectedSmoothFollow && mem_ptr_ok(val)) {
                const uintptr_t cam3 = follow_main_camera_root(pid, val);
                if (cam3 && cam3 == expectedSmoothFollow) outCamRva = rva;
            }
            if (outBmRva && outCamRva) return;
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
    if (!mem_maps(pid, maps)) return;

    uintptr_t scanStart = 0;
    uintptr_t scanEnd = 0;
    for (const MapsReg &m : maps) {
        if (!path_has_il2_lib(m.path)) continue;
        if (m.start < il2LibBase) continue;
        if (m.end <= m.start) continue;
        if (!scanStart || m.start < scanStart) scanStart = m.start;
        if (m.end > scanEnd) scanEnd = m.end;
    }
    if (scanEnd <= scanStart) return;

    uintptr_t expectedSmooth = 0;
    if (resolve.game_method_statics && resolve.runtime_fields.gm_mainCamera)
        mem_read(pid, resolve.game_method_statics + resolve.runtime_fields.gm_mainCamera,
                 expectedSmooth);

    uintptr_t bmRva = 0;
    uintptr_t camRva = 0;
    scan_range(pid, il2LibBase, scanStart, scanEnd, resolve.battle_manager_klass,
               resolve.battle_manager_statics, expectedSmooth, bmRva, camRva);

    resolve.auto_rva_battle_manager_typeinfo = bmRva;
    resolve.auto_rva_main_camera_root = camRva;

    ui_log("[resolve] auto lib global RVAs (scan, replaces Offsets.h BasePtr/MainCameraPtr):");
    ui_log("  il2 lib base=0x%llx scan=[0x%llx,0x%llx)", (unsigned long long)il2LibBase,
           (unsigned long long)scanStart, (unsigned long long)scanEnd);
    if (bmRva)
        ui_log("  BattleManager TypeInfo* slot: lib+0x%llx  (legacy ref ~0x635b290)",
               (unsigned long long)bmRva);
    else
        ui_log("  BattleManager TypeInfo* slot: not found — using FindClass + Instance");
    if (camRva)
        ui_log("  MainCamera root slot: lib+0x%llx  (legacy ref ~0x635b620)",
               (unsigned long long)camRva);
    else
        ui_log("  MainCamera root slot: not found — using GameMethod.mainCamera");
}
