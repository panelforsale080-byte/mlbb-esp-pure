#pragma once
//INCLUDE YOUR AUTO AIM FILE
int delay = 0;
Vector3 gusionTarget = Vector3::zero();
int trigger = 0;
#include "Gusion.hpp"
#include "SoliceAim.h"

bool AimLing;
bool AutoRetriBuff;
bool AutoRetriLord;
bool AutoRetriTurtle;
bool Litho;
bool Crab;
int CalculateRetriDamage(int m_Level, int m_KillWildTimes) {
int AddRetriDmg, CalculateRetriDamage = (m_KillWildTimes>5) ? 520 + ((m_Level+5)*80), 520 + ((m_Level+5)*160), 520 + ((m_Level+5)*240), /*Next 7*/ 780 + ((m_Level+5)*480), 780 + ((m_Level+6)*600), 780 + ((m_Level+7)*720), 780 + ((m_Level+8)*840), 780 + ((m_Level+9)*960), 780 + ((m_Level+10)*1080), 780 + ((m_Level+11)*1200), 780 + ((m_Level+12)*1440), 780 + ((m_Level+13)*1560), 780 + ((m_Level+14)*1680), 780 + ((m_Level+15)*1800) : 0;
if (m_KillWildTimes < 5) {
        
        return 530 + (80 * m_Level);
    } else {
        return 1.5 * (530 + (80 * m_Level)); + (380 + (40 * m_Level)) - 120;  
    }
}

float skillcontrol = 0;
int speedcontrol = 25;



void (*orig_UpdateRetribution)(void *master);
void UpdateRetribution(void *master) {

if (AimLing) {  
        float MaxDist = std::numeric_limits<float>::infinity();
        Vector3 EntityPos = Vector3::zero();
        Vector3 Hp;
        float MaxEnemyHP = std::numeric_limits<float>::infinity();
        void *BattleManager_Instance;
        Il2CppGetStaticFieldValue("Assembly-CSharp.dll", "", "BattleManager", "Instance", &BattleManager_Instance);
        if (BattleManager_Instance) {
            auto m_LocalPlayerShow = *(uintptr_t *) ((uintptr_t)BattleManager_Instance + BattleManager_m_LocalPlayerShow);
            if (m_LocalPlayerShow) {
				
				//Created By Owner solice
                auto selfPos = *(Vector3 *) ((uintptr_t)m_LocalPlayerShow + ShowEntity__Position);
                auto LogicPlayerFighter = *(void **)((long)m_LocalPlayerShow + ShowEntity__logicFighter);  
                auto HeroID = *(int *) ((uintptr_t)m_LocalPlayerShow + EntityBase_m_ID);
                if (selfPos != Vector3::zero()) {
                    monoList<void **> *_RunBullets;
                    Il2CppGetStaticFieldValue("Assembly-CSharp.dll", "", "BattleManager", "_RunBullets", &_RunBullets);
                       if (_RunBullets) {
						   
						   
                        for (int i = 1; i < _RunBullets->getSize(); i++) {
                         auto Pawnsolicejui = _RunBullets->getItems()[i];
                         if (!Pawnsolicejui) continue;
                         auto m_ID = *(int *) ((uintptr_t)Pawnsolicejui + Bullet_m_Id);
                         if (m_ID != 8452) continue;
							
                            //Created By Owner solice
                         auto _Position = *(Transform **) ((uintptr_t)Pawnsolicejui + Bullet_transform);
                         float Distance = (int) Vector3::Distance(selfPos, _Position->get_position());                        
                         if (Distance < 5.5f) {
                         EntityPos = _Position->get_position();
                         }
                         if (EntityPos != Vector3::zero()) {
                         orig_TryUseSkill(master, 8420, Vector3::Normalized(EntityPos - selfPos), true, Vector3::Distance(selfPos, _Position->get_position()), true, true);
                         }
}}}}}}


    if (AutoRetriBuff || AutoRetriLord || AutoRetriTurtle || Litho || Crab) {
        if (master != NULL) {
            void *BattleManager_Instance;
            Il2CppGetStaticFieldValue("Assembly-CSharp.dll", "", "BattleManager", "Instance", &BattleManager_Instance);
            if (BattleManager_Instance) {
                auto m_LocalPlayerShow = *(uintptr_t *) ((uintptr_t)BattleManager_Instance + BattleManager_m_LocalPlayerShow);
                if (m_LocalPlayerShow) {
                    auto selfPos = *(Vector3 *) ((uintptr_t)m_LocalPlayerShow + ShowEntity__Position);
                    if (selfPos != Vector3::zero()) {
                        auto m_Level1 = *(int *) ((uintptr_t)m_LocalPlayerShow + EntityBase_m_Level);
                        auto _logicFighter = *(uintptr_t *) ((uintptr_t)m_LocalPlayerShow + ShowEntity__logicFighter);
                        int iCalculateDamage;
                        if (_logicFighter) {
                            auto m_KillWildTimes = *(int *) ((uintptr_t)_logicFighter + LogicPlayer_m_KillWildTimes);
                            iCalculateDamage = CalculateRetriDamage(m_Level1, m_KillWildTimes);
                        }
                        monoList<void **> *m_ShowMonsters = *(monoList<void **> **) ((uintptr_t)BattleManager_Instance + BattleManager_m_ShowMonsters);
                        if (m_ShowMonsters) {
                            for (int i = 0; i < m_ShowMonsters->getSize(); i++) {
                                auto Pawnprosolice = m_ShowMonsters->getItems()[i];
                                if (!Pawnprosolice) continue;
                                auto m_ID = *(int *) ((uintptr_t)Pawnprosolice + EntityBase_m_ID);
                                if (MonsterToString(m_ID) == "") continue;
                                auto m_bSameCampType = *(bool *) ((uintptr_t)Pawnprosolice + EntityBase_m_bSameCampType);
                                if (m_bSameCampType) continue;
                                auto m_bDeath = *(bool *) ((uintptr_t)Pawnprosolice + EntityBase_m_bDeath);
                                if (m_bDeath) continue;
                                auto _Position = *(Vector3 *) ((uintptr_t)Pawnprosolice + ShowEntity__Position);
                                float Distance = (int) Vector3::Distance(selfPos, _Position);
                                float CurHP = *(int *) ((uintptr_t)Pawnprosolice + EntityBase_m_Hp);
                                float MaxHP = *(int *) ((uintptr_t)Pawnprosolice + EntityBase_m_HpMax);
                                if (Distance < 5.4f && CurHP < MaxHP) {
                                    if (CurHP <= iCalculateDamage) {
                                        if (m_ID == 2004 && AutoRetriBuff /*Red Buff*/ || m_ID == 2005 && AutoRetriBuff /*Blue Buff*/ || m_ID == 2002 && AutoRetriLord /*Lord*/ || m_ID == 2003 && AutoRetriTurtle /*Turtle*/ || m_ID == 2056 && Litho /*Litho*/ || m_ID == 2072 && Litho /*Litho*/ || m_ID == 2011 && Crab /*Crab*/ || m_ID == 2013 && Crab /*Crab*/) {
                                            if (Vector3::Normalized(_Position - selfPos) != Vector3::zero()) {
                                                orig_TryUseSkill(master, 20020, Vector3::Normalized(_Position - selfPos), true, Vector3::zero(), false, true);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (gusionToggle) 
        handleGusion(master);
    orig_UpdateRetribution(master);
}
