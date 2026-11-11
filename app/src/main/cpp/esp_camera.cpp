#include "esp_camera.hpp"
#include "camera_lib_chain.hpp"
#include "matrix4x4.hpp"
#include "mem.hpp"
#include "proc.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <atomic>
#include <cmath>

namespace {

struct BattleCameraMatrices {
    Matrix4x4 worldToCameraMatrix;
    Matrix4x4 projectionMatrix;
};

struct Quat {
    float x, y, z, w;
};

static EspCameraRuntime g_rt{};
static uintptr_t g_sbcStatics = 0;
static uintptr_t g_gmStatics = 0;
static bool g_matrixReady = false;
static bool g_matrixFromVmRead = false;
static uintptr_t g_matrixLibBase = 0;
static uintptr_t g_matrixMainCamRva = 0;
static uintptr_t g_mainCameraSlotVa = 0;
static bool g_mainCameraSlotLiveOk = false;
static Matrix4x4 g_viewProj;
static bool g_vpHave = false;
static pid_t g_vpPid = 0;
static uintptr_t g_fastHolder = 0;

static bool viewproj_sane() {
    float acc = 0.f;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(g_viewProj.m[i])) return false;
        acc += std::fabs(g_viewProj.m[i]);
    }
    return acc > 0.01f;
}

static int g_matrixPathLog = 0;
static int g_matrixFailLog = 0;
static float g_screenW = 0.f;
static float g_screenH = 0.f;

static pid_t g_il2Pid = 0;
static uintptr_t g_rtInvoke = 0;
static uintptr_t g_getMainMi = 0;
static uintptr_t g_w2sMi = 0;
static uintptr_t g_worldToCamMi = 0;
static uintptr_t g_projectionMi = 0;
static bool g_il2Ready = false;

static RemoteSession g_sess{};
static bool g_inFrame = false;
static uintptr_t g_mainCam = 0;
static bool g_useIl2 = false;

/* Double-buffer published projection after frame_begin — one consistent matrix for all W2S per pass. */
static EspCameraProjSnap g_projSlots[2];
static std::atomic<int> g_projLatest{-1};

static void proj_publish(pid_t pid) {
    if (pid <= 0) return;
    const int cur = g_projLatest.load(std::memory_order_relaxed);
    const int w = (cur < 0 || cur > 1) ? 0 : (1 - cur);
    EspCameraProjSnap s{};
    s.pid = pid;
    s.screenW = g_screenW;
    s.screenH = g_screenH;
    if (g_matrixReady && !g_useIl2) {
        s.validMatrixProj = true;
        s.useIl2Fallback = false;
        s.viewProj = g_viewProj;
    } else if (g_useIl2) {
        s.validMatrixProj = false;
        s.useIl2Fallback = true;
    }
    g_projSlots[w] = s;
    g_projLatest.store(w, std::memory_order_release);
}

static void quat_rotate(const Quat &q, float ix, float iy, float iz, float &ox, float &oy,
                        float &oz) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2.f * (y * iz - z * iy);
    const float ty = 2.f * (z * ix - x * iz);
    const float tz = 2.f * (x * iy - y * ix);
    ox = ix + w * tx + (y * tz - z * ty);
    oy = iy + w * ty + (z * tx - x * tz);
    oz = iz + w * tz + (x * ty - y * tx);
}

static void normalize_landscape(float &w, float &h) {
    if (w > 0.f && h > 0.f && w < h) {
        const float t = w;
        w = h;
        h = t;
    }
}

static bool matrix_sane(const Matrix4x4 &m) {
    if (!std::isfinite(m.m00) || !std::isfinite(m.m33)) return false;
    const float mag = std::fabs(m.m00) + std::fabs(m.m11) + std::fabs(m.m22) + std::fabs(m.m33);
    return mag > 1e-4f && mag < 1e8f;
}

static bool apply_battle_camera_matrices(const BattleCameraMatrices &cam) {
    g_viewProj = cam.projectionMatrix * cam.worldToCameraMatrix;
    return matrix_sane(g_viewProj);
}

/** EXTERNAL-MLBB: SmoothFollow step + ViewMatrix @ 0x5C (two Matrix4x4). */
static bool load_vp_matrices_at_main_cam3(pid_t pid, uintptr_t mainCam3) {
    if (!mem_ptr_ok(mainCam3)) return false;
    constexpr uintptr_t kSmoothFollow = 0x10;
    constexpr uintptr_t kViewMatrix = 0x5C;
    uintptr_t holder = 0;
    mem_read(pid, mainCam3 + kSmoothFollow, holder);
    uintptr_t unityCam = 0;
    if (g_rt.sf_m_MainCamera)
        mem_read(pid, mainCam3 + g_rt.sf_m_MainCamera, unityCam);
    const uintptr_t holders[] = {holder, mainCam3, unityCam};
    for (uintptr_t h : holders) {
        if (!mem_ptr_ok(h)) continue;
        BattleCameraMatrices cam{};
        if (!mem_read(pid, h + kViewMatrix, cam)) continue;
        if (apply_battle_camera_matrices(cam)) {
            g_fastHolder = h;
            return true;
        }
    }
    return false;
}

