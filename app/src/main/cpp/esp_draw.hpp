#pragma once
#include "esp_engine.hpp"
#include "imgui.h"

struct EspFeatures {
    bool espOn = true;
    bool box = false;
    bool line = false;
    bool health = false;
    bool name = false;
    bool distance = false;
    bool monsterBox = true;
    bool monsterName = true;
    bool monsterHealth = false;
};

/* conRentV1 EspTab::DrawOverlay — external pseudo W2S (same layout as libtool overlay). */
void esp_draw_overlay(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                      const EspFeatures &feat);
