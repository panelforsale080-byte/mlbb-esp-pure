#include "conrent_esp.hpp"
#include "esp_retri_resolve.hpp"
#include "camera_lib_chain.hpp"
#include "il2_layout.hpp"
#include "il2_lib_globals.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <algorithm>
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
        static const char *kCSharpAsm[] = {"Assembly-CSharp", "Assembly-CSharp.dll",
                                           "Assembly-CSharp-firstpass.dll"};
        for (const char *csName : kCSharpAsm) {
            if (out.assemblyCSharp) break;
            const uintptr_t np = scratch_cstr(s, pid, 3, csName);
            if (!np) continue;
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
    uintptr_t nsPtr = scratch_cstr(s, pid, 5, ns.empty() ? "" : ns.c_str());
    if (!nsPtr) return 0;
    const uintptr_t namePtr = scratch_cstr(s, pid, 6, cls.c_str());
    if (!namePtr) return 0;
    uintptr_t klass = 0;
    if (!remote_session_call(s, il2.sym_class_from_name, {image, nsPtr, namePtr}, klass))
        return 0;
    return mem_ptr_ok(klass) ? klass : 0;
}

static bool klass_name_matches(pid_t pid, uintptr_t klass, const std::string &wantCls) {
    if (!klass) return false;
    for (uintptr_t nameOff : {0x10u, 0x18u, 0x08u}) {
        uintptr_t nameP = 0;
        if (!mem_read(pid, klass + nameOff, nameP) || !nameP) continue;
        char buf[96] = {};
        if (!mem_read(pid, nameP, buf, sizeof(buf) - 1)) continue;
        if (wantCls == buf) return true;
        const char *dot = std::strrchr(buf, '.');
        if (dot && wantCls == (dot + 1)) return true;
    }
    return false;
}

static uintptr_t find_class_scan_images(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                        const ResolveImages &img, const std::string &wantCls) {
    if (!il2.sym_image_get_class || !il2.sym_image_get_class_count) return 0;
    for (uintptr_t image : img.all) {
        uintptr_t count = 0;
        if (!remote_session_call(s, il2.sym_image_get_class_count, {image}, count)) continue;
        const size_t n = (size_t)count;
        if (n == 0 || n > 12000) continue;
        const size_t limit = std::min(n, (size_t)6000);
        for (size_t i = 0; i < limit; ++i) {
            uintptr_t klass = 0;
            if (!remote_session_call(s, il2.sym_image_get_class, {image, i}, klass) || !klass)
                continue;
            if (klass_name_matches(pid, klass, wantCls)) return klass;
        }
    }
    return 0;
}

static uintptr_t find_class_fast(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                 const ResolveImages &img, const char *fullName,
                                 bool preferUnityImage) {
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
    if (!klass) klass = find_class_scan_images(s, pid, il2, img, cls);
    if (klass) g_classCache[key] = klass;
    return klass;
}

static uintptr_t method_native_ptr(pid_t pid, uintptr_t methodInfo) {
    if (!methodInfo) return 0;
    for (uintptr_t off : {0u, 0x8u, 0x10u}) {
        uintptr_t fn = 0;
        if (mem_read(pid, methodInfo + off, fn) && fn > 0x10000 && mem_ptr_ok(fn)) return fn;
    }
    return 0;
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
    for (uintptr_t off : Il2::StaticFieldOffs) {
        uintptr_t st = 0;
        if (mem_read(pid, klass + off, st) && mem_ptr_ok(st)) return st;
    }
    return 0;
}

static uintptr_t instance_field_offset(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                       uintptr_t klass, const char *fieldName) {
    if (!klass || !fieldName || !il2.sym_class_get_field_from_name) return 0;
    const uintptr_t namePtr = scratch_cstr(s, pid, 10, fieldName);
    if (!namePtr) return 0;
    uintptr_t field = 0;
    if (!remote_session_call(s, il2.sym_class_get_field_from_name, {klass, namePtr}, field) ||
        !field)
        return 0;
    if (il2.sym_field_get_offset) {
        uintptr_t off = 0;
        if (remote_session_call(s, il2.sym_field_get_offset, {field}, off) && off > 0 &&
            off < 0x4000)
            return off;
    }
    for (uintptr_t fo : {0x18u, 0x1Cu, 0x20u, 0x14u, 0x10u}) {
        uint32_t o = 0;
        if (mem_read(pid, field + fo, o) && o >= 0x08 && o < 0x2000) return o;
    }
    return 0;
}