static bool load_vp_from_discovered_main_camera_slot(pid_t pid) {
    uintptr_t root = 0;
    if (g_mainCameraSlotVa) {
        if (!mem_read(pid, g_mainCameraSlotVa, root)) return false;
    } else if (g_matrixLibBase && g_matrixMainCamRva) {
        if (!mem_read(pid, g_matrixLibBase + g_matrixMainCamRva, root)) return false;
    } else {
        return false;
    }
    const LibMainCamChain chain = camera_resolve_lib_main_cam_chain(pid, root);
    if (!chain.cam3 || !chain.vpOk) return false;
    return load_vp_matrices_at_main_cam3(pid, chain.cam3);
}

static bool load_vp_from_game_method_smooth(pid_t pid) {
    uintptr_t smooth = mem_ptr_normalize(g_rt.game_method_main_camera_obj);
    if (!smooth && g_gmStatics && g_rt.gm_mainCamera) {
        uintptr_t raw = 0;
        if (mem_read(pid, g_gmStatics + g_rt.gm_mainCamera, raw)) smooth = mem_ptr_normalize(raw);
    }
    if (!smooth) return false;
    return load_vp_matrices_at_main_cam3(pid, smooth);
}

static bool load_vp_show_battle_camera(pid_t pid) {
    if (!g_sbcStatics || !g_rt.sbc_m_CamPos || !g_rt.sbc_m_CamRot) return false;
    Vec3 pos{};
    Quat rot{};
    int fov = 0;
    if (!mem_read(pid, g_sbcStatics + g_rt.sbc_m_CamPos, pos)) return false;
    if (!mem_read(pid, g_sbcStatics + g_rt.sbc_m_CamRot, rot)) return false;
    if (g_rt.sbc_m_iFieldOfView)
        mem_read(pid, g_sbcStatics + g_rt.sbc_m_iFieldOfView, fov);
    if (fov < 10 || fov > 120) fov = 40;
    if (!std::isfinite(pos.x) || !std::isfinite(rot.w)) return false;

    float fx, fy, fz, ux, uy, uz;
    quat_rotate(rot, 0, 0, 1, fx, fy, fz);
    quat_rotate(rot, 0, 1, 0, ux, uy, uz);

    Vec3 f{fx, fy, fz};
    const float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (fl < 1e-4f) return false;
    f.x /= fl;
    f.y /= fl;
    f.z /= fl;
    Vec3 u{ux, uy, uz};
    Vec3 s{f.y * u.z - f.z * u.y, f.z * u.x - f.x * u.z, f.x * u.y - f.y * u.x};
    const float sl = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    if (sl < 1e-4f) return false;
    s.x /= sl;
    s.y /= sl;
    s.z /= sl;
    u = {s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x};

    const float tx = -(s.x * pos.x + s.y * pos.y + s.z * pos.z);
    const float ty = -(u.x * pos.x + u.y * pos.y + u.z * pos.z);
    const float tz = (f.x * pos.x + f.y * pos.y + f.z * pos.z);
    const Matrix4x4 view(s.x, s.y, s.z, 0.f, u.x, u.y, u.z, 0.f, -f.x, -f.y, -f.z, 0.f, tx, ty, tz,
                         1.f);

    const float aspect = (g_screenW > 0.f && g_screenH > 0.f) ? (g_screenW / g_screenH) : (16.f / 9.f);
    const float nearP = 0.3f, farP = 2000.f;
    const float t = 1.f / std::tan((float)fov * 0.5f * 3.14159265f / 180.f);
    const float a = (farP + nearP) / (nearP - farP);
    const float b = (2.f * farP * nearP) / (nearP - farP);
    const Matrix4x4 proj(t / aspect, 0.f, 0.f, 0.f, 0.f, t, 0.f, 0.f, 0.f, 0.f, a, b, 0.f, 0.f, -1.f,
                         0.f);

    g_viewProj = proj * view;
    g_matrixFromVmRead = false;
    return matrix_sane(g_viewProj);
}

