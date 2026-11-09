#pragma once
#include "imgui.h"
#include <sys/types.h>

void esp_lobby_set_enabled(bool on);
void esp_lobby_tick(pid_t pid);
void esp_lobby_draw(ImDrawList *draw, float screenW, float screenH);
