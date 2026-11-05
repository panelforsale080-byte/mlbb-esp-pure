#pragma once
#include <cstdint>

namespace Off {

/* BattleManager (dump.cs ~684144) */
constexpr uintptr_t LocalPlayerShow = 0x48;  /* m_LocalPlayerShow ShowPlayer */
constexpr uintptr_t DicPlayerShow   = 0x60;  /* m_dicPlayerShow */
constexpr uintptr_t DicMonsterShow  = 0x68;  /* m_dicMonsterShow */
constexpr uintptr_t InstanceStatic  = 0x0;

/* ShowEntity position (dump.cs ~667840–667913, ~667866) — W2S uses visual first */
constexpr uintptr_t EntityObjPos = 0x308;    /* m_EntityObjPos Unity model anchor */
constexpr uintptr_t CachePosition = 0x28C;   /* m_vCachePosition */
constexpr uintptr_t UnityCachePosition = 0x298; /* m_vUnityCachePosition */
constexpr uintptr_t LastPosition  = 0x3F4; /* LastPosition logic/smooth */
constexpr uintptr_t ListPlayerShow  = 0x70;
constexpr uintptr_t ListMonsterShow = 0x78;
constexpr uintptr_t ListItems = 0x10;
constexpr uintptr_t ListSize  = 0x18;
constexpr uintptr_t EntityId     = 0x18C;  /* m_ID */
constexpr uintptr_t EntityDeath  = 0xC5;   /* m_bDeath */
constexpr uintptr_t SameCamp     = 0x2A9;  /* m_bSameCampType */
constexpr uintptr_t Hp           = 0x1A4;  /* m_Hp */
constexpr uintptr_t HpMax        = 0x1A8;  /* m_HpMax */

constexpr uintptr_t KlassName = 0x10;
constexpr uintptr_t KlassImage = 0x00;
constexpr uintptr_t StaticFieldOffs[] = {0xB8, 0xC0, 0xB0, 0xC8, 0xD0, 0xA8, 0xD8};

constexpr uintptr_t DictEntries = 0x18;
constexpr uintptr_t DictCount   = 0x20;
constexpr uintptr_t ArrMaxLen   = 0x18;
constexpr uintptr_t ArrData     = 0x20;
constexpr uintptr_t EntryStride = 24;
constexpr uintptr_t EntryHash   = 0x00;
constexpr uintptr_t EntryKey    = 0x08;
constexpr uintptr_t EntryValue  = 0x10;

inline const int kMonsterIds[] = {
    2002, 2003, 2004, 2005, 2006, 2008, 2009, 2011, 2012, 2013, 2056, 2059, 2072
};

inline const char *monsterLabel(int id) {
    switch (id) {
    case 2002: return "Lord";
    case 2003: return "Turtle";
    case 2004: return "Fiend";
    case 2005: return "Serpent";
    case 2006: return "Lizard";
    case 2008:
    case 2059: return "Crammer";
    case 2009: return "Rockursa";
    case 2011:
    case 2013: return "Crab";
    case 2012: return "SerpentK";
    case 2056:
    case 2072: return "Litho";
    default:   return "Mob";
    }
}

} // namespace Off
