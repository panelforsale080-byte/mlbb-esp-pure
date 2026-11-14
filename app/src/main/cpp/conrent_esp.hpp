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
    uintptr_t se_vCachePosition = 0;
    uintptr_t se_m_ID = 0;
    uintptr_t sm_m_Config = 0;          /* ShowMonster.m_Config */
    uintptr_t monster_m_ID = 0;         /* CData_Monster_Element.m_ID */
    uintptr_t sw_m_wildType = 0;        /* ShowWildMonster.m_wildType */
    uintptr_t se_m_bDeath = 0;
    uintptr_t se_m_bSameCampType = 0;
    uintptr_t se_m_Hp = 0;
    uintptr_t se_m_HpMax = 0;
    uintptr_t se_m_Level = 0;
    uintptr_t se_vecFaceDir = 0;        /* ShowEntity._vecFaceDir */
    uintptr_t se_m_ShowCoolDownComp = 0;
    uintptr_t sp_m_HeroName = 0;
    uintptr_t sp_m_iSummonSkillId = 0;
    uintptr_t sp_m_KillWildTimes = 0;
    uintptr_t sp_m_iOriginHeroId = 0;
    uintptr_t bm_static_m_RunBullets = 0;
    uintptr_t bm_static_RunBulletsList = 0; /* BattleManager._RunBullets BetterList (VIP) */
    uintptr_t bul_m_Id = 0;
    uintptr_t bul_bulletPos = 0;
    uintptr_t bul_bulletForward = 0;
    uintptr_t scc__dicCD = 0;
    uintptr_t cdd_iSpellID = 0;
    uintptr_t cdd_uiCoolTime = 0;
    uintptr_t cdd_uiStartTime = 0;
    uintptr_t show_player_klass = 0;
    uintptr_t show_entity_klass = 0;
    uintptr_t gm_mainCamera = 0;
    uintptr_t sf_m_MainCamera = 0;
    uintptr_t sbc_m_CamPos = 0;
    uintptr_t sbc_m_CamRot = 0;
    uintptr_t sbc_m_iFieldOfView = 0;
};

struct ConrentResolve {
    bool runtime_ready = false;
    ConrentRuntimeOffsets runtime_fields{};
    uintptr_t battle_manager = 0; /* g_off.battleManager = staticFieldValue(BattleManager, Instance) */
    uintptr_t battle_manager_klass = 0;
    uintptr_t battle_manager_statics = 0;
    uintptr_t show_battle_camera_statics = 0; /* Il2Cpp static_fields for m_CamPos/m_CamRot */
    uintptr_t game_method_statics = 0;
    /** GameMethod.mainCamera SmoothFollow* from IL2CPP static_field_get_value */
    uintptr_t game_method_main_camera_obj = 0;
    uintptr_t il2_lib_base = 0;
    /** Discovered at resolve: *(lib + RVA) == BattleManager Il2CppClass* */
    uintptr_t auto_rva_battle_manager_typeinfo = 0;
    /** Discovered at resolve: external-style MainCamera chain matches GameMethod.mainCamera */
    uintptr_t auto_rva_main_camera_root = 0;
    /** Module base where auto_rva_main_camera_root applies (often libcsharp.so). */
    uintptr_t main_camera_matrix_lib_base = 0;
    /** Resolve-time absolute VA of global pointer slots (ESP reads here; no hardcoded RVA at draw). */
    uintptr_t bm_global_slot_va = 0;
    uintptr_t main_camera_global_slot_va = 0;
    /** True when discovered lib slot chain matched GameMethod.mainCamera (accurate W2S). */
    bool main_camera_slot_live_ok = false;
    uintptr_t camera_get_main_method = 0; /* MethodInfo* */
    uintptr_t camera_w2s_method = 0;
    uintptr_t camera_world_to_camera_method = 0;
    uintptr_t camera_projection_matrix_method = 0;
    uintptr_t camera_get_main_native = 0; /* methodPointer — conRent direct call */
    uintptr_t camera_w2s_native = 0;
    uintptr_t retri_try_use_skill_mi = 0; /* ShowSelfPlayer.TryUseSkill */
    uintptr_t retri_show_self_player_klass = 0;
    uintptr_t device_game_server_config_klass = 0;
    uintptr_t device_sdk_common_klass = 0;
    uintptr_t device_util_klass = 0;
    uintptr_t battle_bridge = 0;          /* BattleData.m_BattleBridge live ptr */
    uintptr_t get_bStartBattle_native = 0;
    bool metadata_ready = false;
    char method[64] = {};
    char detail[200] = {};
};

bool conrent_runtime_ready(pid_t pid, RemoteIl2CppInfo &il2);
bool conrent_resolve_battle_manager(pid_t pid, const std::vector<MapsReg> &maps, RemoteIl2CppInfo &il2,
                                    ConrentResolve &out);
/** il2cpp_field_static_get_value — correct BM _RunBullets / m_RunBullets (not raw statics+off). */
bool conrent_il2_static_field_ptr(pid_t pid, RemoteIl2CppInfo &il2, uintptr_t klass,
                                    const char *fieldName, uintptr_t &outPtr);
