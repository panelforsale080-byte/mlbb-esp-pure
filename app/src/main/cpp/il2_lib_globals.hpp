#pragma once
#include "conrent_esp.hpp"
#include <cstdint>
#include <sys/types.h>

/** Probe fixed EXTERNAL RVAs (0x635b290 / 0x635b620) on each game .so base; log + pick for ESP. */
void il2_discover_lib_global_rvas(pid_t pid, uintptr_t il2LibBase, ConrentResolve &resolve);
