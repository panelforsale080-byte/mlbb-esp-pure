#pragma once
#include "conrent_esp.hpp"
#include <cstdint>
#include <sys/types.h>

/** Discover BM/MainCamera global slots (GameMethod statics + rw-auto); store slot_va only for ESP. */
void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve);
