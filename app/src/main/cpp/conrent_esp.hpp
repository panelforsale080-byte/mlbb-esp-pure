#pragma once
#include "remote_il2cpp.hpp"
#include "mem.hpp"
#include <sys/types.h>
#include <vector>

/* conRentV1 Tool/EspTab.cpp — external PID port (no inject). */
/** IL2CPP instance field offsets (il2cpp_field_get_offset or FieldInfo decode). */
struct ConrentRuntimeOffsets {
    uintptr_t bm_m_LocalPlayerShow = 0;
    uintptr_t bm_m_dicPlayerShow = 0;
    uintptr_t bm_m_dicMonsterShow = 0;
    uintptr_t bm_m_ShowPlayerList = 0;
    uintptr_t bm_m_ShowMonsterList = 0;
    uintptr_t se_LastPosition = 0;
    uintptr_t se_CachePosition = 0;
    uintptr_t se_m_ID = 0;
    uintptr_t se_m_bDeath = 0;
    uintptr_t se_m_bSameCampType = 0;
    uintptr_t se_m_Hp = 0;
    uintptr_t se_m_HpMax = 0;
    uintptr_t show_entity_klass = 0;
};

struct ConrentResolve {
    bool runtime_ready = false;
    ConrentRuntimeOffsets runtime_fields{};
    uintptr_t battle_manager = 0; /* g_off.battleManager = staticFieldValue(BattleManager, Instance) */
    uintptr_t battle_manager_klass = 0;
    uintptr_t battle_manager_statics = 0;
    uintptr_t show_battle_camera_statics = 0; /* Il2Cpp static_fields for m_CamPos/m_CamRot */
    uintptr_t game_method_statics = 0;
    uintptr_t camera_get_main_method = 0; /* MethodInfo* */
    uintptr_t camera_w2s_method = 0;
    uintptr_t camera_get_main_native = 0; /* methodPointer — conRent direct call */
    uintptr_t camera_w2s_native = 0;
    bool metadata_ready = false;
    char method[64] = {};
    char detail[200] = {};
};

bool conrent_runtime_ready(pid_t pid, RemoteIl2CppInfo &il2);
bool conrent_resolve_battle_manager(pid_t pid, const std::vector<MapsReg> &maps, RemoteIl2CppInfo &il2,
                                    ConrentResolve &out);
