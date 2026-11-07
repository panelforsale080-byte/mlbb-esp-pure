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
void ui_sync_combo_fab_visible(bool show);
/** Push menu open/closed to Android overlay touch flags (pass-through when closed). */
void ui_sync_menu_touch_state();

void ui_menu_video_sync(bool visible, int x, int y, int w, int h);
void ui_menu_video_set_muted(bool muted);
void ui_menu_video_apply_muted_from_java(bool muted);
bool ui_menu_video_is_muted();
/** Hide video + force Java pass-through (menu X / minimize). */
void ui_force_menu_pass_through();
