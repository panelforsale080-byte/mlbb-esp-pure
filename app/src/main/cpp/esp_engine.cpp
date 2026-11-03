#include "esp_engine.hpp"
#include "offsets.hpp"
#include "proc.hpp"
#include "remote_il2cpp.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <elf.h>
#include <mutex>

namespace {

std::mutex g_mu;
pid_t g_pid = 0;
uintptr_t g_bmKlass = 0;
uintptr_t g_bmStatics = 0;
uintptr_t g_bmInstance = 0;
bool g_resolved = false;
EspResolveReport g_lastReport{};

struct BmFindMeta {
    int nameStrings = 0;
    bool usedElf = false;
};

static bool name_at(pid_t pid, uintptr_t p, const char *want) {
    char buf[80] = {};
    if (!mem_read(pid, p, buf, sizeof(buf) - 1)) return false;
    return std::strcmp(buf, want) == 0;
}

static const char *tail_name(const char *full) {
    const char *d = std::strrchr(full, '.');
    return d ? d + 1 : full;
}

static bool map_for_strings(const MapsReg &r) {
    if (r.perms[0] != 'r' || r.end <= r.start) return false;
    const size_t sz = (size_t)(r.end - r.start);
    if (sz < 16 || sz > 128 * 1024 * 1024) return false;
    const std::string &p = r.path;
    if (p.find("[stack]") != std::string::npos) return false;
    if (p.find("global-metadata") != std::string::npos) return true;
    if (p.find("liblogic") != std::string::npos) return true;
    if (p.find("csharp") != std::string::npos) return true;
    if (p.find("il2cpp") != std::string::npos) return true;
    if (p.find(".apk") != std::string::npos) return true;
    if (p.find(".dat") != std::string::npos) return true;
    if (p.find(".so") != std::string::npos) return sz <= 48 * 1024 * 1024;
    if (p.empty() || p[0] == '[') return sz <= 32 * 1024 * 1024;
    return sz <= 16 * 1024 * 1024;
}

static int klass_region_prio(const MapsReg &r) {
    if (r.perms[0] != 'r' || r.perms[1] != 'w' || r.end <= r.start) return 0;
    const size_t sz = (size_t)(r.end - r.start);
    if (sz < 0x40 || sz > 96 * 1024 * 1024) return 0;
    const std::string &p = r.path;
    if (p.find("[stack]") != std::string::npos) return 0;
    if (p.find("liblogic") != std::string::npos) return 100;
    if (p.find("csharp") != std::string::npos) return 90;
    if (p.find("libil2cpp") != std::string::npos) return 85;
    if (p.find("il2cpp") != std::string::npos) return 80;
    if (p.find("libunity") != std::string::npos) return 70;
    if (p.find(".so") != std::string::npos) return 40;
    if (p.empty() || p.find("[anon:") != std::string::npos) {
        if (sz <= 12 * 1024 * 1024) return 30;
        return 0;
    }
    return 0;
}

static void scan_strings(pid_t pid, const std::vector<MapsReg> &maps,
                         const char *needle, std::vector<uintptr_t> &out) {
    out.clear();
    const size_t nlen = std::strlen(needle) + 1;
    for (const auto &m : maps) {
        if (!map_for_strings(m)) continue;
        const size_t sz = (size_t)(m.end - m.start);
        const size_t step = 1024 * 1024;
        for (size_t off = 0; off < sz && out.size() < 32; off += step) {
            const size_t n = std::min(step + nlen, sz - off);
            std::vector<char> buf(n);
            if (!mem_read(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + nlen <= n; ++i) {
                if (std::memcmp(buf.data() + i, needle, nlen) == 0)
                    out.push_back(m.start + off + i);
            }
        }
    }
}

static bool dict_entries(pid_t pid, uintptr_t dict,
                         std::vector<std::pair<int, uintptr_t>> &out) {
    out.clear();
    if (!dict) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!mem_read(pid, dict + Off::DictEntries, entries) || !entries) return false;
    if (!mem_read(pid, dict + Off::DictCount, count) || count <= 0 || count > 256)
        return false;
    int maxLength = 0;
    if (!mem_read(pid, entries + Off::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 512)
        return false;
    const uintptr_t data = entries + Off::ArrData;
    for (int i = 0; i < maxLength; ++i) {
        const uintptr_t entry = data + (uintptr_t)i * Off::EntryStride;
        int hash = 0, key = 0;
        uintptr_t value = 0;
        if (!mem_read(pid, entry + Off::EntryHash, hash) || hash < 0) continue;
        if (!mem_read(pid, entry + Off::EntryKey, key)) continue;
        if (!mem_read(pid, entry + Off::EntryValue, value) || !value) continue;
        out.emplace_back(key, value);
        if ((int)out.size() >= count) break;
    }
    return !out.empty();
}

static bool pos_ok(const Vec3 &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && !(std::fabs(p.x) < 0.01f && std::fabs(p.z) < 0.01f)
        && std::fabs(p.x) <= 10000.f && std::fabs(p.z) <= 10000.f;
}

static bool monster_id(int id) {
    for (int m : Off::kMonsterIds)
        if (m == id) return true;
    return false;
}

static bool statics_for(pid_t pid, uintptr_t klass, uintptr_t instOff,
                        uintptr_t &statics, uintptr_t &instance) {
    for (uintptr_t off : Off::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!mem_read(pid, klass + off, st) || !mem_ptr_ok(st)) continue;
        uintptr_t inst = 0;
        mem_read(pid, st + instOff, inst);
        statics = st;
        instance = mem_ptr_ok(inst) ? inst : 0;
        return true;
    }
    return false;
}

static bool verify_klass(pid_t pid, uintptr_t klass, const char *name,
                         uintptr_t instOff, uintptr_t &statics, uintptr_t &instance) {
    uintptr_t namePtr = 0;
    if (!mem_read(pid, klass + Off::KlassName, namePtr) || !mem_ptr_ok(namePtr))
        return false;
    if (!name_at(pid, namePtr, name)) return false;
    return statics_for(pid, klass, instOff, statics, instance);
}

static int score_bm(pid_t pid, uintptr_t bm) {
    if (!bm || !mem_ptr_ok(bm)) return -1;
    uintptr_t local = 0;
    mem_read(pid, bm + Off::LocalPlayerShow, local);
    Vec3 self{};
    bool haveSelf = local && mem_read(pid, local + Off::LastPosition, self) && pos_ok(self);
    uintptr_t pDict = 0, mDict = 0;
    mem_read(pid, bm + Off::DicPlayerShow, pDict);
    mem_read(pid, bm + Off::DicMonsterShow, mDict);
    std::vector<std::pair<int, uintptr_t>> players, monsters;
    dict_entries(pid, pDict, players);
    dict_entries(pid, mDict, monsters);
    int usable = 0;
    auto take = [&](uintptr_t v, bool mob, int key) {
        if (!v || v == local) return;
        uint8_t dead = 0, camp = 0;
        if (mem_read(pid, v + Off::EntityDeath, dead) && dead) return;
        if (mem_read(pid, v + Off::SameCamp, camp) && camp) return;
        int id = key;
        mem_read(pid, v + Off::EntityId, id);
        if (mob && !monster_id(id)) return;
        Vec3 pos{};
        if (!mem_read(pid, v + Off::LastPosition, pos) || !pos_ok(pos)) return;
        usable++;
    };
    for (const auto &kv : players) take(kv.second, false, kv.first);
    for (const auto &kv : monsters) take(kv.second, true, kv.first);
    if (usable > 0) return 1000 + usable + (haveSelf ? 50 : 0);
    if (haveSelf) return 10;
    if (mem_ptr_ok(local) || mem_ptr_ok(pDict)) return 1;
    return 0;
}

static bool elf_bm_klass_in(pid_t pid, uintptr_t base, uintptr_t soEnd, uintptr_t &klass) {
    klass = 0;
    if (!base) return false;

    Elf64_Ehdr eh{};
    if (!mem_read(pid, base, eh) || eh.e_ident[0] != 0x7f) return false;
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;

    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (!mem_read(pid, base + eh.e_shoff, sh.data(), sh.size() * sizeof(Elf64_Shdr)))
        return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;
    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 256 * 1024) return false;
    std::vector<char> shstr(strsh.sh_size);
    if (!mem_read(pid, base + strsh.sh_offset, shstr.data(), shstr.size())) return false;

