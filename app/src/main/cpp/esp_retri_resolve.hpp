#pragma once

/**
 * Auto Retri IL2CPP classes — resolved up front during conRent Resolve so
 * find_class_fast hits cache (no slow image scan for ShowEntity / LogicPlayer).
 */
inline constexpr const char *kEspAutoRetriClasses[] = {
    "BattleManager",
    "ShowEntity",
    "ShowPlayer",
    "ShowSelfPlayer",
    "Battle.LogicPlayer",
    "LogicPlayer",
    "Battle.LogicFighter",
};

inline constexpr unsigned kEspAutoRetriClassCount =
        sizeof(kEspAutoRetriClasses) / sizeof(kEspAutoRetriClasses[0]);