static bool matrix_w2s(float wx, float wy, float wz, float &outX, float &outY) {
    if (!g_matrixReady) return false;
    const Vector3 from(wx, wy, wz);
    const float w = g_viewProj.m30 * from.X + g_viewProj.m31 * from.Y + g_viewProj.m32 * from.Z +
                    g_viewProj.m33;
    if (w < 0.01f) return false;

    const Vector3 clip = g_viewProj.MultiplyPoint(from);
    const float sx = (clip.X + 1.f) * 0.5f;
    const float sy = (clip.Y + 1.f) * 0.5f;
    outX = sx * g_screenW;
    outY = g_screenH - sy * g_screenH;
    return std::isfinite(outX) && std::isfinite(outY);
}

} // namespace

void esp_camera_reset() {
    g_projLatest.store(-1, std::memory_order_release);
    g_projSlots[0] = {};
    g_projSlots[1] = {};
    g_rt = {};
    g_sbcStatics = 0;
    g_gmStatics = 0;
    g_matrixReady = false;
    g_matrixFromVmRead = false;
    g_matrixLibBase = 0;
    g_matrixMainCamRva = 0;
    g_mainCameraSlotVa = 0;
    g_mainCameraSlotLiveOk = false;
    g_matrixPathLog = 0;
    g_matrixFailLog = 0;
    g_vpHave = false;
    g_vpPid = 0;
    g_fastHolder = 0;
    g_il2Pid = 0;
    g_rtInvoke = 0;
    g_getMainMi = 0;
    g_w2sMi = 0;
    g_worldToCamMi = 0;
    g_projectionMi = 0;
    g_il2Ready = false;
    g_useIl2 = false;
    g_mainCam = 0;
    if (g_inFrame) {
        remote_session_end(g_sess);
        g_inFrame = false;
    }
}

void esp_camera_set_static_bases(uintptr_t showBattleCameraStatics, uintptr_t gameMethodStatics) {
    g_sbcStatics = showBattleCameraStatics;
    g_gmStatics = gameMethodStatics;
}

void esp_camera_set_runtime(const EspCameraRuntime &runtime) {
    g_rt = runtime;
    g_matrixLibBase = runtime.matrix_lib_base;
    g_matrixMainCamRva = runtime.matrix_main_camera_rva;
    g_mainCameraSlotVa = runtime.main_camera_global_slot_va;
    g_mainCameraSlotLiveOk = runtime.main_camera_slot_live_ok;
    g_vpHave = false;
    g_vpPid = 0;
    g_fastHolder = 0;
}

