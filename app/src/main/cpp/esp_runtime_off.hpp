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
    uintptr_t entityDeath = 0;
    uintptr_t sameCamp = 0;
    uintptr_t hp = 0;
    uintptr_t hpMax = 0;
    /** From ShowEntity only — same extra path as #213 + one field. */
    uintptr_t showCooldownComp = 0;
    /** ShowPlayer fields on player ShowEntity* (dump.cs — avoids extra FindClass). */
    static constexpr uintptr_t heroName = 0x8f0;
    static constexpr uintptr_t summonSkillId = 0x984;
    /** ShowCoolDownComp._dicCD */
    static constexpr uintptr_t showCdDic = 0x18;
    /** Battle.CoolDownData */
    static constexpr uintptr_t cddSpellId = 0x10;
    static constexpr uintptr_t cddCoolTime = 0x14;
    static constexpr uintptr_t cddStartTime = 0x1c;
};

void esp_runtime_off_apply(const ConrentResolve &cr);
void esp_runtime_off_clear();
const EspRuntimeOff &esp_runtime_off();
bool esp_runtime_off_ready();
