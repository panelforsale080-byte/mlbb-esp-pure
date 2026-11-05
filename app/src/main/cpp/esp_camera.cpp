#include "esp_camera.hpp"
#include "matrix4x4.hpp"
#include "mem.hpp"
#include "proc.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <cmath>
#include <string>
#include <vector>

namespace {

/** EXTERNAL-MLBB / EspSource Offsets::Camera */
namespace CamOff {
constexpr uintptr_t kMainCameraPtr = 0x635b620;
constexpr uintptr_t kCameraData = 0xa8;
constexpr uintptr_t kCameraData2 = 0x08;
constexpr uintptr_t kSmoothFollow = 0x10;
constexpr uintptr_t kViewMatrix = 0x5C;
} // namespace CamOff

/** EspSource: worldToCamera then projection at ViewMatrix. */
struct BattleCameraMatrices {
    Matrix4x4 worldToCameraMatrix;
    Matrix4x4 projectionMatrix;
};

struct Quat {
    float x, y, z, w;
};

static uintptr_t g_sbcStatics = 0;
static uintptr_t g_gmStatics = 0;
static bool g_matrixReady = false;
/** VP from EXTERNAL-style read (lib MainCamera chain or GameMethod), not manual ShowBattleCamera. */
static bool g_matrixExternal = false;
static Matrix4x4 g_viewProj;
static int g_matrixPathLog = 0;
static float g_screenW = 0.f;
static float g_screenH = 0.f;

static pid_t g_il2Pid = 0;
static uintptr_t g_rtInvoke = 0;
static uintptr_t g_getMainMi = 0;
static uintptr_t g_w2sMi = 0;
static bool g_il2Ready = false;

static RemoteSession g_sess{};
static bool g_inFrame = false;
static uintptr_t g_mainCam = 0;
static bool g_useIl2 = false;
static int g_w2sBudget = 0;
constexpr int kMaxIl2W2sPerFrame = 128;

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

static uintptr_t module_base_lowest(pid_t pid, const char *needle) {
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return 0;
    uintptr_t base = 0;
    for (const MapsReg &m : maps) {
        if (m.path.find(needle) == std::string::npos) continue;
        if (!base || m.start < base) base = m.start;
    }
    return base;
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

/** EXTERNAL-MLBB GetMainCamera(): libbase+MainCameraPtr -> +0xa8 -> +0x08 */
static uintptr_t external_get_main_cam3(pid_t pid, uintptr_t libbase) {
    if (!libbase) return 0;
    uintptr_t p = 0;
    if (!mem_read(pid, libbase + CamOff::kMainCameraPtr, p) || !mem_ptr_ok(p)) return 0;
    if (!mem_read(pid, p + CamOff::kCameraData, p) || !mem_ptr_ok(p)) return 0;
    if (!mem_read(pid, p + CamOff::kCameraData2, p) || !mem_ptr_ok(p)) return 0;
    return p;
}

/** EXTERNAL draw path: main_cam3 + SmoothFollow(0x10) -> matrix holder + ViewMatrix(0x5C). */
static bool load_vp_from_main_cam3(pid_t pid, uintptr_t main_cam3) {
    if (!mem_ptr_ok(main_cam3)) return false;

    uintptr_t camera = 0;
    mem_read(pid, main_cam3 + CamOff::kSmoothFollow, camera);

    const uintptr_t candidates[] = {camera, main_cam3};
    for (uintptr_t holder : candidates) {
        if (!mem_ptr_ok(holder)) continue;
        BattleCameraMatrices cam{};
        if (!mem_read(pid, holder + CamOff::kViewMatrix, cam)) continue;
        if (apply_battle_camera_matrices(cam)) return true;
    }
    return false;
}

static bool load_vp_external_main_camera(pid_t pid) {
    uintptr_t lib = module_base_lowest(pid, "libcsharp.so");
    if (!lib) lib = module_base_lowest(pid, "libil2cpp.so");
    if (!lib) return false;
    const uintptr_t main_cam3 = external_get_main_cam3(pid, lib);
    if (!main_cam3) return false;
    return load_vp_from_main_cam3(pid, main_cam3);
}

static bool load_vp_game_method(pid_t pid) {
    if (!g_gmStatics) return false;
    uintptr_t smooth = 0;
    if (!mem_read(pid, g_gmStatics + 0x10, smooth) || !mem_ptr_ok(smooth)) return false;
    return load_vp_from_main_cam3(pid, smooth);
}

static bool load_vp_show_battle_camera(pid_t pid) {
    if (!g_sbcStatics) return false;
    Vec3 pos{};
    Quat rot{};
    int fov = 0;
    if (!mem_read(pid, g_sbcStatics + 0x10, pos)) return false;
    if (!mem_read(pid, g_sbcStatics + 0x58, rot)) return false;
    mem_read(pid, g_sbcStatics + 0x8C, fov);
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
    g_matrixExternal = false;
    return matrix_sane(g_viewProj);
}

static bool read_vec3_from_object(pid_t pid, uintptr_t obj, float &x, float &y, float &z) {
    if (!mem_ptr_ok(obj)) return false;
    for (uintptr_t off : {0x10u, 0x14u, 0x18u, 0x20u, 0x24u, 0x28u}) {
        Vec3 v{};
        if (!mem_read(pid, obj + off, v)) continue;
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) {
            x = v.x;
            y = v.y;
            z = v.z;
            return true;
        }
    }
    return false;
}

static bool il2_invoke_w2s(float wx, float wy, float wz, float &outX, float &outY) {
    if (!g_inFrame || !g_useIl2 || !g_rtInvoke || !g_w2sMi || g_il2Pid <= 0) return false;
    if (g_w2sBudget <= 0) return false;
    --g_w2sBudget;

    if (!mem_ptr_ok(g_mainCam)) {
        uintptr_t cam = 0;
        if (!remote_session_call(g_sess, g_rtInvoke, {g_getMainMi, 0, 0, 0}, cam)) {
            g_useIl2 = false;
            return false;
        }
        g_mainCam = cam;
    }
    if (!mem_ptr_ok(g_mainCam)) return false;

    const uintptr_t vecSlot = remote_session_scratch(g_sess, 0, 16);
    const uintptr_t paramSlot = remote_session_scratch(g_sess, 1, 8);
    const uintptr_t excSlot = remote_session_scratch(g_sess, 2, 8);
    if (!vecSlot || !paramSlot || !excSlot) return false;

    const float xyz[3] = {wx, wy, wz};
    mem_write(g_il2Pid, vecSlot, xyz, sizeof(xyz));
    mem_write(g_il2Pid, paramSlot, &vecSlot, sizeof(vecSlot));
    uintptr_t zero = 0;
    mem_write(g_il2Pid, excSlot, &zero, sizeof(zero));

    uintptr_t ret = 0;
    if (!remote_session_call(g_sess, g_rtInvoke, {g_w2sMi, g_mainCam, paramSlot, excSlot}, ret)) {
        g_useIl2 = false;
        return false;
    }

    float sx = 0.f, sy = 0.f, sz = 0.f;
    if (!read_vec3_from_object(g_il2Pid, ret, sx, sy, sz)) {
        if (!mem_read(g_il2Pid, vecSlot, sx) || !mem_read(g_il2Pid, vecSlot + 4, sy) ||
            !mem_read(g_il2Pid, vecSlot + 8, sz))
            return false;
    }

    if (sz > 0.f) {
        outX = sx;
        outY = g_screenH - sy;
    } else {
        outX = g_screenW - sx;
        outY = sy;
    }
    return std::isfinite(outX) && std::isfinite(outY);
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
    g_sbcStatics = 0;
    g_gmStatics = 0;
    g_matrixReady = false;
    g_matrixExternal = false;
    g_matrixPathLog = 0;
    g_il2Pid = 0;
    g_rtInvoke = 0;
    g_getMainMi = 0;
    g_w2sMi = 0;
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

void esp_camera_set_il2cpp_w2s(pid_t pid, uintptr_t runtime_invoke, uintptr_t get_main_method,
                               uintptr_t w2s_method) {
    g_il2Pid = pid;
    g_rtInvoke = runtime_invoke;
    g_getMainMi = get_main_method;
    g_w2sMi = w2s_method;
    g_il2Ready = runtime_invoke && get_main_method && w2s_method;
}

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH, bool allowIl2Ptrace) {
    g_screenW = screenW;
    g_screenH = screenH;
    normalize_landscape(g_screenW, g_screenH);
    g_matrixReady = false;
    g_matrixExternal = false;
    g_useIl2 = false;
    g_mainCam = 0;

    if (g_inFrame) remote_session_end(g_sess);
    g_inFrame = false;

    g_w2sBudget = kMaxIl2W2sPerFrame;

    if (pid > 0 && proc_alive(pid)) {
        if (load_vp_external_main_camera(pid)) {
            g_matrixReady = true;
            g_matrixExternal = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 1;
                ui_log("[esp] W2S matrix: EXTERNAL MainCamera chain (lib+0x%x -> +0x5C)",
                       (unsigned)CamOff::kMainCameraPtr);
            }
        } else if (load_vp_game_method(pid)) {
            g_matrixReady = true;
            g_matrixExternal = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 2;
                ui_log("[esp] W2S matrix: GameMethod.mainCamera + SmoothFollow + ViewMatrix");
            }
        } else if (load_vp_show_battle_camera(pid)) {
            g_matrixReady = true;
            if (g_matrixPathLog == 0) {
                g_matrixPathLog = 3;
                ui_log("[esp] W2S matrix: ShowBattleCamera pos/rot fallback");
            }
        }
    }

    /* IL2 W2S only when EXTERNAL matrix unavailable (same as EXTERNAL-MLBB read-only path). */
    if (!g_matrixExternal && allowIl2Ptrace && g_il2Ready && pid == g_il2Pid && g_rtInvoke &&
        pid > 0 && proc_alive(pid)) {
        if (remote_session_begin_try(pid, g_sess)) {
            g_inFrame = true;
            g_useIl2 = true;
        }
    }
}

void esp_camera_frame_end() {
    if (g_inFrame) {
        remote_session_end(g_sess);
        g_inFrame = false;
    }
    g_mainCam = 0;
}

bool esp_camera_ready() { return g_useIl2 || g_matrixReady; }

bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY) {
    /* EXTERNAL-MLBB WorldToScreen + Matrix4x4::MultiplyPoint */
    if (g_matrixReady && matrix_w2s(wx, wy, wz, outX, outY)) return true;
    if (g_useIl2 && il2_invoke_w2s(wx, wy, wz, outX, outY)) return true;
    return false;
}
