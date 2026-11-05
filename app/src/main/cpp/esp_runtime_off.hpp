#pragma once
#include "conrent_esp.hpp"
#include "offsets.hpp"

/** Live IL2CPP field offsets (conRent takeField); fallback to Off:: when zero. */
struct EspRuntimeOff {
    bool from_il2cpp = false;
    uintptr_t localPlayerShow = Off::LocalPlayerShow;
    uintptr_t dicPlayerShow = Off::DicPlayerShow;
    uintptr_t dicMonsterShow = Off::DicMonsterShow;
    uintptr_t listPlayerShow = Off::ListPlayerShow;
    uintptr_t listMonsterShow = Off::ListMonsterShow;
    uintptr_t lastPosition = Off::LastPosition;
    uintptr_t cachePosition = Off::CachePosition;
    uintptr_t entityId = Off::EntityId;
    uintptr_t entityDeath = Off::EntityDeath;
    uintptr_t sameCamp = Off::SameCamp;
    uintptr_t hp = Off::Hp;
    uintptr_t hpMax = Off::HpMax;
};

void esp_runtime_off_apply(const ConrentResolve &cr);
const EspRuntimeOff &esp_runtime_off();
