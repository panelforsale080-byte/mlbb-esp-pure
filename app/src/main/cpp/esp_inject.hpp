#pragma once
#include <sys/types.h>

void esp_inject_set_scratch_dir(const char *appWritableDir);

/* One-shot ptrace dlopen into game (ESP W2S agent only — not used during draw). */
bool esp_inject_dlopen_so(pid_t pid, const char *soPath, char *errOut, size_t errCap);
