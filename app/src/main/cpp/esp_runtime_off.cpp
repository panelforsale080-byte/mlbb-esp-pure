#include "esp_runtime_off.hpp"
#include "ui_log.hpp"

static EspRuntimeOff g_off{};

void esp_runtime_off_clear() { g_off = EspRuntimeOff{}; }

void esp_runtime_off_apply(const ConrentResolve &cr) {
    g_off = EspRuntimeOff{};
    const ConrentRuntimeOffsets &rt = cr.runtime_fields;
    g_off.localPlayerShow = rt.bm_m_LocalPlayerShow;
    g_off.dicPlayerShow = rt.bm_m_dicPlayerShow;
    g_off.dicMonsterShow = rt.bm_m_dicMonsterShow;
    g_off.listPlayerShow = rt.bm_m_ShowPlayerList;
    g_off.listMonsterShow = rt.bm_m_ShowMonsterList;
    g_off.lastPosition = rt.se_LastPosition;
    g_off.entityId = rt.se_m_ID;
    g_off.entityDeath = rt.se_m_bDeath;
    g_off.sameCamp = rt.se_m_bSameCampType;
    g_off.hp = rt.se_m_Hp;
    g_off.hpMax = rt.se_m_HpMax;
    g_off.ready = cr.runtime_ready && g_off.localPlayerShow && g_off.dicPlayerShow &&
                  g_off.dicMonsterShow && g_off.lastPosition && g_off.entityDeath &&
                  g_off.sameCamp && g_off.entityId;
    ui_log("[resolve] runtime offsets (IL2CPP, no dump.cs):");
    ui_log("  BM local=0x%llx dictP=0x%llx dictM=0x%llx",
           (unsigned long long)g_off.localPlayerShow, (unsigned long long)g_off.dicPlayerShow,
           (unsigned long long)g_off.dicMonsterShow);
    ui_log("  SE pos=0x%llx id=0x%llx death=0x%llx camp=0x%llx hp=0x%llx",
           (unsigned long long)g_off.lastPosition, (unsigned long long)g_off.entityId,
           (unsigned long long)g_off.entityDeath, (unsigned long long)g_off.sameCamp,
           (unsigned long long)g_off.hp);
    if (!g_off.ready) ui_log("[resolve] WARN: incomplete runtime field offsets");
}

const EspRuntimeOff &esp_runtime_off() { return g_off; }

bool esp_runtime_off_ready() { return g_off.ready; }
