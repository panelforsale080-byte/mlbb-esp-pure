#pragma once
#include "conrent_esp.hpp"
#include <cstdint>
#include <sys/types.h>

/** Discover global BM/MainCamera slots (hint/statics/narrow/rw-auto); store slot_va + computed rva. */
void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve);
