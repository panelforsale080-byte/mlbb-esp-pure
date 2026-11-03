#include "mem.hpp"
#include <sys/uio.h>
#include <cstdio>
#include <fstream>

bool mem_ptr_ok(uintptr_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL && ((p & 7ULL) == 0);
}

bool mem_read(pid_t pid, uintptr_t addr, void *buf, size_t n) {
    if (!pid || !addr || !buf || !n) return false;
    iovec local{buf, n};
    iovec remote{(void *)addr, n};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)n;
}

bool mem_maps(pid_t pid, std::vector<MapsReg> &out) {
    out.clear();
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps) return false;
    std::string line;
    while (std::getline(maps, line)) {
        MapsReg r{};
        char path[512] = {};
        unsigned long off = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx %7s %lx %*s %*s %511[^\n]",
                        &r.start, &r.end, r.perms, &off, path) < 4)
            continue;
        r.fileOff = off;
        r.path = path;
        while (!r.path.empty() && r.path[0] == ' ') r.path.erase(0, 1);
        out.push_back(r);
    }
    return !out.empty();
}
