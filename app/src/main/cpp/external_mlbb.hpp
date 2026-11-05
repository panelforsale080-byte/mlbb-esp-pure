#pragma once
#include <sys/types.h>
#include <cstdint>

struct ExtMat4 {
    float m[16]{};
};

/** Resolve libcsharp.so / libil2cpp.so base in target (EXTERNAL-MLBB attach). */
bool external_mlbb_attach(pid_t pid);

uintptr_t external_mlbb_libbase();

/** BattleManager instance chain (a32) from EXTERNAL-MLBB DrawMonster. */
uintptr_t external_mlbb_battle_instance(pid_t pid);

/** Load view-projection matrix for WorldToScreen (no ptrace). */
bool external_mlbb_update_matrix(pid_t pid, float screenW, float screenH, ExtMat4 &outVp);

/** EXTERNAL-MLBB WorldToScreen using loaded matrix. */
bool external_mlbb_world_to_screen(const ExtMat4 &vp, float wx, float wy, float wz, float screenW,
                                   float screenH, float &outX, float &outY);

void external_mlbb_reset();
