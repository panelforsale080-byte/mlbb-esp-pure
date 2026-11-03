#pragma once
#include <sys/types.h>

/* MLBB GSpace / inject target thread */
pid_t proc_find_unity_kills();
bool proc_alive(pid_t pid);
bool proc_probe_read(pid_t pid);
