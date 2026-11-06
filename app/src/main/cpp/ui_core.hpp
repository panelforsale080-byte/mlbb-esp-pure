#pragma once
#include <android/native_window.h>
#include "imgui.h"

ImVec2 &ui_initial_screen_size();

void ui_set_display_size(int width, int height);
void ui_esp_pipeline_idle();

void ui_gl_surface_created();
void ui_gl_surface_changed(int width, int height);
void ui_gl_surface_destroyed();
void ui_gl_draw_frame();

void ui_touch(int action, float x, float y);
void ui_open_menu();
void ui_toggle_menu();
void ui_minimize_menu();
bool ui_menu_interactive();

void ui_esp_start();
void ui_esp_stop();
bool ui_esp_is_active();
