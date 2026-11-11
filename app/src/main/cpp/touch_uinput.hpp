#pragma once

/** Virtual uinput tap only — no EVIOCGRAB, no reader threads (overlay-safe). */
bool touch_uinput_ready();
bool touch_uinput_open(int screenW, int screenH);
void touch_uinput_close();
bool touch_uinput_tap(int x, int y);
bool touch_uinput_swipe(int x1, int y1, int x2, int y2, int durationMs);

/** uinput if possible, else su/input shell. Returns method name for log (static buffer). */
const char *touch_inject_screen_tap(int x, int y, int screenW, int screenH);
/** uinput swipe first, else su. durationMs e.g. 45 */
const char *touch_inject_screen_swipe(int x1, int y1, int x2, int y2, int durationMs, int screenW,
                                      int screenH);
