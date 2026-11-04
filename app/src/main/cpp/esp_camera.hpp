#pragma once
#include <sys/types.h>

struct Vec3;

void esp_camera_reset();
void esp_camera_update(pid_t pid, float screenW, float screenH);
bool esp_camera_ready();
bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY);