void esp_camera_set_il2cpp_w2s(pid_t pid, uintptr_t runtime_invoke, uintptr_t get_main_method,
                               uintptr_t w2s_method) {
    g_il2Pid = pid;
    g_rtInvoke = runtime_invoke;
    g_getMainMi = get_main_method;
    g_w2sMi = w2s_method;
    g_worldToCamMi = g_rt.camera_world_to_camera_method;
    g_projectionMi = g_rt.camera_projection_matrix_method;
    g_il2Ready = runtime_invoke && get_main_method && w2s_method;
}

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH, bool allowIl2Ptrace) {
    g_screenW = screenW;
    g_screenH = screenH;
    normalize_landscape(g_screenW, g_screenH);
    g_matrixReady = false;
    g_matrixFromVmRead = false;
    g_useIl2 = false;
    g_mainCam = 0;
    g_worldToCamMi = g_rt.camera_world_to_camera_method;
    g_projectionMi = g_rt.camera_projection_matrix_method;
    g_matrixLibBase = g_rt.matrix_lib_base;
    g_matrixMainCamRva = g_rt.matrix_main_camera_rva;
    g_mainCameraSlotVa = g_rt.main_camera_global_slot_va;
    g_mainCameraSlotLiveOk = g_rt.main_camera_slot_live_ok;

    if (g_inFrame) remote_session_end(g_sess);
    g_inFrame = false;

    /* Reuse only when the caller did not ask for a new matrix.
       The draw path never seizes the game thread. */
    if (!allowIl2Ptrace && g_vpHave && g_vpPid == pid && viewproj_sane()) {
        g_matrixReady = true;
        g_matrixFromVmRead = true;
        g_useIl2 = false;
        proj_publish(pid);
        return;
    }

    Matrix4x4 savedVp = g_viewProj;
    const bool savedOk = g_vpHave && g_vpPid == pid && viewproj_sane();
    /* One 128-byte read of the holder that last worked. Pointer chase only if it misses. */
    if (pid > 0 && g_vpPid == pid && mem_ptr_ok(g_fastHolder)) {
        BattleCameraMatrices cam{};
        if (mem_read(pid, g_fastHolder + 0x5C, cam) && apply_battle_camera_matrices(cam)) {
            g_matrixReady = true;
            g_matrixFromVmRead = true;
            g_vpHave = true;
            g_vpPid = pid;
            proj_publish(pid);
            return;
        }
        if (savedOk) g_viewProj = savedVp;
        g_fastHolder = 0;
    }

    /* Draw tick only reads. Same slot, GameMethod, then ShowBattleCamera result.
       Calling the camera getters here stops the game thread, so that fallback stays off. */

    if (pid > 0 && proc_alive(pid)) {
        /* live=OK lib slot = accurate (build ~211). Else GameMethod, then the same slot read. */
        if (g_mainCameraSlotLiveOk && g_mainCameraSlotVa &&
            load_vp_from_discovered_main_camera_slot(pid)) {
            g_matrixReady = true;
            g_matrixFromVmRead = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 1;
                ui_log("[esp] W2S matrix: slot_va=0x%llx chain (live validated, accurate)",
                       (unsigned long long)g_mainCameraSlotVa);
            }
        } else if (load_vp_from_game_method_smooth(pid)) {
            g_matrixReady = true;
            g_matrixFromVmRead = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 2;
                ui_log("[esp] W2S matrix: GameMethod.mainCamera (IL2CPP)");
            }
        } else if (g_mainCameraSlotVa && load_vp_from_discovered_main_camera_slot(pid)) {
            g_matrixReady = true;
            g_matrixFromVmRead = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 4;
                ui_log("[esp] W2S matrix: slot_va=0x%llx (lib fallback, live not confirmed)",
                       (unsigned long long)g_mainCameraSlotVa);
            }
        }
        if (!g_matrixReady && load_vp_show_battle_camera(pid)) {
            g_matrixReady = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 5;
                ui_log("[esp] W2S matrix: ShowBattleCamera fallback");
            }
        }
    }

    if (g_matrixReady && viewproj_sane()) {
        g_vpHave = true;
        g_vpPid = pid;
    } else if (savedOk) {
        g_viewProj = savedVp;
        g_matrixReady = true;
        g_matrixFromVmRead = true;
        g_vpHave = true;
        g_vpPid = pid;
    } else if (!viewproj_sane()) {
        g_vpHave = false;
    }

    if (!g_matrixReady && !g_useIl2 && pid > 0 && g_matrixFailLog == 0) {
        g_matrixFailLog = 1;
        uintptr_t live = 0;
        if (g_gmStatics && g_rt.gm_mainCamera)
            mem_read(pid, g_gmStatics + g_rt.gm_mainCamera, live);
        ui_log("[esp] W2S matrix FAILED gmStatics=0x%llx mainCamera=0x%llx lib=0x%llx rva=0x%llx",
               (unsigned long long)g_gmStatics, (unsigned long long)live,
               (unsigned long long)g_matrixLibBase, (unsigned long long)g_matrixMainCamRva);
    }

    proj_publish(pid);
}

void esp_camera_frame_end() {
    if (g_inFrame) {
        remote_session_end(g_sess);
        g_inFrame = false;
    }
    g_mainCam = 0;
}

bool esp_camera_ready() { return g_useIl2 || g_matrixReady; }

void esp_camera_acquire_proj_for_draw(pid_t pid, EspCameraProjSnap &out) {
    out = {};
    if (pid <= 0) return;
    const int idx = g_projLatest.load(std::memory_order_acquire);
    if (idx < 0 || idx > 1) return;
    out = g_projSlots[idx];
    if (out.pid != pid) out = {};
}

bool esp_camera_proj_snap_w2s(const EspCameraProjSnap &snap, float wx, float wy, float wz, float &outX,
                              float &outY) {
    if (!snap.validMatrixProj) return false;
    const Vector3 from(wx, wy, wz);
    const float w = snap.viewProj.m30 * from.X + snap.viewProj.m31 * from.Y +
                    snap.viewProj.m32 * from.Z + snap.viewProj.m33;
    if (w < 0.01f) return false;
    const Vector3 clip = snap.viewProj.MultiplyPoint(from);
    const float sx = (clip.X + 1.f) * 0.5f;
    const float sy = (clip.Y + 1.f) * 0.5f;
    outX = sx * snap.screenW;
    outY = snap.screenH - sy * snap.screenH;
    return std::isfinite(outX) && std::isfinite(outY);
}

bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY) {
    /* Same MultiplyPoint projection as build 448. No in-process camera call. */
    if (g_matrixReady && matrix_w2s(wx, wy, wz, outX, outY)) return true;
    return false;
}
