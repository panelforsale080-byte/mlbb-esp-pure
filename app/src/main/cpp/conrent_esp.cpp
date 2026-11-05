#include "conrent_esp.hpp"
#include "offsets.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

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

/** conRent-style: write IL2CPP name strings into tracee stack (no /maps string scan). */
static uintptr_t scratch_cstr(RemoteSession &s, pid_t pid, int slot, const char *lit) {
    if (!lit) lit = "";
    const size_t n = std::strlen(lit) + 1;
    const uintptr_t p = remote_session_scratch(s, slot, n);
    if (!p || !mem_write(pid, p, lit, n)) return 0;
    return p;
}

struct ResolveImages {
    uintptr_t assemblyCSharp = 0;
    uintptr_t unityCore = 0;
    std::vector<uintptr_t> all;
};

static pid_t g_classPid = 0;
static std::unordered_map<std::string, uintptr_t> g_classCache;

static void build_resolve_images(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                 uintptr_t domain, size_t asmCount, uintptr_t assembliesArray,
                                 ResolveImages &out) {
    out = {};
    if (il2.sym_domain_assembly_open && mem_ptr_ok(domain)) {
        const uintptr_t np = scratch_cstr(s, pid, 4, "Assembly-CSharp");
        if (np) {
            uintptr_t as = 0;
            if (remote_session_call(s, il2.sym_domain_assembly_open, {domain, np}, as) && as)
                remote_session_call(s, il2.sym_assembly_get_image, {as}, out.assemblyCSharp);
        }
        static const char *kUnityAsm[] = {"UnityEngine.CoreModule.dll", "UnityEngine.CoreModule",
                                          "UnityEngine.dll"};
        for (const char *unityName : kUnityAsm) {
            if (out.unityCore) break;
            const uintptr_t up = scratch_cstr(s, pid, 4, unityName);
            if (!up) continue;
            uintptr_t as = 0;
            if (remote_session_call(s, il2.sym_domain_assembly_open, {domain, up}, as) && as) {
                uintptr_t img = 0;
                if (remote_session_call(s, il2.sym_assembly_get_image, {as}, img) && img)
                    out.unityCore = img;
            }
        }
    }

    for (size_t i = 0; i < asmCount && out.all.size() < 80; ++i) {
        uintptr_t asmPtr = 0;
        if (!mem_read(pid, assembliesArray + i * sizeof(uintptr_t), asmPtr) || !asmPtr) continue;
        uintptr_t image = 0;
        if (!remote_session_call(s, il2.sym_assembly_get_image, {asmPtr}, image) || !image) continue;
        bool dup = false;
        for (uintptr_t x : out.all)
            if (x == image) {
                dup = true;
                break;
            }
        if (!dup) out.all.push_back(image);
    }
}

static uintptr_t class_from_name_image(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                       uintptr_t image, const std::string &ns,
                                       const std::string &cls) {
    if (!image || !il2.sym_class_from_name) return 0;
    uintptr_t nsPtr = 0;
    if (!ns.empty()) {
        nsPtr = scratch_cstr(s, pid, 5, ns.c_str());
        if (!nsPtr) return 0;
    }
    const uintptr_t namePtr = scratch_cstr(s, pid, 6, cls.c_str());
    if (!namePtr) return 0;
    uintptr_t klass = 0;
    if (!remote_session_call(s, il2.sym_class_from_name, {image, nsPtr, namePtr}, klass))
        return 0;
    return mem_ptr_ok(klass) ? klass : 0;
}

static uintptr_t find_class_fast(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                 const ResolveImages &img, const char *fullName,
                                 bool preferUnityImage) {
    if (g_classPid != pid) {
        g_classCache.clear();
        g_classPid = pid;
    }
    const std::string key(fullName);
    const auto hit = g_classCache.find(key);
    if (hit != g_classCache.end()) return hit->second;

    std::string ns, cls;
    split_class_name(fullName, ns, cls);

    uintptr_t klass = 0;
    if (preferUnityImage && img.unityCore)
        klass = class_from_name_image(s, pid, il2, img.unityCore, ns, cls);
    if (!klass && img.assemblyCSharp)
        klass = class_from_name_image(s, pid, il2, img.assemblyCSharp, ns, cls);
    if (!klass && !preferUnityImage && img.unityCore)
        klass = class_from_name_image(s, pid, il2, img.unityCore, ns, cls);
    if (!klass) {
        for (uintptr_t image : img.all) {
            klass = class_from_name_image(s, pid, il2, image, ns, cls);
            if (klass) break;
        }
    }
    g_classCache[key] = klass;
    return klass;
}

