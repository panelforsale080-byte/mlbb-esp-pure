#pragma once
#include <cstdint>

/** EXTERNAL-MLBB Android_touch/TouchHelperA — uinput tap (root). */
bool Touch_Init(int w, int h, uint32_t orientation, bool readOnly);
void UpdateScreenData(int w, int h, uint32_t orientation);
void Touch_Close();
void Touch_Down(float x, float y);
void Touch_Move(float x, float y);
void Touch_Up();

static inline void Touch_Tap(int x, int y) {
    Touch_Down((float)x, (float)y);
    Touch_Up();
}
