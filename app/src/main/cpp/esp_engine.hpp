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
    uintptr_t ent = 0;
    float worldX = 0, worldY = 0, worldZ = 0;
    float relX = 0, relZ = 0;
    float screenX = 0.f, screenY = 0.f;
    bool screenValid = false;
    int meters = 0;
    int hp = 0, hpMax = 0;
    int kind = 0; /* 0 player 1 monster */
    char label[24] = {};
};

struct EspFrame {
    EspStatus status = EspStatus::Idle;
    pid_t pid = 0;
    char hint[96] = {};
    float selfX = 0, selfY = 0, selfZ = 0;
    float selfScreenX = 0.f, selfScreenY = 0.f;
    bool selfScreenValid = false;
    std::vector<EspTarget> targets;
    /* live read diagnostics (shown in UI log panel) */
    int mapsCount = 0;
    bool readProbe = false;
    uintptr_t bmKlass = 0;
    uintptr_t bmStatics = 0;
    uintptr_t bmInstance = 0;
    uintptr_t localPlayerEnt = 0;
    uintptr_t playerDict = 0;
    uintptr_t monsterDict = 0;
    int dictPlayers = 0;
    int dictMonsters = 0;
};

struct EspResolveReport {
    EspStatus status = EspStatus::Idle;
    int mapsCount = 0;
    bool readProbe = false;
    int bmNameStrings = 0;
    bool usedElfTypeInfo = false;
    uintptr_t klass = 0;
    uintptr_t statics = 0;
    uintptr_t instance = 0;
    char il2cppLib[128] = {};
    uintptr_t il2cppBase = 0;
    uintptr_t il2cppDomainGet = 0;
    uintptr_t il2cppDomainPtr = 0;
    char il2cppNote[256] = {};
    char detail[240] = {};
};

void esp_reset();
EspStatus esp_attach(pid_t pid);
EspStatus esp_resolve(EspResolveReport *reportOut = nullptr);
void esp_request_force_resolve();
bool esp_is_resolved();
EspStatus esp_tick(EspFrame &out);
/** conRent IL2 W2S on reader thread (not GL) — fills screen* fields. */
void esp_project_screens(EspFrame &frame, float screenW, float screenH);
const EspResolveReport &esp_last_resolve_report();
