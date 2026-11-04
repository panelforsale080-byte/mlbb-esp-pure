#include "conrent_esp.hpp"
#include "offsets.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <cstring>
#include <string>

namespace {

static void split_class_name(const char *full, std::string &ns, std::string &cls) {
    ns.clear();
    cls = full ? full : "";
    const size_t dot = cls.find_last_of('.');
    if (dot != std::string::npos) {
        ns = cls.substr(0, dot);
        cls = cls.substr(dot + 1);
    }
}

static uintptr_t find_cstring(pid_t pid, const char *lit) {
    if (!lit) return 0;
    const size_t n = std::strlen(lit) + 1;
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return 0;
    for (const auto &m : maps) {
        if (m.perms[0] != 'r') continue;
        const size_t sz = (size_t)(m.end - m.start);
        if (sz < n || sz > 64 * 1024 * 1024) continue;
        const size_t step = 512 * 1024;
        for (size_t off = 0; off < sz; off += step) {
            const size_t chunk = std::min(step + n, sz - off);
            std::vector<char> buf(chunk);
            if (!mem_read(pid, m.start + off, buf.data(), chunk)) continue;
            for (size_t i = 0; i + n <= chunk; ++i) {
                if (std::memcmp(buf.data() + i, lit, n) == 0)
                    return m.start + off + i;
            }
        }
    }
    return 0;
}

static bool runtime_ready_session(RemoteSession &s, RemoteIl2CppInfo &il2, size_t &assemblyCount) {
    assemblyCount = 0;
    if (!il2.sym_domain_get || !il2.sym_domain_get_assemblies)
        return false;

    uintptr_t domain = 0;
    if (!remote_session_call(s, il2.sym_domain_get, {}, domain) || !mem_ptr_ok(domain))
        return false;

    if (il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
    }

    const uintptr_t sizeSlot = remote_session_scratch(s, 0, sizeof(size_t));
    if (!sizeSlot) return false;
    size_t zero = 0;
    mem_write(s.pid, sizeSlot, &zero, sizeof(zero));

    uintptr_t assemblies = 0;
    if (!remote_session_call(s, il2.sym_domain_get_assemblies, {domain, sizeSlot}, assemblies))
        return false;
    if (!mem_ptr_ok(assemblies)) return false;
    if (!mem_read(s.pid, sizeSlot, assemblyCount) || assemblyCount == 0 || assemblyCount > 512)
        return false;
    il2.domain_ptr = domain;
    return true;
}

static uintptr_t find_class_conrent(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                    const char *fullName, size_t assemblyCount,
                                    uintptr_t assembliesArray) {
    if (!il2.sym_class_from_name || !il2.sym_assembly_get_image || !assembliesArray)
        return 0;

    std::string ns, cls;
    split_class_name(fullName, ns, cls);

    uintptr_t nsPtr = remote_session_scratch(s, 1, 4);
    if (!nsPtr) return 0;
    if (!ns.empty()) {
        nsPtr = find_cstring(pid, ns.c_str());
        if (!nsPtr) return 0;
    }

    uintptr_t namePtr = find_cstring(pid, cls.c_str());
    if (!namePtr) return 0;

    for (size_t i = 0; i < assemblyCount; ++i) {
        uintptr_t asmPtr = 0;
        if (!mem_read(pid, assembliesArray + i * sizeof(uintptr_t), asmPtr) || !asmPtr) continue;
        uintptr_t image = 0;
        if (!remote_session_call(s, il2.sym_assembly_get_image, {asmPtr}, image) || !image)
            continue;
        uintptr_t klass = 0;
        if (!remote_session_call(s, il2.sym_class_from_name, {image, nsPtr, namePtr}, klass))
            continue;
        if (mem_ptr_ok(klass)) return klass;
    }
    return 0;
}

static uintptr_t method_from_name(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                  uintptr_t klass, const char *methodName, int argCount) {
    if (!il2.sym_class_get_method_from_name || !klass) return 0;
    const uintptr_t namePtr = find_cstring(pid, methodName);
    if (!namePtr) return 0;
    uintptr_t method = 0;
    if (!remote_session_call(s, il2.sym_class_get_method_from_name,
                             {klass, namePtr, (uintptr_t)argCount}, method))
        return 0;
    return mem_ptr_ok(method) ? method : 0;
}

static uintptr_t klass_static_fields(pid_t pid, uintptr_t klass) {
    if (!klass) return 0;
    for (uintptr_t off : Off::StaticFieldOffs) {
        uintptr_t st = 0;
        if (mem_read(pid, klass + off, st) && mem_ptr_ok(st)) return st;
    }
    return 0;
}

static uintptr_t static_field_ptr(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                    uintptr_t klass, const char *fieldName) {
    if (!il2.sym_class_get_field_from_name || !il2.sym_field_static_get_value || !klass)
        return 0;

    uintptr_t field = 0;
    const uintptr_t namePtr = find_cstring(pid, fieldName);
    if (!namePtr) return 0;
    if (!remote_session_call(s, il2.sym_class_get_field_from_name, {klass, namePtr}, field) ||
        !field)
        return 0;

    const uintptr_t valSlot = remote_session_scratch(s, 2, sizeof(uintptr_t));
    if (!valSlot) return 0;
    uintptr_t z = 0;
    mem_write(s.pid, valSlot, &z, sizeof(z));
    uintptr_t dummy = 0;
    if (!remote_session_call(s, il2.sym_field_static_get_value, {field, valSlot}, dummy))
        return 0;
    uintptr_t inst = 0;
    mem_read(s.pid, valSlot, inst);
    return mem_ptr_ok(inst) ? inst : 0;
}

} // namespace

