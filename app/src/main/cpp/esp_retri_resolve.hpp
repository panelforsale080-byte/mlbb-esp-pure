#pragma once

/** Single FindClass list at Resolve: mod menu + Auto Retri (one pass, fast cache). */
inline constexpr const char *kEspResolveFindClasses[] = {
        "UnityEngine.Camera",
        "BattleData",
        "BattleBridge",
        "BattleManager",
        "ShowEntity",
        "ShowPlayer",
        "ShowSelfPlayer",
        "LogicBattleManager",
        "Battle.LogicFighter",
        "Battle.LogicSkillComp",
        "Battle.CoolDownComp",
        "Battle.CoolDownData",
        "Battle.ShowOwnSkillComp",
        "ShowSkillData",
        /* Auto Retri */
        "Battle.LogicPlayer",
        /* Ling sword / Gusion mark (BattleManager.m_RunBullets HashSet) */
        "Battle.Bullet",
};

inline constexpr unsigned kEspResolveFindClassCount =
        sizeof(kEspResolveFindClasses) / sizeof(kEspResolveFindClasses[0]);
