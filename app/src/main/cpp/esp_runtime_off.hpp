#pragma once
#include "conrent_esp.hpp"
#include <cstdint>

/** Live IL2CPP field offsets from Resolve (conRentV1 takeField equivalent). */
struct EspRuntimeOff {
    bool ready = false;
    uintptr_t localPlayerShow = 0;
    uintptr_t dicPlayerShow = 0;
    uintptr_t dicMonsterShow = 0;
    uintptr_t listPlayerShow = 0;
    uintptr_t listMonsterShow = 0;
    uintptr_t lastPosition = 0;
    uintptr_t cachePosition = 0;
    uintptr_t vCachePosition = 0;
    uintptr_t entityId = 0;
    uintptr_t monsterConfig = 0;
    uintptr_t monsterTableId = 0;
    uintptr_t monsterName = 0;
    uintptr_t monsterPrefab = 0;
    uintptr_t roleName = 0;
    uintptr_t wildType = 0;
    uintptr_t entityDeath = 0;
    uintptr_t sameCamp = 0;
    uintptr_t hp = 0;
    uintptr_t hpMax = 0;
    uintptr_t level = 0;
    uintptr_t showKillWildTimes = 0;
    uintptr_t showCooldownComp = 0;
    uintptr_t heroName = 0;
    uintptr_t summonSkillId = 0;
    uintptr_t showCdDic = 0;
    uintptr_t cddSpellId = 0;
    uintptr_t cddCoolTime = 0;
    uintptr_t cddStartTime = 0;
    uintptr_t heroOriginId = 0;
    uintptr_t bmStaticRunBullets = 0;
    uintptr_t bmStaticRunBulletsList = 0;
    uintptr_t bulletId = 0;
    uintptr_t bulletPos = 0;
    uintptr_t bulletForward = 0;
};

void esp_runtime_off_apply(const ConrentResolve &cr);
void esp_runtime_off_clear();
const EspRuntimeOff &esp_runtime_off();
bool esp_runtime_off_ready();