static uintptr_t method_from_name(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                  uintptr_t klass, const char *methodName, int argCount) {
    if (!il2.sym_class_get_method_from_name || !klass) return 0;
    const uintptr_t namePtr = scratch_cstr(s, pid, 7, methodName);
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

    const uintptr_t namePtr = scratch_cstr(s, pid, 8, fieldName);
    if (!namePtr) return 0;

    uintptr_t field = 0;
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
    if (!remote_session_begin(pid, s)) return false;
    uintptr_t domain = mem_ptr_ok(il2.domain_ptr) ? il2.domain_ptr : 0;
    if (!domain && !remote_session_call(s, il2.sym_domain_get, {}, domain)) {
        remote_session_end(s);
        return false;
    }
    if (il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
    }
    const uintptr_t sizeSlot = remote_session_scratch(s, 0, sizeof(size_t));
    size_t n = 0;
    uintptr_t assemblies = 0;
    const bool ok = sizeSlot &&
                    remote_session_call(s, il2.sym_domain_get_assemblies, {domain, sizeSlot},
                                        assemblies) &&
                    mem_ptr_ok(assemblies) && mem_read(pid, sizeSlot, n) && n > 0;
    if (ok) il2.domain_ptr = domain;
    remote_session_end(s);
    return ok;
}

bool conrent_resolve_battle_manager(pid_t pid, const std::vector<MapsReg> & /*maps*/,
                                    RemoteIl2CppInfo &il2, ConrentResolve &out) {
    out = ConrentResolve{};
    std::snprintf(out.method, sizeof(out.method), "conRentV1 fast ptrace IL2CPP");

    if (!il2.sym_class_from_name || !il2.sym_assembly_get_image ||
        !il2.sym_class_get_field_from_name || !il2.sym_field_static_get_value) {
        std::snprintf(out.detail, sizeof(out.detail), "missing il2cpp class/field exports");
        return false;
    }

    RemoteSession s{};
    if (!remote_session_begin(pid, s)) {
        std::snprintf(out.detail, sizeof(out.detail), "%s", s.err);
        return false;
    }

    uintptr_t domain = mem_ptr_ok(il2.domain_ptr) ? il2.domain_ptr : 0;
    if (!domain) {
        if (!il2.sym_domain_get ||
            !remote_session_call(s, il2.sym_domain_get, {}, domain) || !mem_ptr_ok(domain)) {
            std::snprintf(out.detail, sizeof(out.detail), "il2cpp_domain_get failed");
            remote_session_end(s);
            return false;
        }
    }

    if (il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
    }

    size_t asmCount = 0;
    uintptr_t assemblies = 0;
    const uintptr_t sizeSlot = remote_session_scratch(s, 0, sizeof(size_t));
    if (il2.sym_domain_get_assemblies && sizeSlot) {
        size_t zero = 0;
        mem_write(s.pid, sizeSlot, &zero, sizeof(zero));
        remote_session_call(s, il2.sym_domain_get_assemblies, {domain, sizeSlot}, assemblies);
        mem_read(pid, sizeSlot, asmCount);
    }
    if (!mem_ptr_ok(assemblies) || asmCount == 0 || asmCount > 512) {
        std::snprintf(out.detail, sizeof(out.detail), "domain_get_assemblies failed");
        remote_session_end(s);
        return false;
    }
    out.runtime_ready = true;
    il2.domain_ptr = domain;

    ResolveImages images{};
    build_resolve_images(s, pid, il2, domain, asmCount, assemblies, images);

    uintptr_t klass = find_class_fast(s, pid, il2, images, "BattleManager", false);
    if (!klass && il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
        klass = find_class_fast(s, pid, il2, images, "BattleManager", false);
    }
    if (!klass) {
        std::snprintf(out.detail, sizeof(out.detail), "FindClass(BattleManager) failed");
        remote_session_end(s);
        return false;
    }
    out.battle_manager_klass = klass;
    out.battle_manager_statics = klass_static_fields(pid, klass);
    out.battle_manager = static_field_ptr(s, pid, il2, klass, "Instance");

    const uintptr_t sbcKlass = find_class_fast(s, pid, il2, images, "ShowBattleCamera", false);
    if (sbcKlass) out.show_battle_camera_statics = klass_static_fields(pid, sbcKlass);
    const uintptr_t gmKlass = find_class_fast(s, pid, il2, images, "GameMethod", false);
    if (gmKlass) out.game_method_statics = klass_static_fields(pid, gmKlass);

    const uintptr_t camKlass =
        find_class_fast(s, pid, il2, images, "UnityEngine.Camera", true);
    if (camKlass) {
        out.camera_get_main_method = method_from_name(s, pid, il2, camKlass, "get_main", 0);
        out.camera_w2s_method =
            method_from_name(s, pid, il2, camKlass, "WorldToScreenPoint", 1);
    }
    out.metadata_ready =
        klass && out.camera_get_main_method && out.camera_w2s_method && out.runtime_ready;

    remote_session_end(s);

    if (!out.battle_manager) {
        std::snprintf(out.detail, sizeof(out.detail),
                      "W2S OK, Instance=0 (enter match) klass=0x%llx",
                      (unsigned long long)klass);
        return out.metadata_ready;
    }

    std::snprintf(out.detail, sizeof(out.detail), "klass=0x%llx Instance=0x%llx (conRent fast)",
                  (unsigned long long)klass, (unsigned long long)out.battle_manager);
    ui_log("[conrent] OK %s", out.detail);
    return true;
}
