#pragma once
#include "esp_engine.hpp"

struct EspCameraProjSnap;
struct ImDrawList;

void esp_monid_set_enabled(bool on);
bool esp_monid_world_on();
void esp_monid_tick(const EspFrame &frame);
void esp_monid_project(const EspCameraProjSnap &snap);
void esp_monid_draw();
void esp_monid_draw_world(ImDrawList *draw);
