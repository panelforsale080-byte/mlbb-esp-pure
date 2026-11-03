#include "remote_elf.hpp"
#include "mem.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <vector>

namespace {

struct SymTables {
    const Elf64_Shdr *dynsym = nullptr;
    const Elf64_Shdr *dynstr = nullptr;
    const Elf64_Shdr *symtab = nullptr;
    const Elf64_Shdr *strtab = nullptr;
};

static bool read_ehdr(pid_t pid, uintptr_t base, Elf64_Ehdr &eh) {
    return mem_read(pid, base, eh) && eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E';
}

static bool read_ehdr_file(FILE *f, Elf64_Ehdr &eh) {
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    return std::fread(&eh, 1, sizeof(eh), f) == sizeof(eh) && eh.e_ident[0] == 0x7f;
}

static bool calc_bias_from_phdr(pid_t pid, uintptr_t base, const Elf64_Ehdr &eh, uintptr_t &bias) {
    if (eh.e_phnum == 0 || eh.e_phnum > 128 || !eh.e_phoff) return false;
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (!mem_read(pid, base + eh.e_phoff, ph.data(), ph.size() * sizeof(Elf64_Phdr)))
        return false;
    uint64_t min_v = UINT64_MAX;
    for (const auto &p : ph) {
        if (p.p_type != PT_LOAD) continue;
        min_v = std::min(min_v, (uint64_t)p.p_vaddr);
    }
    if (min_v == UINT64_MAX) return false;
    bias = base - (uintptr_t)min_v;
    return true;
}

static bool calc_bias_from_phdr_file(FILE *f, const Elf64_Ehdr &eh, uintptr_t map_base,
                                     uintptr_t &bias) {
    if (eh.e_phnum == 0 || eh.e_phnum > 128 || !eh.e_phoff) return false;
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (std::fseek(f, (long)eh.e_phoff, SEEK_SET) != 0) return false;
    if (std::fread(ph.data(), sizeof(Elf64_Phdr), ph.size(), f) != ph.size()) return false;
    uint64_t min_v = UINT64_MAX;
    for (const auto &p : ph) {
        if (p.p_type != PT_LOAD) continue;
        min_v = std::min(min_v, (uint64_t)p.p_vaddr);
    }
    if (min_v == UINT64_MAX) return false;
    bias = map_base - (uintptr_t)min_v;
    return true;
}

static bool lookup_in_tables(const char *sym_name, const char *str_data, size_t str_size,
                             const Elf64_Sym *syms, size_t sym_count, uintptr_t load_bias,
                             uintptr_t &addr_out) {
    for (size_t i = 0; i < sym_count; ++i) {
        const Elf64_Sym &s = syms[i];
        if (s.st_name >= str_size || !s.st_value) continue;
        if (ELF64_ST_TYPE(s.st_info) != STT_FUNC && ELF64_ST_TYPE(s.st_info) != STT_OBJECT)
            continue;
        if (std::strcmp(str_data + s.st_name, sym_name) != 0) continue;
        addr_out = load_bias + (uintptr_t)s.st_value;
        return true;
    }
    return false;
}

static bool sym_from_sections_remote(pid_t pid, uintptr_t base, const Elf64_Ehdr &eh,
                                     uintptr_t load_bias, const char *sym, uintptr_t &addr) {
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (!mem_read(pid, base + eh.e_shoff, sh.data(), sh.size() * sizeof(Elf64_Shdr)))
        return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;

    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 512 * 1024) return false;
    std::vector<char> shstr(strsh.sh_size);
    if (!mem_read(pid, base + strsh.sh_offset, shstr.data(), shstr.size())) return false;

