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
    g_off.cachePosition = rt.se_CachePosition;
    g_off.vCachePosition = rt.se_vCachePosition;
    g_off.entityId = rt.se_m_ID;
    g_off.monsterConfig = rt.sm_m_Config;
    g_off.monsterTableId = rt.monster_m_ID;
    g_off.wildType = rt.sw_m_wildType;
    g_off.entityDeath = rt.se_m_bDeath;
    g_off.sameCamp = rt.se_m_bSameCampType;
    g_off.hp = rt.se_m_Hp;
    g_off.hpMax = rt.se_m_HpMax;
    g_off.level = rt.se_m_Level;
    g_off.faceDir = rt.se_vecFaceDir;
    g_off.mp = rt.se_m_Mp;
    g_off.mpMax = rt.se_m_MpMax;
    g_off.coolPer = rt.se_m_CoolPer;
    g_off.moveSpeed = rt.se_m_MoveSpeed;
    g_off.atkSpeed = rt.se_m_AtkSpeed;
    g_off.grassId = rt.se_m_GrassId;
    g_off.gold = rt.sp_iGold;
    g_off.exp = rt.sp_m_Exp;
    g_off.expNeed = rt.sp_m_NowUpExp;
    g_off.kills = rt.sp_m_killNum;
    g_off.deaths = rt.sp_m_deadNum;
    g_off.assists = rt.sp_m_assistNum;
    g_off.rank = rt.sp_m_uiRankLevel;
    g_off.onHome = rt.sp_m_iOnHome;
    g_off.connected = rt.sp_m_bConnected;
    g_off.killBounty = rt.sp_m_KillBounty;
    g_off.continueKill = rt.sp_continueKill;
    g_off.systemDataKlass = rt.sd_klass;
    g_off.roomDict = rt.sd_roomPlayerInfo;
    g_off.rdName = rt.rd_name;
    g_off.rdTeam = rt.rd_team;
    g_off.rdUid = rt.rd_uid;
    g_off.rdZone = rt.rd_zone;
    g_off.rdLevel = rt.rd_level;
    g_off.rdRank = rt.rd_rank;
    g_off.rdSubRank = rt.rd_subRank;
    g_off.rdMyth = rt.rd_myth;
    g_off.rdHistRank = rt.rd_histRank;
    g_off.rdHero = rt.rd_hero;
    g_off.rdSkin = rt.rd_skin;
    g_off.rdSpell = rt.rd_spell;
    g_off.rdRune = rt.rd_rune;
    g_off.rdRuneLv = rt.rd_runeLv;
    g_off.rdBan = rt.rd_ban;
    g_off.rdRoad = rt.rd_road;
    g_off.rdPing = rt.rd_ping;
    g_off.rdRobot = rt.rd_robot;
    g_off.rdVp = rt.rd_vp;
    g_off.rdFake = rt.rd_fake;
    g_off.rdLeader = rt.rd_leader;
    g_off.rdCamp = rt.rd_camp;
    g_off.rdPos = rt.rd_pos;
    g_off.rdHeroUse = rt.rd_heroUse;
    g_off.rdStar = rt.rd_star;
    g_off.rdRoom = rt.rd_room;
    g_off.rdWant = rt.rd_want;
    g_off.rdCountry = rt.rd_country;
    g_off.showKillWildTimes = rt.sp_m_KillWildTimes;
    g_off.showCooldownComp = rt.se_m_ShowCoolDownComp;
    g_off.heroName = rt.sp_m_HeroName;
    g_off.summonSkillId = rt.sp_m_iSummonSkillId;
    g_off.showCdDic = rt.scc__dicCD;
    g_off.cddSpellId = rt.cdd_iSpellID;
    g_off.cddCoolTime = rt.cdd_uiCoolTime;
    g_off.cddStartTime = rt.cdd_uiStartTime;
    g_off.heroOriginId = rt.sp_m_iOriginHeroId;
    g_off.bmStaticRunBullets = rt.bm_static_m_RunBullets;
    g_off.bmStaticRunBulletsList = rt.bm_static_RunBulletsList;
    g_off.bulletId = rt.bul_m_Id;
    g_off.bulletPos = rt.bul_bulletPos;
    g_off.bulletForward = rt.bul_bulletForward;
    g_off.ready = cr.runtime_ready && g_off.localPlayerShow && g_off.dicPlayerShow &&
                  g_off.dicMonsterShow && g_off.lastPosition && g_off.entityDeath &&
                  g_off.sameCamp && g_off.entityId;
    ui_log("[resolve] runtime offsets (IL2CPP, no dump.cs):");
    ui_log("  BM local=0x%llx dictP=0x%llx dictM=0x%llx",
           (unsigned long long)g_off.localPlayerShow, (unsigned long long)g_off.dicPlayerShow,
           (unsigned long long)g_off.dicMonsterShow);
    ui_log("  dicM=0x%llx ShowMonster.m_Config=0x%llx CData_Monster.m_ID=0x%llx",
           (unsigned long long)g_off.dicMonsterShow, (unsigned long long)g_off.monsterConfig,
           (unsigned long long)g_off.monsterTableId);
    ui_log("  SE pos=0x%llx id=0x%llx death=0x%llx camp=0x%llx hp=0x%llx level=0x%llx",
           (unsigned long long)g_off.lastPosition, (unsigned long long)g_off.entityId,
           (unsigned long long)g_off.entityDeath, (unsigned long long)g_off.sameCamp,
           (unsigned long long)g_off.hp, (unsigned long long)g_off.level);
    ui_log("  hero/CD off: SE.m_ShowCoolDownComp=0x%llx SP.m_HeroName=0x%llx _dicCD=0x%llx",
           (unsigned long long)g_off.showCooldownComp, (unsigned long long)g_off.heroName,
           (unsigned long long)g_off.showCdDic);
    ui_log("  [retri] ShowEntity.m_Level=0x%llx (EXTERNAL CalculateRetriDamage)",
           (unsigned long long)g_off.level);
    ui_log("  [esp] ShowEntity._vecFaceDir=0x%llx", (unsigned long long)g_off.faceDir);
    ui_log("  [info] mp=0x%llx gold=0x%llx kda=0x%llx rank=0x%llx spell=0x%llx",
           (unsigned long long)g_off.mp, (unsigned long long)g_off.gold,
           (unsigned long long)g_off.kills, (unsigned long long)g_off.rank,
           (unsigned long long)g_off.summonSkillId);
    ui_log("  [hero] m_ID=0x%llx SP.m_iOriginHeroId=0x%llx BM.m_RunBullets=0x%llx _RunBullets=0x%llx",
           (unsigned long long)g_off.entityId, (unsigned long long)g_off.heroOriginId,
           (unsigned long long)g_off.bmStaticRunBullets, (unsigned long long)g_off.bmStaticRunBulletsList);
    ui_log("  [hero] Bullet m_Id=0x%llx pos=0x%llx", (unsigned long long)g_off.bulletId,
           (unsigned long long)g_off.bulletPos);
    if (!g_off.ready) ui_log("[resolve] WARN: incomplete runtime field offsets");
}

const EspRuntimeOff &esp_runtime_off() { return g_off; }

bool esp_runtime_off_ready() { return g_off.ready; }