    const Elf64_Shdr *dynsym = nullptr, *dynstr = nullptr;
    for (const auto &s : sh) {
        if (s.sh_name >= shstr.size()) continue;
        const char *nm = shstr.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) dynstr = &s;
    }
    if (!dynsym || !dynstr || dynsym->sh_entsize != sizeof(Elf64_Sym)) return false;

    std::vector<char> strtab(dynstr->sh_size);
    if (!mem_read(pid, base + dynstr->sh_offset, strtab.data(), strtab.size())) return false;
    const size_t nsym = dynsym->sh_size / sizeof(Elf64_Sym);
    std::vector<Elf64_Sym> syms(nsym);
    if (!mem_read(pid, base + dynsym->sh_offset, syms.data(), dynsym->sh_size)) return false;

    const char *names[] = {"BattleManager_TypeInfo", "BattleManager_c"};
    for (const auto &s : syms) {
        if (s.st_name >= strtab.size() || !s.st_value) continue;
        const char *nm = strtab.data() + s.st_name;
        bool hit = false;
        for (const char *w : names)
            if (std::strcmp(nm, w) == 0) { hit = true; break; }
        if (!hit) continue;
        uintptr_t addr = base + s.st_value;
        if (addr < base || addr >= soEnd) continue;
        uintptr_t k = 0;
        if (mem_read(pid, addr, k) && mem_ptr_ok(k)) {
            klass = k;
            return true;
        }
        if (mem_ptr_ok(addr)) {
            klass = addr;
            return true;
        }
    }
    return false;
}

