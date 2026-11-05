#pragma once
#include "conrent_esp.hpp"
#include <sys/types.h>

void esp_camera_reset();
void esp_camera_set_static_bases(uintptr_t showBattleCameraStatics, uintptr_t gameMethodStatics);
void esp_camera_set_conrent_w2s(pid_t pid, uintptr_t runtime_invoke, const ConrentResolve &cr);

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH, bool allowIl2Ptrace = false);
void esp_camera_frame_end();
bool esp_camera_ready();
bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY);
