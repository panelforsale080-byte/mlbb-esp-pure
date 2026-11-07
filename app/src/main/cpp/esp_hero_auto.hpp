#pragma once
#include "esp_engine.hpp"

struct EspHeroAutoSettings {
    bool lingSword = false;
    bool gusionCombo = false;
    bool useIl2Cast = true;
    int comboDelayMs = 80;
    float lingMaxRange = 8.5f;
};

EspHeroAutoSettings &esp_hero_auto_settings();
void esp_hero_auto_process(const EspFrame &frame);