static bool elf_bm_klass(pid_t pid, const std::vector<MapsReg> &maps, uintptr_t &klass) {
    klass = 0;
    struct Cand {
        uintptr_t base = 0, end = 0;
        int prio = 0;
    };
    std::vector<Cand> cands;
    for (const auto &m : maps) {
        if (m.fileOff != 0 || m.perms[0] != 'r') continue;
        if (m.path.find(".so") == std::string::npos) continue;
        const std::string &p = m.path;
        int prio = 0;
        if (p.find("liblogic") != std::string::npos) prio = 100;
        else if (p.find("csharp") != std::string::npos) prio = 90;
        else if (p.find("il2cpp") != std::string::npos) prio = 85;
        else if (p.find("GameAssembly") != std::string::npos) prio = 80;
        else continue;
        bool merged = false;
        for (auto &c : cands) {
            if (c.base == m.start) {
                c.end = std::max(c.end, m.end);
                merged = true;
                break;
            }
        }
        if (!merged) cands.push_back({m.start, m.end, prio});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b) { return a.prio > b.prio; });
    for (const Cand &c : cands) {
        if (elf_bm_klass_in(pid, c.base, c.end, klass) && klass) return true;
    }
    return false;
}

static bool find_bm(pid_t pid, const std::vector<MapsReg> &maps,
                    uintptr_t &klass, uintptr_t &statics, uintptr_t &instance,
                    BmFindMeta *meta) {
    klass = statics = instance = 0;
    const char *name = "BattleManager";
    if (meta) {
        meta->nameStrings = 0;
        meta->usedElf = false;
    }

    if (elf_bm_klass(pid, maps, klass)) {
        if (meta) meta->usedElf = true;
        uintptr_t st = 0, inst = 0;
        if (verify_klass(pid, klass, name, Off::InstanceStatic, st, inst)) {
            statics = st;
            instance = inst;
            if (klass) return true;
        }
        klass = 0;
    }

    std::vector<uintptr_t> strs;
    scan_strings(pid, maps, name, strs);
    if (meta) meta->nameStrings = (int)strs.size();
    if (strs.empty()) return false;

    int bestScore = -1;
    static const uintptr_t kNameOffs[] = {0x10, 0x18, 0x08, 0x20};
    for (uintptr_t s : strs) {
        for (uintptr_t nameOff : kNameOffs) {
            const uintptr_t cand = s - nameOff;
            uintptr_t st = 0, inst = 0;
            if (!verify_klass(pid, cand, name, Off::InstanceStatic, st, inst)) continue;
            int sc = inst ? score_bm(pid, inst) : (st ? 2 : 0);
            if (sc > bestScore) {
                bestScore = sc;
                klass = cand;
                statics = st;
                instance = inst;
            }
            if (bestScore >= 1000) return true;
        }
    }
    if (klass) return true;

    std::vector<const MapsReg *> order;
    for (const auto &m : maps)
        if (klass_region_prio(m) > 0) order.push_back(&m);
    std::sort(order.begin(), order.end(), [](const MapsReg *a, const MapsReg *b) {
        return klass_region_prio(*a) > klass_region_prio(*b);
    });

    size_t scanned = 0;
    for (const MapsReg *mp : order) {
        const MapsReg &m = *mp;
        const size_t sz = (size_t)(m.end - m.start);
        const size_t scan = std::min(sz, (size_t)12 * 1024 * 1024);
        if ((scanned += scan) > 128 * 1024 * 1024) break;
        const size_t chunk = 256 * 1024;
        for (size_t off = 0; off < scan; off += chunk) {
            const size_t n = std::min(chunk, scan - off);
            std::vector<uint8_t> buf(n);
            if (!mem_read(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t p = 0;
                std::memcpy(&p, buf.data() + i, 8);
                bool hit = false;
                for (uintptr_t s : strs)
                    if (p == s) { hit = true; break; }
                if (!hit) continue;
                const uintptr_t cand = m.start + off + i - Off::KlassName;
                uintptr_t st = 0, inst = 0;
                if (!verify_klass(pid, cand, name, Off::InstanceStatic, st, inst)) continue;
                int sc = inst ? score_bm(pid, inst) : (st ? 2 : 0);
                if (sc > bestScore) {
                    bestScore = sc;
                    klass = cand;
                    statics = st;
                    instance = inst;
                }
                if (bestScore >= 1000) return klass != 0;
            }
        }
    }
    return klass != 0;
}

static void refresh_instance(pid_t pid) {
    if (!g_bmStatics) return;
    uintptr_t inst = 0;
    if (mem_read(pid, g_bmStatics + Off::InstanceStatic, inst) && mem_ptr_ok(inst))
        g_bmInstance = inst;
}

} // namespace

