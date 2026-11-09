#pragma once
#include "mem.hpp"
#include "remote_il2cpp.hpp"
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
    float faceX = 0.f, faceZ = 0.f;
    bool faceValid = false;
    float faceScreenX = 0.f, faceScreenY = 0.f;
    bool faceScreenValid = false;
    bool infoValid = false;
    char info0[96] = {};
    char info1[96] = {};
    char info2[96] = {};
    char info3[96] = {};
    char label[192] = {};
    int cdSkill1 = 0, cdSkill2 = 0, cdSkill3 = 0, cdSpell = 0;
    bool cdValid = false;
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
/** Reader thread: skip expensive per-entity reads when overlay toggles off (reduces syscall load). */
void esp_tick_set_feature_flags(bool heroName, bool enemyCooldown, bool monsters, bool facing,
                                bool playerInfo);
EspStatus esp_tick(EspFrame &out);
/** conRent IL2 W2S on reader thread (not GL) — fills screen* fields. */
void esp_project_screens(EspFrame &frame, float screenW, float screenH);
const EspResolveReport &esp_last_resolve_report();
/** Full monster dict (BattleManager), not ESP-filtered targets. */
bool esp_monster_dict_entries(pid_t pid, uintptr_t bmInstance,
                              std::vector<std::pair<int, uintptr_t>> &out);
bool esp_read_entity_world(pid_t pid, uintptr_t ent, float &x, float &y, float &z);
bool esp_il2_managed_list(pid_t pid, uintptr_t listObj, std::vector<uintptr_t> &out);
uintptr_t esp_bm_static_fields_ptr(pid_t pid, uintptr_t bmKlass);
bool esp_il2_array_obj_ptrs(pid_t pid, uintptr_t arrObj, std::vector<uintptr_t> &out,
                            int maxItems = 256);
bool esp_il2_better_list_ptrs(pid_t pid, uintptr_t listObj, std::vector<uintptr_t> &out);
/** Cached IL2CPP exports after successful Resolve (for static field reads on reader thread). */
bool esp_resolved_il2(RemoteIl2CppInfo &out);
