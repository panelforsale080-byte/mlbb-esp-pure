#pragma once
#include "esp_engine.hpp"
#include "imgui.h"

/**
 * Auto Retribution — EXTERNAL overlay port of VIP {@code SoliceRetri.h} / EXTERNAL-MLBB
 * {@code ProcessAutoRetribution}. VIP inject calls {@code TryUseSkill(20020, ...)}; pure
 * external uses {@code input tap} at the Retribution button (root / shell).
 */
struct EspRetriSettings {
    bool enabled = false;
    /** VIP: Toggle_Shortcut Buff/Turtle/Lord */
    bool shortcutBuffTurtleLord = false;
    bool buff = true;    /* red + blue (VIP AutoRetriBuff) */
    bool turtle = true;
    bool lord = true;
    bool litho = true;
    bool crab = true;
    bool showCircle = true;
    float tapX = 1575.f;
    float tapY = 661.f;
    int assumedLevel = 8;
    /** VIP LogicPlayer_m_KillWildTimes — use 0 if unread (lower retri dmg formula). */
    int assumedKillWildTimes = 0;
};

EspRetriSettings &esp_retri_settings();

void esp_retri_process(const EspFrame &frame);
void esp_retri_draw_indicator(ImDrawList *draw);
