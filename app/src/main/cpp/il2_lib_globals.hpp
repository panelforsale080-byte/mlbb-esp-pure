#pragma once
#include "conrent_esp.hpp"
#include <cstdint>
#include <sys/types.h>

/** Scan libil2cpp/libcsharp for global TypeInfo / MainCamera slots (replaces hardcoded RVAs). */
void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve);