void esp_reset() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = 0;
    g_bmKlass = g_bmStatics = g_bmInstance = 0;
    g_resolved = false;
}

EspStatus esp_attach(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = pid;
    g_bmKlass = g_bmStatics = g_bmInstance = 0;
    g_resolved = false;
    if (pid <= 0) return EspStatus::NoProcess;
    return EspStatus::Idle;
}

static void fill_report(EspResolveReport &r, EspStatus status, int mapsCount, bool probe,
                        const BmFindMeta &meta, uintptr_t klass, uintptr_t statics,
                        uintptr_t inst) {
    r.status = status;
    r.mapsCount = mapsCount;
    r.readProbe = probe;
    r.bmNameStrings = meta.nameStrings;
    r.usedElfTypeInfo = meta.usedElf;
    r.klass = klass;
    r.statics = statics;
    r.instance = inst;
    std::snprintf(r.detail, sizeof(r.detail),
                  "maps=%d probe=%s strings=%d elf=%s klass=0x%llx statics=0x%llx inst=0x%llx",
                  mapsCount, probe ? "OK" : "FAIL", meta.nameStrings,
                  meta.usedElf ? "yes" : "no",
                  (unsigned long long)klass, (unsigned long long)statics,
                  (unsigned long long)inst);
}

static void merge_il2cpp_into_report(EspResolveReport &rep, const RemoteIl2CppInfo &il2) {
    std::snprintf(rep.il2cppLib, sizeof(rep.il2cppLib), "%s",
                  il2.libBaseName[0] ? il2.libBaseName : "(none)");
    rep.il2cppBase = il2.base;
    rep.il2cppDomainGet = il2.sym_domain_get;
    rep.il2cppDomainPtr = il2.domain_ptr;
    std::snprintf(rep.il2cppNote, sizeof(rep.il2cppNote), "%s", il2.note);
}

EspStatus esp_resolve(EspResolveReport *reportOut) {
    std::lock_guard<std::mutex> lock(g_mu);
    EspResolveReport rep{};
    if (g_pid <= 0) {
        rep.status = EspStatus::NoProcess;
        std::snprintf(rep.detail, sizeof(rep.detail), "no pid attached");
        g_lastReport = rep;
        if (reportOut) *reportOut = rep;
        return rep.status;
    }
    std::vector<MapsReg> maps;
    if (!mem_maps(g_pid, maps)) {
        rep.status = EspStatus::NoRead;
        rep.mapsCount = 0;
        rep.readProbe = false;
        std::snprintf(rep.detail, sizeof(rep.detail),
                      "cannot read /proc/%d/maps (root? process dead?)", (int)g_pid);
        g_lastReport = rep;
        if (reportOut) *reportOut = rep;
        return rep.status;
    }
    const bool probe = proc_probe_read(g_pid);
    RemoteIl2CppInfo il2{};
    remote_detect_il2cpp(g_pid, il2);
    merge_il2cpp_into_report(rep, il2);

    BmFindMeta meta{};
    uintptr_t k = 0, st = 0, inst = 0;
    if (!find_bm(g_pid, maps, k, st, inst, &meta)) {
        g_resolved = false;
        fill_report(rep, EspStatus::NoBattleManager, (int)maps.size(), probe, meta, k, st, inst);
        g_lastReport = rep;
        if (reportOut) *reportOut = rep;
        return rep.status;
    }
    g_bmKlass = k;
    g_bmStatics = st;
    g_bmInstance = inst;
    g_resolved = true;
    const EspStatus stOut = inst ? EspStatus::Ready : EspStatus::NoInstance;
    fill_report(rep, stOut, (int)maps.size(), probe, meta, k, st, inst);
    g_lastReport = rep;
    if (reportOut) *reportOut = rep;
    return stOut;
}

