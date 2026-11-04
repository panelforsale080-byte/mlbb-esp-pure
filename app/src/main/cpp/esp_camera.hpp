#pragma once
#include <sys/types.h>

void esp_camera_reset();
void esp_camera_set_static_bases(uintptr_t showBattleCameraStatics, uintptr_t gameMethodStatics);
void esp_camera_set_il2cpp_w2s(pid_t pid, uintptr_t runtime_invoke, uintptr_t get_main_method,
                               uintptr_t w2s_method);

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH);
void esp_camera_frame_end();
bool esp_camera_ready();
bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY);
