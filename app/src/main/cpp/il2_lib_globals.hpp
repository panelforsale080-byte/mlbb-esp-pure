#pragma once
#include "conrent_esp.hpp"
#include <cstdint>
#include <sys/types.h>

/** Discover global BM/MainCamera slots; store slot_va + computed rva for ESP (hints only in search). */
void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve);
