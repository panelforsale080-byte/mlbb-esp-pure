#pragma once
#include "esp_engine.hpp"

struct ImDrawList;

struct EspHeroAutoSettings {
    bool lingSword = false;
    bool gusionCombo = false;
    bool useIl2Cast = true;
    int comboDelayMs = 80;
    float lingMaxRange = 8.5f;
    int lastHeroId = 0;
    int lastBulletScan = 0;
    int lastBulletTotal = 0;
    bool showSkillCircles = true;

    float lingS2X = 0.f;
    float lingS2Y = 0.f;
    float gusionS1X = 0.f, gusionS1Y = 0.f;
    float gusionS2X = 0.f, gusionS2Y = 0.f;
    float gusionS3X = 0.f, gusionS3Y = 0.f;
    bool useSkillAnchorNorm = true;
    float lingS2NormX = 0.85f, lingS2NormY = 0.75f;
    float gS1NormX = 0.85f, gS1NormY = 0.75f;
    float gS2NormX = 0.78f, gS2NormY = 0.82f;
    float gS3NormX = 0.72f, gS3NormY = 0.58f;
};

EspHeroAutoSettings &esp_hero_auto_settings();
void esp_hero_auto_process(const EspFrame &frame);
void esp_hero_auto_sync_skill_pixels(float screenW, float screenH);
void esp_hero_auto_reset_skill_anchors();
void esp_hero_auto_draw_indicator(ImDrawList *draw);
void esp_hero_auto_anchor_drag(bool menuOpen, float screenW, float screenH);
