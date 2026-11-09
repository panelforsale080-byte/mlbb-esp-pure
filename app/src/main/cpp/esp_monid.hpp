#pragma once
#include "esp_engine.hpp"

void esp_monid_set_enabled(bool on);
void esp_monid_tick(const EspFrame &frame);
void esp_monid_draw();
