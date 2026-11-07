#pragma once
#include <cstdint>
#include <sys/types.h>

/** External IL2 cast: ShowSelfPlayer.TryUseSkill(20020) via il2cpp_runtime_invoke + ptrace. */
void esp_retri_il2_bind(uintptr_t runtime_invoke, uintptr_t try_use_skill_mi);
bool esp_retri_il2_ready();
bool esp_retri_il2_cast_summon(pid_t pid, uintptr_t showSelfPlayer, float dirX, float dirY,
                               float dirZ);
bool esp_il2_try_use_skill(pid_t pid, uintptr_t showSelfPlayer, int32_t skillId, float dirX,
                           float dirY, float dirZ, float posX, float posY, float posZ);
