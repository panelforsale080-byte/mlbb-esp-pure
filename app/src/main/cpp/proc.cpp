#include "proc.hpp"
#include "mem.hpp"
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static bool cmdline_has(pid_t pid, const char *needle) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline");
    if (!f) return false;
    std::string all;
    std::getline(f, all, '\0');
    while (f) {
        std::string part;
        std::getline(f, part, '\0');
        if (!part.empty()) all += part;
    }
    return all.find(needle) != std::string::npos;
}

pid_t proc_find_unity_kills() {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    pid_t best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const pid_t pid = (pid_t)std::atoi(e->d_name);
        if (pid <= 0) continue;
        if (cmdline_has(pid, "com.mobile.legends") && cmdline_has(pid, "UnityKillsMe"))
            best = pid;
    }
    closedir(d);
    return best;
}

bool proc_alive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0;
}

bool proc_probe_read(pid_t pid) {
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return false;
    for (const auto &r : maps) {
        if (r.perms[0] != 'r' || r.end <= r.start) continue;
        uint8_t scratch[8] = {};
        if (mem_read(pid, r.start, scratch, sizeof(scratch)))
            return true;
    }
    return false;
}