    SymTables t{};
    for (const auto &s : sh) {
        if (s.sh_name >= shstr.size()) continue;
        const char *nm = shstr.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) t.dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) t.dynstr = &s;
        if (std::strcmp(nm, ".symtab") == 0) t.symtab = &s;
        if (std::strcmp(nm, ".strtab") == 0) t.strtab = &s;
    }

    auto try_table = [&](const Elf64_Shdr *symsh, const Elf64_Shdr *strshp) -> bool {
        if (!symsh || !strshp || symsh->sh_entsize != sizeof(Elf64_Sym)) return false;
        if (strshp->sh_size == 0 || strshp->sh_size > 16 * 1024 * 1024) return false;
        if (symsh->sh_size == 0 || symsh->sh_size > 16 * 1024 * 1024) return false;
        std::vector<char> strtab(strshp->sh_size);
        if (!mem_read(pid, base + strshp->sh_offset, strtab.data(), strtab.size())) return false;
        const size_t n = symsh->sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> syms(n);
        if (!mem_read(pid, base + symsh->sh_offset, syms.data(), symsh->sh_size)) return false;
        return lookup_in_tables(sym, strtab.data(), strtab.size(), syms.data(), n, load_bias,
                                addr);
    };

    if (try_table(t.dynsym, t.dynstr)) return true;
    if (try_table(t.symtab, t.strtab)) return true;
    return false;
}

static bool sym_from_sections_file(FILE *f, const Elf64_Ehdr &eh, uintptr_t load_bias,
                                   const char *sym, uintptr_t &addr) {
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (std::fseek(f, (long)eh.e_shoff, SEEK_SET) != 0) return false;
    if (std::fread(sh.data(), sizeof(Elf64_Shdr), sh.size(), f) != sh.size()) return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;

    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 512 * 1024) return false;
    std::vector<char> shstr(strsh.sh_size);
    if (std::fseek(f, (long)strsh.sh_offset, SEEK_SET) != 0) return false;
    if (std::fread(shstr.data(), 1, shstr.size(), f) != shstr.size()) return false;

    SymTables t{};
    for (const auto &s : sh) {
        if (s.sh_name >= shstr.size()) continue;
        const char *nm = shstr.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) t.dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) t.dynstr = &s;
        if (std::strcmp(nm, ".symtab") == 0) t.symtab = &s;
        if (std::strcmp(nm, ".strtab") == 0) t.strtab = &s;
    }

    auto try_table = [&](const Elf64_Shdr *symsh, const Elf64_Shdr *strshp) -> bool {
        if (!symsh || !strshp || symsh->sh_entsize != sizeof(Elf64_Sym)) return false;
        std::vector<char> strtab(strshp->sh_size);
        if (std::fseek(f, (long)strshp->sh_offset, SEEK_SET) != 0) return false;
        if (std::fread(strtab.data(), 1, strtab.size(), f) != strtab.size()) return false;
        const size_t n = symsh->sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> syms(n);
        if (std::fseek(f, (long)symsh->sh_offset, SEEK_SET) != 0) return false;
        if (std::fread(syms.data(), sizeof(Elf64_Sym), n, f) != n) return false;
        return lookup_in_tables(sym, strtab.data(), strtab.size(), syms.data(), n, load_bias,
                                addr);
    };

    if (try_table(t.dynsym, t.dynstr)) return true;
    if (try_table(t.symtab, t.strtab)) return true;
    return false;
}

} // namespace

bool elf_remote_bias(pid_t pid, uintptr_t map_base, ElfRemote &out) {
    out = {};
    out.base = map_base;
    Elf64_Ehdr eh{};
    if (!read_ehdr(pid, map_base, eh)) return false;
    return calc_bias_from_phdr(pid, map_base, eh, out.load_bias);
}

bool elf_remote_sym(pid_t pid, uintptr_t map_base, const char *name, uintptr_t &addr_out) {
    addr_out = 0;
    Elf64_Ehdr eh{};
    if (!read_ehdr(pid, map_base, eh)) return false;
    uintptr_t bias = 0;
    if (!calc_bias_from_phdr(pid, map_base, eh, bias)) return false;
    return sym_from_sections_remote(pid, map_base, eh, bias, name, addr_out);
}

bool elf_file_sym(const char *path, uintptr_t load_bias, const char *name, uintptr_t &addr_out) {
    addr_out = 0;
    if (!path || !path[0]) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    Elf64_Ehdr eh{};
    if (!read_ehdr_file(f, eh)) {
        std::fclose(f);
        return false;
    }
    const bool ok = sym_from_sections_file(f, eh, load_bias, name, addr_out);
    std::fclose(f);
    return ok;
}
