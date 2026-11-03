#include "remote_il2cpp.hpp"
#include "mem.hpp"
#include <algorithm>
#include <cstring>
#include <elf.h>
#include <string>
#include <vector>

namespace {

struct SoModule {
    std::string path;
    uintptr_t base = 0;
    uintptr_t end = 0;
};

static std::string basename(const std::string &p) {
    const auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

static bool skip_lib(const std::string &path) {
    if (path.find("/system/") != std::string::npos) return true;
    if (path.find("/apex/") != std::string::npos) return true;
    if (path.find("/vendor/") != std::string::npos) return true;
    const std::string name = basename(path);
    return name == "libdobby.so" || name == "libTool.so";
}

static void collect_modules(pid_t pid, std::vector<SoModule> &out) {
    out.clear();
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return;

    for (const auto &m : maps) {
        if (m.path.empty() || m.path[0] != '/') continue;
        if (m.path.find(".so") == std::string::npos) continue;
        if (skip_lib(m.path)) continue;
        if (m.fileOff != 0 || m.perms[0] != 'r') continue;

        bool merged = false;
        for (auto &mod : out) {
            if (mod.path == m.path) {
                mod.end = std::max(mod.end, m.end);
                merged = true;
                break;
            }
        }
        if (!merged) {
            SoModule mod{};
            mod.path = m.path;
            mod.base = m.start;
            mod.end = m.end;
            out.push_back(mod);
        }
    }

    std::sort(out.begin(), out.end(), [](const SoModule &a, const SoModule &b) {
        const std::string na = basename(a.path);
        const std::string nb = basename(b.path);
        auto score = [](const std::string &n) {
            if (n.find("il2cpp") != std::string::npos) return 100;
            if (n.find("logic") != std::string::npos) return 90;
            if (n.find("csharp") != std::string::npos) return 85;
            if (n.find("GameAssembly") != std::string::npos) return 80;
            if (n.find("unity") != std::string::npos) return 50;
            return 10;
        };
        return score(na) > score(nb);
    });
}

static bool remote_dynsym(pid_t pid, uintptr_t base, uintptr_t soEnd,
                          const char *sym, uintptr_t &addrOut) {
    addrOut = 0;
    Elf64_Ehdr eh{};
    if (!mem_read(pid, base, eh) || eh.e_ident[0] != 0x7f) return false;
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;

    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (!mem_read(pid, base + eh.e_shoff, sh.data(), sh.size() * sizeof(Elf64_Shdr)))
        return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;

    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 512 * 1024) return false;
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

    for (const auto &s : syms) {
        if (s.st_name >= strtab.size() || !s.st_value) continue;
        if (std::strcmp(strtab.data() + s.st_name, sym) != 0) continue;
        const uintptr_t addr = base + s.st_value;
        if (addr >= base && addr < soEnd) {
            addrOut = addr;
            return true;
        }
    }
    return false;
}

/* ARM64: il2cpp_domain_get often = ADRP + LDR + RET loading s_Domain */
static bool domain_from_getter(pid_t pid, uintptr_t func, uintptr_t &domainOut) {
    domainOut = 0;
    if (!func) return false;
    uint32_t ins[8] = {};
    if (!mem_read(pid, func, ins, sizeof(ins))) return false;

    for (int i = 0; i < 6; ++i) {
        const uint32_t w = ins[i];
        const uint32_t w2 = ins[i + 1];
        if ((w & 0x9F000000u) == 0x90000000u && (w2 & 0xFFC00000u) == 0xF9400000u) {
            const int rd = w & 0x1fu;
            const int rn = (w2 >> 5) & 0x1fu;
            if (rd != rn) continue;
            int64_t immhi = (int64_t)((w >> 5) & 0x7ffffu) << 12;
            int64_t immlo = (int64_t)((w >> 29) & 0x3u) << 12;
            int64_t imm = (immhi | immlo) << 12;
            if (w & (1u << 23)) imm |= 0xFFFFFFFFF0000000ULL;
            const int64_t page = ((int64_t)func & ~0xFFFLL) + imm;
            const uint32_t imm12 = (w2 >> 10) & 0xfffu;
            const uintptr_t slot = (uintptr_t)(page + (int64_t)imm12 * 8);
            uintptr_t ptr = 0;
            if (!mem_read(pid, slot, ptr) || !mem_ptr_ok(ptr)) continue;
            domainOut = ptr;
            return true;
        }
    }
    return false;
}

static bool known_il2cpp_name(const std::string &name) {
    return name.find("il2cpp") != std::string::npos || name.find("logic") != std::string::npos ||
           name.find("csharp") != std::string::npos || name.find("GameAssembly") != std::string::npos;
}

} // namespace

bool remote_detect_il2cpp(pid_t pid, RemoteIl2CppInfo &out) {
    out = RemoteIl2CppInfo{};
    std::vector<SoModule> mods;
    collect_modules(pid, mods);

    for (const SoModule &m : mods) {
        const std::string name = basename(m.path);
        const bool inApp = m.path.find("/data/app/") != std::string::npos ||
                           m.path.find("/data/data/") != std::string::npos ||
                           m.path.find("/data/user/") != std::string::npos;
        if (!inApp && !known_il2cpp_name(name)) continue;

        uintptr_t symDomain = 0, symClass = 0, symAsm = 0;
        const bool hasDomain =
            remote_dynsym(pid, m.base, m.end, "il2cpp_domain_get", symDomain);
        if (!hasDomain) continue;

        remote_dynsym(pid, m.base, m.end, "il2cpp_class_from_name", symClass);
        remote_dynsym(pid, m.base, m.end, "il2cpp_domain_get_assemblies", symAsm);

        out.export_domain_get = true;
        out.export_class_from_name = symClass != 0;
        out.export_domain_get_assemblies = symAsm != 0;
        out.base = m.base;
        out.end = m.end;
        out.sym_domain_get = symDomain;
        out.sym_class_from_name = symClass;
        out.sym_domain_get_assemblies = symAsm;
        std::snprintf(out.libPath, sizeof(out.libPath), "%s", m.path.c_str());
        std::snprintf(out.libBaseName, sizeof(out.libBaseName), "%s", name.c_str());

        if (domain_from_getter(pid, symDomain, out.domain_ptr)) {
            std::snprintf(out.note, sizeof(out.note),
                          "domain* via il2cpp_domain_get stub @ 0x%llx",
                          (unsigned long long)out.domain_ptr);
        } else {
            std::snprintf(out.note, sizeof(out.note),
                          "exports OK; domain* not decoded (need in-match VM)");
        }
        return true;
    }

    std::snprintf(out.note, sizeof(out.note),
                  "no .so with il2cpp_domain_get (open MLBB / enter match)");
    return false;
}
