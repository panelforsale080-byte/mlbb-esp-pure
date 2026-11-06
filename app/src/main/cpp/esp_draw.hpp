#pragma once
#include "esp_engine.hpp"
#include "imgui.h"

struct EspFeatures {
    bool box = true;
    bool line = true;
    bool health = true;
    bool name = true;
    bool enemyCooldown = true;
    bool distance = true;
    bool monsterBox = true;
    bool monsterName = true;
    bool monsterHealth = true;
};

/* conRentV1 EspTab::DrawOverlay — external pseudo W2S (same layout as libtool overlay). */
void esp_draw_overlay(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                      const EspFeatures &feat);
