#pragma once
#include "esp_engine.hpp"
#include "imgui.h"

/** EXTERNAL-MLBB (2) ProcessAutoRetribution + Touch_Tap. */
struct EspRetriSettings {
    bool enabled = false;
    bool showCircle = true;
    bool targetRed = true;
    bool targetBlue = true;
    bool targetLord = true;
    bool targetTurtle = true;
    bool targetCrab = true;
    bool targetLito = true;
    /** When set, tapX/Y are recomputed from screen size (Retri anchor follows resolution). */
    bool useScreenAnchor = true;
    float tapNormX = 0.65625f; /* EXTERNAL default ~1575 @ 2400w */
    float tapNormY = 0.612f;   /* EXTERNAL default ~661 @ 1080h */
    float tapX = 1575.f;
    float tapY = 661.f;
    /** Fallback when ShowEntity.m_Level cannot be read. */
    int assumedLevel = 8;
    /** Updated each retri tick from live memory (menu display). */
    int liveRetriDamage = 0;
    int livePlayerLevel = 0;
    /** -1 when LogicPlayer.m_KillWildTimes not resolved/read. */
    int liveKillWildTimes = -1;
    /** Virtual/non-root: cast via IL2 TryUseSkill(20020), not screen tap. */
    bool useIl2SummonCast = true;
    /** Lv4+: full hunter damage (3000 @ 15). Off = legacy 1.5x base only (2580 @ 15). */
    bool advancedHunterDamage = true;
};

EspRetriSettings &esp_retri_settings();
void esp_retri_process(const EspFrame &frame);
void esp_retri_draw_indicator(ImDrawList *draw);
/** Drag white circle while menu is open to set spell tap anchor. */
void esp_retri_anchor_drag(bool menuOpen, float screenW, float screenH);
void esp_retri_set_default_spell_anchor();
void esp_retri_sync_anchor_pixels(float screenW, float screenH);
void esp_retri_notify_display(int w, int h);
