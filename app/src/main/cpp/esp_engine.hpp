#pragma once
#include "mem.hpp"
#include <sys/types.h>
#include <vector>

enum class EspStatus : int {
    Idle = 0,
    NoProcess,
    NoRead,
    Resolving,
    NoBattleManager,
    NoInstance,
    Ready,
};

struct EspTarget {
    float worldX = 0, worldZ = 0;
    float relX = 0, relZ = 0;
    int meters = 0;
    int hp = 0, hpMax = 0;
    int kind = 0; /* 0 player 1 monster */
    char label[24] = {};
};

struct EspFrame {
    EspStatus status = EspStatus::Idle;
    pid_t pid = 0;
    char hint[96] = {};
    float selfX = 0, selfZ = 0;
    std::vector<EspTarget> targets;
};

void esp_reset();
EspStatus esp_attach(pid_t pid);
EspStatus esp_resolve();
EspStatus esp_tick(EspFrame &out);