const EspResolveReport &esp_last_resolve_report() {
    return g_lastReport;
}

EspStatus esp_tick(EspFrame &out) {
    std::lock_guard<std::mutex> lock(g_mu);
    out = EspFrame{};
    out.pid = g_pid;
    if (g_pid <= 0) {
        out.status = EspStatus::NoProcess;
        std::snprintf(out.hint, sizeof(out.hint), "No MLBB pid");
        return out.status;
    }
    if (!g_resolved) {
        out.status = EspStatus::NoBattleManager;
        std::snprintf(out.hint, sizeof(out.hint), "Tap Resolve (in match)");
        return out.status;
    }

    refresh_instance(g_pid);
    const uintptr_t bm = g_bmInstance;
    if (!bm) {
        out.status = EspStatus::NoInstance;
        std::snprintf(out.hint, sizeof(out.hint), "BattleManager null — enter match");
        return out.status;
    }

    uintptr_t local = 0;
    mem_read(g_pid, bm + Off::LocalPlayerShow, local);
    Vec3 self{};
    if (local) mem_read(g_pid, local + Off::LastPosition, self);
    out.selfX = self.x;
    out.selfZ = self.z;

    uintptr_t pDict = 0, mDict = 0;
    mem_read(g_pid, bm + Off::DicPlayerShow, pDict);
    mem_read(g_pid, bm + Off::DicMonsterShow, mDict);
    std::vector<std::pair<int, uintptr_t>> players, monsters;
    dict_entries(g_pid, pDict, players);
    dict_entries(g_pid, mDict, monsters);

    auto push = [&](uintptr_t v, int kind, const char *labelOverride, int key) {
        if (!v) return;
        uint8_t dead = 0, camp = 0;
        if (mem_read(g_pid, v + Off::EntityDeath, dead) && dead) return;
        if (mem_read(g_pid, v + Off::SameCamp, camp) && camp) return;
        int id = key;
        mem_read(g_pid, v + Off::EntityId, id);
        if (kind == 1 && !monster_id(id)) return;
        Vec3 pos{};
        if (!mem_read(g_pid, v + Off::LastPosition, pos) || !pos_ok(pos)) return;
        EspTarget t{};
        t.worldX = pos.x;
        t.worldZ = pos.z;
        t.relX = pos.x - self.x;
        t.relZ = pos.z - self.z;
        t.meters = (int)std::sqrt(t.relX * t.relX + t.relZ * t.relZ);
        mem_read(g_pid, v + Off::Hp, t.hp);
        mem_read(g_pid, v + Off::HpMax, t.hpMax);
        t.kind = kind;
        if (labelOverride)
            std::snprintf(t.label, sizeof(t.label), "%s", labelOverride);
        else if (kind == 1)
            std::snprintf(t.label, sizeof(t.label), "%s", Off::monsterLabel(id));
        else
            std::snprintf(t.label, sizeof(t.label), "P%d", id);
        out.targets.push_back(t);
    };

    for (const auto &kv : players) push(kv.second, 0, nullptr, kv.first);
    for (const auto &kv : monsters) {
        int id = kv.first;
        if (kv.second) mem_read(g_pid, kv.second + Off::EntityId, id);
        if (!monster_id(id)) continue;
        push(kv.second, 1, Off::monsterLabel(id), kv.first);
    }

    out.mapsCount = 0;
    out.readProbe = proc_probe_read(g_pid);
    out.bmKlass = g_bmKlass;
    out.bmStatics = g_bmStatics;
    out.bmInstance = bm;
    out.playerDict = pDict;
    out.monsterDict = mDict;
    out.dictPlayers = (int)players.size();
    out.dictMonsters = (int)monsters.size();

    out.status = EspStatus::Ready;
    std::snprintf(out.hint, sizeof(out.hint), "OK  %zu targets  dict P%d M%d",
                  out.targets.size(), out.dictPlayers, out.dictMonsters);
    return out.status;
}
