#pragma once
#include <cstdint>
#include <sys/types.h>

struct EspCameraRuntime {
    uintptr_t gm_mainCamera = 0;
    uintptr_t sf_m_MainCamera = 0;
    uintptr_t sbc_m_CamPos = 0;
    uintptr_t sbc_m_CamRot = 0;
    uintptr_t sbc_m_iFieldOfView = 0;
    uintptr_t camera_world_to_camera_method = 0;
    uintptr_t camera_projection_matrix_method = 0;
    uintptr_t matrix_lib_base = 0;
    uintptr_t matrix_main_camera_rva = 0;
    uintptr_t game_method_main_camera_obj = 0;
};

void esp_camera_reset();
void esp_camera_set_static_bases(uintptr_t showBattleCameraStatics, uintptr_t gameMethodStatics);
void esp_camera_set_runtime(const EspCameraRuntime &runtime);
void esp_camera_set_il2cpp_w2s(pid_t pid, uintptr_t runtime_invoke, uintptr_t get_main_method,
                               uintptr_t w2s_method);

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH, bool allowIl2Ptrace = false);
void esp_camera_frame_end();
bool esp_camera_ready();
bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY);
