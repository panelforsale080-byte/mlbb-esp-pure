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
    uintptr_t entityId = 0;
    uintptr_t entityDeath = 0;
    uintptr_t sameCamp = 0;
    uintptr_t hp = 0;
    uintptr_t hpMax = 0;
};

void esp_runtime_off_apply(const ConrentResolve &cr);
void esp_runtime_off_clear();
const EspRuntimeOff &esp_runtime_off();
bool esp_runtime_off_ready();
