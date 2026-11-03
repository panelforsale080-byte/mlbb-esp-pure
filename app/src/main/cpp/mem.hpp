#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct MapsReg {
    uintptr_t start = 0, end = 0, fileOff = 0;
    char perms[8] = {};
    std::string path;
};

bool mem_read(pid_t pid, uintptr_t addr, void *buf, size_t n);

template <typename T>
inline bool mem_read(pid_t pid, uintptr_t addr, T &out) {
    return mem_read(pid, addr, &out, sizeof(T));
}

bool mem_maps(pid_t pid, std::vector<MapsReg> &out);
bool mem_ptr_ok(uintptr_t p);
