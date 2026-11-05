#pragma once
#include "remote_il2cpp.hpp"
#include "mem.hpp"
#include <sys/types.h>
#include <vector>

/* conRentV1 Tool/EspTab.cpp — external PID port (no inject). */
struct ConrentResolve {
    bool runtime_ready = false;
    uintptr_t battle_manager = 0; /* g_off.battleManager = staticFieldValue(BattleManager, Instance) */
    uintptr_t battle_manager_klass = 0;
    uintptr_t show_battle_camera_statics = 0; /* Il2Cpp static_fields for m_CamPos/m_CamRot */
    uintptr_t game_method_statics = 0;
    uintptr_t camera_get_main_method = 0; /* MethodInfo* */
    uintptr_t camera_w2s_method = 0;
    char method[64] = {};
    char detail[200] = {};
};

bool conrent_runtime_ready(pid_t pid, RemoteIl2CppInfo &il2);
bool conrent_resolve_battle_manager(pid_t pid, const std::vector<MapsReg> &maps, RemoteIl2CppInfo &il2,
                                    ConrentResolve &out);