bool conrent_runtime_ready(pid_t pid, RemoteIl2CppInfo &il2) {
    if (!il2.sym_domain_get || !il2.sym_domain_get_assemblies)
        return false;
    RemoteSession s{};
    if (!remote_session_begin(pid, s)) {
        ui_log("[conrent] runtime: %s", s.err);
        return false;
    }
    size_t n = 0;
    const bool ok = runtime_ready_session(s, il2, n);
    remote_session_end(s);
    if (ok)
        ui_log("[conrent] runtime_ready=1 assemblies=%zu domain=0x%llx", n,
               (unsigned long long)il2.domain_ptr);
    else
        ui_log("[conrent] runtime_ready=0 (%s)", s.err[0] ? s.err : "domain/assemblies");
    return ok;
}

bool conrent_resolve_battle_manager(pid_t pid, const std::vector<MapsReg> & /*maps*/,
                                    RemoteIl2CppInfo &il2, ConrentResolve &out) {
    out = ConrentResolve{};
    std::snprintf(out.method, sizeof(out.method), "conRentV1 EspTab ptrace IL2CPP");

    if (!il2.sym_domain_get || !il2.sym_domain_get_assemblies ||
        !il2.sym_class_from_name || !il2.sym_assembly_get_image ||
        !il2.sym_class_get_field_from_name || !il2.sym_field_static_get_value) {
        std::snprintf(out.detail, sizeof(out.detail),
                      "missing il2cpp exports (need domain+assemblies+class_from_name+field APIs)");
        return false;
    }

    RemoteSession s{};
    if (!remote_session_begin(pid, s)) {
        std::snprintf(out.detail, sizeof(out.detail), "%s", s.err);
        ui_log("[conrent] attach fail: %s", s.err);
        return false;
    }

    size_t asmCount = 0;
    uintptr_t domain = 0;
    if (!remote_session_call(s, il2.sym_domain_get, {}, domain) || !mem_ptr_ok(domain)) {
        std::snprintf(out.detail, sizeof(out.detail), "il2cpp_domain_get failed");
        remote_session_end(s);
        return false;
    }
    if (il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
    }

    const uintptr_t sizeSlot = remote_session_scratch(s, 0, sizeof(size_t));
    uintptr_t assemblies = 0;
    if (!sizeSlot ||
        !remote_session_call(s, il2.sym_domain_get_assemblies, {domain, sizeSlot}, assemblies) ||
        !mem_ptr_ok(assemblies) || !mem_read(pid, sizeSlot, asmCount) || asmCount == 0) {
        std::snprintf(out.detail, sizeof(out.detail), "domain_get_assemblies failed");
        remote_session_end(s);
        return false;
    }
    out.runtime_ready = true;
    il2.domain_ptr = domain;

    uintptr_t klass = find_class_conrent(s, pid, il2, "BattleManager", asmCount, assemblies);
    if (!klass && il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
        klass = find_class_conrent(s, pid, il2, "BattleManager", asmCount, assemblies);
        if (klass) ui_log("[conrent] FindClass OK on retry (thread re-attach)");
    }
    if (!klass) {
        std::snprintf(out.detail, sizeof(out.detail),
                      "FindClass(BattleManager) failed (%zu assemblies)", asmCount);
        remote_session_end(s);
        ui_log("[conrent] %s", out.detail);
        return false;
    }
    out.battle_manager_klass = klass;

    const uintptr_t inst = static_field_ptr(s, pid, il2, klass, "Instance");
    out.battle_manager = inst;

    const uintptr_t sbcKlass =
        find_class_conrent(s, pid, il2, "ShowBattleCamera", asmCount, assemblies);
    if (sbcKlass) out.show_battle_camera_statics = klass_static_fields(pid, sbcKlass);
    const uintptr_t gmKlass =
        find_class_conrent(s, pid, il2, "GameMethod", asmCount, assemblies);
    if (gmKlass) out.game_method_statics = klass_static_fields(pid, gmKlass);
    const uintptr_t camKlass =
        find_class_conrent(s, pid, il2, "UnityEngine.Camera", asmCount, assemblies);
    if (camKlass) {
        out.camera_get_main_method = method_from_name(s, pid, il2, camKlass, "get_main", 0);
        out.camera_w2s_method = method_from_name(s, pid, il2, camKlass, "WorldToScreenPoint", 1);
    }
    ui_log("[conrent] camera statics sbc=0x%llx gm=0x%llx w2s=%d",
           (unsigned long long)out.show_battle_camera_statics,
           (unsigned long long)out.game_method_statics,
           (out.camera_get_main_method && out.camera_w2s_method) ? 1 : 0);

    out.metadata_ready =
        klass && out.camera_get_main_method && out.camera_w2s_method && out.runtime_ready;

    remote_session_end(s);

    if (!out.metadata_ready) {
        std::snprintf(out.detail, sizeof(out.detail),
                      "metadata incomplete (klass/w2s/runtime) asm=%zu", asmCount);
        ui_log("[conrent] %s", out.detail);
        return false;
    }

    if (!inst) {
        std::snprintf(out.detail, sizeof(out.detail),
                      "metadata OK lobby: klass=0x%llx Instance=0 (enter match) asm=%zu",
                      (unsigned long long)klass, asmCount);
        ui_log("[conrent] %s", out.detail);
        return true;
    }

    std::snprintf(out.detail, sizeof(out.detail),
                  "klass=0x%llx Instance=0x%llx asm=%zu (EspTab resolveOffsets)",
                  (unsigned long long)klass, (unsigned long long)inst, asmCount);
    ui_log("[conrent] OK %s", out.detail);
    return true;
}

bool conrent_refresh_battle_instance(pid_t pid, RemoteIl2CppInfo &il2, uintptr_t battleManagerKlass,
                                     uintptr_t &instanceOut) {
    instanceOut = 0;
    if (!battleManagerKlass || !il2.sym_domain_get || !il2.sym_field_static_get_value ||
        !il2.sym_class_get_field_from_name)
        return false;

    RemoteSession s{};
    if (!remote_session_begin_try(pid, s)) return false;

    if (il2.sym_domain_get && il2.sym_thread_attach) {
        uintptr_t domain = 0;
        if (remote_session_call(s, il2.sym_domain_get, {}, domain))
            remote_session_call(s, il2.sym_thread_attach, {domain}, domain);
    }

    instanceOut = static_field_ptr(s, pid, il2, battleManagerKlass, "Instance");
    remote_session_end(s);
    return mem_ptr_ok(instanceOut);
}
