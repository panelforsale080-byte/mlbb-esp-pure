#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

struct RemoteIl2CppInfo {
    char libPath[512] = {};
    char libBaseName[128] = {};
    uintptr_t base = 0;
    uintptr_t end = 0;
    uintptr_t sym_domain_get = 0;
    uintptr_t sym_class_from_name = 0;
    uintptr_t sym_domain_get_assemblies = 0;
    uintptr_t domain_ptr = 0;
    bool export_domain_get = false;
    bool export_class_from_name = false;
    bool export_domain_get_assemblies = false;
    char note[256] = {};
};

bool remote_detect_il2cpp(pid_t pid, RemoteIl2CppInfo &out);
