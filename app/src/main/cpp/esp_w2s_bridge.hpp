#pragma once
#include <sys/types.h>
#include <cstdint>

/* Build #49 IL2 WorldToScreenPoint inside game process (injected .so); overlay uses vm_read/write only. */

void esp_w2s_bridge_set_agent_path(const char *absolutePath);
void esp_w2s_bridge_reset();
void esp_w2s_bridge_publish_cfg(pid_t pid, uintptr_t get_main_native, uintptr_t w2s_native,
                                float screenW, float screenH);
bool esp_w2s_bridge_ensure_injected(pid_t pid, const char *agentSoPath);
bool esp_w2s_bridge_active();
void esp_w2s_bridge_set_screen(float screenW, float screenH);
bool esp_w2s_bridge_world_to_screen(pid_t pid, float wx, float wy, float wz, float &outX,
                                    float &outY);
