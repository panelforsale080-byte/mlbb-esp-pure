#include "esp_runtime_off.hpp"

static EspRuntimeOff g_off{};

static uintptr_t pick(uintptr_t runtime, uintptr_t compile) {
    return runtime ? runtime : compile;
}

void esp_runtime_off_apply(const ConrentResolve &cr) {
    const ConrentRuntimeOffsets &rt = cr.runtime_fields;
    g_off.from_il2cpp = cr.runtime_ready && cr.battle_manager_klass != 0;
    g_off.localPlayerShow = pick(rt.bm_m_LocalPlayerShow, Off::LocalPlayerShow);
    g_off.dicPlayerShow = pick(rt.bm_m_dicPlayerShow, Off::DicPlayerShow);
    g_off.dicMonsterShow = pick(rt.bm_m_dicMonsterShow, Off::DicMonsterShow);
    g_off.listPlayerShow = pick(rt.bm_m_ShowPlayerList, Off::ListPlayerShow);
    g_off.listMonsterShow = pick(rt.bm_m_ShowMonsterList, Off::ListMonsterShow);
    g_off.lastPosition = pick(rt.se_LastPosition, Off::LastPosition);
    g_off.cachePosition = pick(rt.se_CachePosition, Off::CachePosition);
    g_off.entityId = pick(rt.se_m_ID, Off::EntityId);
    g_off.entityDeath = pick(rt.se_m_bDeath, Off::EntityDeath);
    g_off.sameCamp = pick(rt.se_m_bSameCampType, Off::SameCamp);
    g_off.hp = pick(rt.se_m_Hp, Off::Hp);
    g_off.hpMax = pick(rt.se_m_HpMax, Off::HpMax);
}

const EspRuntimeOff &esp_runtime_off() { return g_off; }
