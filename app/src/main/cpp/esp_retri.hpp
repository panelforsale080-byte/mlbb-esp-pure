#pragma once
#include "esp_engine.hpp"
#include "imgui.h"

/** EXTERNAL-MLBB ProcessAutoRetribution — in-process (no file handoff). */
struct EspRetriSettings {
    bool enabled = false;
    bool showCircle = true;
    bool targetLord = true;
    bool targetTurtle = true;
    bool targetRed = true;
    bool targetBlue = true;
    bool targetCrab = true;
    bool targetLito = true;
    float tapX = 1575.f;
    float tapY = 661.f;
    /** Assumed hero level when IL2CPP Level field is not resolved (retri damage formula). */
    int assumedLevel = 8;
};

EspRetriSettings &esp_retri_settings();

/** Reader thread: evaluate jungle HP / distance and fire tap when lethal in range. */
void esp_retri_process(const EspFrame &frame);

/** GL / ImGui background — retribution button position marker. */
void esp_retri_draw_indicator(ImDrawList *draw);

void esp_retri_fire_tap(float x, float y);
/** Implemented in overlay_jni.cpp (JNI → Java). */
void esp_retri_jni_tap(float x, float y);
