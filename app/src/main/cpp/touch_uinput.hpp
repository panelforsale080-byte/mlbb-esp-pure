#pragma once

/** Virtual uinput tap only — no EVIOCGRAB, no reader threads (overlay-safe). */
bool touch_uinput_ready();
bool touch_uinput_open(int screenW, int screenH);
void touch_uinput_close();
bool touch_uinput_tap(int x, int y);

/** uinput if possible, else su/input shell. Returns method name for log (static buffer). */
const char *touch_inject_screen_tap(int x, int y, int screenW, int screenH);
