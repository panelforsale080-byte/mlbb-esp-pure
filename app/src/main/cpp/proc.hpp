#pragma once
#include <sys/types.h>
#include <cstddef>

struct ProcCandidate {
    pid_t pid = 0;
    char cmdline[280] = {};
};

pid_t proc_find_unity_kills();
pid_t proc_find_mlbb_fallback();
bool proc_alive(pid_t pid);
bool proc_probe_read(pid_t pid);
size_t proc_list_mlbb(ProcCandidate *out, size_t maxOut);
void proc_format_cmdline(pid_t pid, char *out, size_t outCap);