static void fill_runtime_field_offsets(RemoteSession &s, pid_t pid, RemoteIl2CppInfo &il2,
                                       const ResolveImages &images, ConrentResolve &out) {
    const uintptr_t bmKlass = out.battle_manager_klass;
    if (bmKlass) {
        out.runtime_fields.bm_m_LocalPlayerShow =
            instance_field_offset(s, pid, il2, bmKlass, "m_LocalPlayerShow");
        out.runtime_fields.bm_m_dicPlayerShow =
            instance_field_offset(s, pid, il2, bmKlass, "m_dicPlayerShow");
        out.runtime_fields.bm_m_dicMonsterShow =
            instance_field_offset(s, pid, il2, bmKlass, "m_dicMonsterShow");
        out.runtime_fields.bm_m_ShowPlayerList =
            instance_field_offset(s, pid, il2, bmKlass, "m_ShowPlayers");
        if (!out.runtime_fields.bm_m_ShowPlayerList)
            out.runtime_fields.bm_m_ShowPlayerList =
                instance_field_offset(s, pid, il2, bmKlass, "m_ShowPlayerList");
        if (!out.runtime_fields.bm_m_ShowPlayerList)
            out.runtime_fields.bm_m_ShowPlayerList =
                instance_field_offset(s, pid, il2, bmKlass, "m_listPlayerShow");
        out.runtime_fields.bm_m_ShowMonsterList =
            instance_field_offset(s, pid, il2, bmKlass, "m_ShowMonsters");
        if (!out.runtime_fields.bm_m_ShowMonsterList)
            out.runtime_fields.bm_m_ShowMonsterList =
                instance_field_offset(s, pid, il2, bmKlass, "m_ShowMonsterList");
        out.runtime_fields.bm_static_m_RunBullets =
            instance_field_offset(s, pid, il2, bmKlass, "m_RunBullets");
        out.runtime_fields.bm_static_RunBulletsList =
            instance_field_offset(s, pid, il2, bmKlass, "_RunBullets");
    }
    uintptr_t seKlass = find_class_fast(s, pid, il2, images, "ShowEntity", false);
    out.runtime_fields.show_entity_klass = seKlass;
    if (seKlass) {
        out.runtime_fields.se_LastPosition =
            instance_field_offset(s, pid, il2, seKlass, "<LastPosition>k__BackingField");
        if (!out.runtime_fields.se_LastPosition)
            out.runtime_fields.se_LastPosition =
                instance_field_offset(s, pid, il2, seKlass, "LastPosition");
        out.runtime_fields.se_CachePosition =
            instance_field_offset(s, pid, il2, seKlass, "<CachePosition>k__BackingField");
        if (!out.runtime_fields.se_CachePosition)
            out.runtime_fields.se_CachePosition =
                instance_field_offset(s, pid, il2, seKlass, "CachePosition");
        out.runtime_fields.se_vCachePosition =
            instance_field_offset(s, pid, il2, seKlass, "m_vCachePosition");
        if (!out.runtime_fields.se_vCachePosition)
            out.runtime_fields.se_vCachePosition =
                instance_field_offset(s, pid, il2, seKlass, "vCachePosition");
        out.runtime_fields.se_m_ID = instance_field_offset(s, pid, il2, seKlass, "m_ID");
        uintptr_t smKlass = find_class_fast(s, pid, il2, images, "ShowMonster", false);
        if (smKlass)
            out.runtime_fields.sm_m_Config =
                instance_field_offset(s, pid, il2, smKlass, "m_Config");
        uintptr_t monKlass = find_class_fast(s, pid, il2, images, "CData_Monster_Element", false);
        if (monKlass) {
            out.runtime_fields.monster_m_ID =
                instance_field_offset(s, pid, il2, monKlass, "m_ID");
            out.runtime_fields.monster_m_MonsterName =
                instance_field_offset(s, pid, il2, monKlass, "m_MonsterName");
            out.runtime_fields.monster_m_Prefabs =
                instance_field_offset(s, pid, il2, monKlass, "m_Prefabs");
        }
        out.runtime_fields.se_m_RoleName =
            instance_field_offset(s, pid, il2, seKlass, "m_RoleName");
        uintptr_t wildKlass = find_class_fast(s, pid, il2, images, "ShowWildMonster", false);
        if (wildKlass)
            out.runtime_fields.sw_m_wildType =
                instance_field_offset(s, pid, il2, wildKlass, "m_wildType");
        out.runtime_fields.se_m_bDeath = instance_field_offset(s, pid, il2, seKlass, "m_bDeath");
        out.runtime_fields.se_m_bSameCampType =
            instance_field_offset(s, pid, il2, seKlass, "m_bSameCampType");
        out.runtime_fields.se_m_Hp = instance_field_offset(s, pid, il2, seKlass, "m_Hp");
        out.runtime_fields.se_m_HpMax = instance_field_offset(s, pid, il2, seKlass, "m_HpMax");
        out.runtime_fields.se_m_Level = instance_field_offset(s, pid, il2, seKlass, "m_Level");
        out.runtime_fields.se_m_ShowCoolDownComp =
            instance_field_offset(s, pid, il2, seKlass, "m_ShowCoolDownComp");
    }

    uintptr_t spKlass = find_class_fast(s, pid, il2, images, "ShowPlayer", false);
    out.runtime_fields.show_player_klass = spKlass;
    if (spKlass) {
        out.runtime_fields.sp_m_HeroName =
            instance_field_offset(s, pid, il2, spKlass, "m_HeroName");
        out.runtime_fields.sp_m_iSummonSkillId =
            instance_field_offset(s, pid, il2, spKlass, "m_iSummonSkillId");
        out.runtime_fields.sp_m_KillWildTimes =
            instance_field_offset(s, pid, il2, spKlass, "m_KillWildTimes");
        out.runtime_fields.sp_m_iOriginHeroId =
            instance_field_offset(s, pid, il2, spKlass, "m_iOriginHeroId");
    }

    uintptr_t bulKlass = find_class_fast(s, pid, il2, images, "Battle.Bullet", false);
    if (!bulKlass) bulKlass = find_class_fast(s, pid, il2, images, "Bullet", false);
    if (bulKlass) {
        out.runtime_fields.bul_m_Id = instance_field_offset(s, pid, il2, bulKlass, "m_Id");
        out.runtime_fields.bul_bulletPos =
            instance_field_offset(s, pid, il2, bulKlass, "_bulletPos");
        if (!out.runtime_fields.bul_bulletPos)
            out.runtime_fields.bul_bulletPos =
                instance_field_offset(s, pid, il2, bulKlass, "bulletPos");
        out.runtime_fields.bul_bulletForward =
            instance_field_offset(s, pid, il2, bulKlass, "bulletForward");
    }

    /* dump.cs: Namespace Battle — unqualified name misses class_from_name and falls back to
     * find_class_scan_images (slow: up to 6000 classes/image). */
    uintptr_t sccKlass = find_class_fast(s, pid, il2, images, "Battle.ShowCoolDownComp", false);
    if (!sccKlass)
        sccKlass = find_class_fast(s, pid, il2, images, "ShowCoolDownComp", false);
    if (sccKlass) {
        out.runtime_fields.scc__dicCD =
            instance_field_offset(s, pid, il2, sccKlass, "_dicCD");
    }

    uintptr_t cddKlass = find_class_fast(s, pid, il2, images, "Battle.CoolDownData", false);
    if (!cddKlass) cddKlass = find_class_fast(s, pid, il2, images, "CoolDownData", false);
    if (cddKlass) {
        out.runtime_fields.cdd_iSpellID = instance_field_offset(s, pid, il2, cddKlass, "iSpellID");
        out.runtime_fields.cdd_uiCoolTime =
            instance_field_offset(s, pid, il2, cddKlass, "uiCoolTime");
        out.runtime_fields.cdd_uiStartTime =
            instance_field_offset(s, pid, il2, cddKlass, "uiStartTime");
    }

    const uintptr_t gmKlass = find_class_fast(s, pid, il2, images, "GameMethod", false);
    if (gmKlass) {
        out.runtime_fields.gm_mainCamera =
            instance_field_offset(s, pid, il2, gmKlass, "mainCamera");
    }
    const uintptr_t sfKlass = find_class_fast(s, pid, il2, images, "SmoothFollow", false);
    if (sfKlass) {
        out.runtime_fields.sf_m_MainCamera =
            instance_field_offset(s, pid, il2, sfKlass, "m_MainCamera");
    }
    const uintptr_t sbcKlass = find_class_fast(s, pid, il2, images, "ShowBattleCamera", false);
    if (sbcKlass) {
        out.runtime_fields.sbc_m_CamPos =
            instance_field_offset(s, pid, il2, sbcKlass, "m_CamPos");
        out.runtime_fields.sbc_m_CamRot =
            instance_field_offset(s, pid, il2, sbcKlass, "m_CamRot");
        out.runtime_fields.sbc_m_iFieldOfView =
            instance_field_offset(s, pid, il2, sbcKlass, "m_iFieldOfView");
    }

    ui_log("[resolve] camera runtime (IL2CPP fields, no lib Offsets.h):");
    ui_log("  GameMethod statics=0x%llx mainCamera(IL2)=0x%llx ShowBattleCamera statics=0x%llx",
           (unsigned long long)out.game_method_statics,
           (unsigned long long)out.game_method_main_camera_obj,
           (unsigned long long)out.show_battle_camera_statics);
    ui_log("  GameMethod.mainCamera static off=0x%llx",
           (unsigned long long)out.runtime_fields.gm_mainCamera);
    ui_log("  SmoothFollow.m_MainCamera off=0x%llx",
           (unsigned long long)out.runtime_fields.sf_m_MainCamera);
    ui_log("  ShowBattleCamera m_CamPos=0x%llx m_CamRot=0x%llx FOV=0x%llx",
           (unsigned long long)out.runtime_fields.sbc_m_CamPos,
           (unsigned long long)out.runtime_fields.sbc_m_CamRot,
           (unsigned long long)out.runtime_fields.sbc_m_iFieldOfView);
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
    g_classCache.clear();
    g_classPid = pid;
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

    uintptr_t domain = 0;
    if (!il2.sym_domain_get ||
        !remote_session_call(s, il2.sym_domain_get, {}, domain) || !mem_ptr_ok(domain)) {
        std::snprintf(out.detail, sizeof(out.detail), "il2cpp_domain_get failed");
        remote_session_end(s);
        return false;
    }

    if (il2.sym_thread_attach) {
        uintptr_t thr = 0;
        remote_session_call(s, il2.sym_thread_attach, {domain}, thr);
    }

    size_t asmCount = 0;
    uintptr_t assemblies = 0;
    const uintptr_t sizeSlot = remote_session_scratch(s, 0, sizeof(size_t));
    if (!il2.sym_domain_get_assemblies || !sizeSlot) {
        std::snprintf(out.detail, sizeof(out.detail), "missing domain_get_assemblies");
        remote_session_end(s);
        return false;
    }
    size_t zero = 0;
    mem_write(s.pid, sizeSlot, &zero, sizeof(zero));
    if (!remote_session_call(s, il2.sym_domain_get_assemblies, {domain, sizeSlot}, assemblies) ||
        !mem_read(pid, sizeSlot, asmCount)) {
        std::snprintf(out.detail, sizeof(out.detail), "domain_get_assemblies call failed");
        remote_session_end(s);
        return false;
    }
    if (!mem_ptr_ok(assemblies) || asmCount == 0 || asmCount > 512) {
        std::snprintf(out.detail, sizeof(out.detail), "assemblies=0 count=%zu domain=0x%llx",
                      asmCount, (unsigned long long)domain);
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
        std::snprintf(out.detail, sizeof(out.detail),
                      "FindClass(BattleManager) failed asm=%zu imgCS=0x%llx imgAll=%zu",
                      asmCount, (unsigned long long)images.assemblyCSharp, images.all.size());
        ui_log("[conrent] %s", out.detail);
        remote_session_end(s);
        return false;
    }
    out.battle_manager_klass = klass;
    out.battle_manager_statics = klass_static_fields(pid, klass);
    out.battle_manager = static_field_ptr(s, pid, il2, klass, "Instance");

    const uintptr_t sbcKlass = find_class_fast(s, pid, il2, images, "ShowBattleCamera", false);
    if (sbcKlass) out.show_battle_camera_statics = klass_static_fields(pid, sbcKlass);
    const uintptr_t gmKlass = find_class_fast(s, pid, il2, images, "GameMethod", false);
    if (gmKlass) {
        out.game_method_statics = klass_static_fields(pid, gmKlass);
        out.game_method_main_camera_obj =
            mem_ptr_normalize(static_field_ptr(s, pid, il2, gmKlass, "mainCamera"));
    }

    const uintptr_t camKlass =
        find_class_fast(s, pid, il2, images, "UnityEngine.Camera", true);
    if (camKlass) {
        out.camera_get_main_method = method_from_name(s, pid, il2, camKlass, "get_main", 0);
        out.camera_w2s_method =
            method_from_name(s, pid, il2, camKlass, "WorldToScreenPoint", 1);
        out.camera_world_to_camera_method =
            method_from_name(s, pid, il2, camKlass, "get_worldToCameraMatrix", 0);
        out.camera_projection_matrix_method =
            method_from_name(s, pid, il2, camKlass, "get_projectionMatrix", 0);
        out.camera_get_main_native = method_native_ptr(pid, out.camera_get_main_method);
        out.camera_w2s_native = method_native_ptr(pid, out.camera_w2s_method);
        ui_log("[resolve] Camera methods: get_main=0x%llx w2s=0x%llx w2c=0x%llx proj=0x%llx",
               (unsigned long long)out.camera_get_main_method,
               (unsigned long long)out.camera_w2s_method,
               (unsigned long long)out.camera_world_to_camera_method,
               (unsigned long long)out.camera_projection_matrix_method);
    }

    const uintptr_t bdKlass = find_class_fast(s, pid, il2, images, "BattleData", false);
    if (bdKlass) {
        const uintptr_t bridgeKlass = find_class_fast(s, pid, il2, images, "BattleBridge", false);
        if (bridgeKlass) {
            const uintptr_t m = method_from_name(s, pid, il2, bridgeKlass, "get_bStartBattle", 0);
            out.get_bStartBattle_native = method_native_ptr(pid, m);
        }
        out.battle_bridge = static_field_ptr(s, pid, il2, bdKlass, "m_BattleBridge");
    }
    out.metadata_ready =
        klass && out.camera_get_main_method && out.camera_w2s_method && out.runtime_ready;

    fill_runtime_field_offsets(s, pid, il2, images, out);

    out.retri_show_self_player_klass =
        find_class_fast(s, pid, il2, images, "ShowSelfPlayer", false);
    if (out.retri_show_self_player_klass) {
        out.retri_try_use_skill_mi =
            method_from_name(s, pid, il2, out.retri_show_self_player_klass, "TryUseSkill", 9);
        if (!out.retri_try_use_skill_mi)
            out.retri_try_use_skill_mi =
                method_from_name(s, pid, il2, out.retri_show_self_player_klass, "TryUseSkill", 7);
    }

    ui_log("[resolve] mod menu + auto retri classes (FindClass):");
    for (unsigned i = 0; i < kEspResolveFindClassCount; ++i) {
        const char *cn = kEspResolveFindClasses[i];
        const bool unity = std::strstr(cn, "UnityEngine") != nullptr;
        const uintptr_t k = find_class_fast(s, pid, il2, images, cn, unity);
        ui_log("  %s -> 0x%llx", cn, (unsigned long long)k);
    }

    remote_session_end(s);
    il2_discover_lib_global_rvas(pid, il2.base, out);

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

static uintptr_t il2_name_scratch(RemoteSession &s, pid_t pid, int slot, const char *lit) {
    if (!lit) lit = "";
    const size_t n = std::strlen(lit) + 1;
    const uintptr_t p = remote_session_scratch(s, slot, n);
    if (!p) return 0;
    mem_write(s.pid, p, lit, n);
    return p;
}

bool conrent_il2_static_field_ptr(pid_t pid, RemoteIl2CppInfo &il2, uintptr_t klass,
                                    const char *fieldName, uintptr_t &outPtr) {
    outPtr = 0;
    if (!klass || !fieldName || !il2.sym_class_get_field_from_name ||
        !il2.sym_field_static_get_value)
        return false;
    RemoteSession s{};
    if (!remote_session_begin_try(pid, s)) return false;
    const uintptr_t namePtr = il2_name_scratch(s, pid, 11, fieldName);
    if (!namePtr) {
        remote_session_end(s);
        return false;
    }
    uintptr_t field = 0;
    if (!remote_session_call(s, il2.sym_class_get_field_from_name, {klass, namePtr}, field) ||
        !field) {
        remote_session_end(s);
        return false;
    }
    const uintptr_t valSlot = remote_session_scratch(s, 2, sizeof(uintptr_t));
    if (!valSlot) {
        remote_session_end(s);
        return false;
    }
    uintptr_t z = 0;
    mem_write(s.pid, valSlot, &z, sizeof(z));
    uintptr_t dummy = 0;
    if (!remote_session_call(s, il2.sym_field_static_get_value, {field, valSlot}, dummy)) {
        remote_session_end(s);
        return false;
    }
    mem_read(s.pid, valSlot, outPtr);
    remote_session_end(s);
    return outPtr != 0 && outPtr > 0xffffULL && mem_ptr_ok(outPtr);
}
