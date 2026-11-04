#include "esp_camera.hpp"
#include "mem.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <cmath>

namespace {

struct Matrix4x4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    void multiply_point(const Vec3 &p, float &cx, float &cy, float &cz, float &cw) const {
        cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        cz = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    }
};

struct Quat {
    float x, y, z, w;
};

static uintptr_t g_sbcStatics = 0;
static uintptr_t g_gmStatics = 0;
static bool g_matrixReady = false;
static Matrix4x4 g_viewProj;
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
constexpr int kMaxIl2W2sPerFrame = 28;

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

static bool load_vp_game_method(pid_t pid) {
    if (!g_gmStatics) return false;
    uintptr_t smooth = 0;
    if (!mem_read(pid, g_gmStatics + 0x10, smooth) || !mem_ptr_ok(smooth)) return false;
    return mem_read(pid, smooth + 0x5C, g_viewProj);
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

    Matrix4x4 view{};
    view.m[0] = s.x;
    view.m[4] = s.y;
    view.m[8] = s.z;
    view.m[12] = -(s.x * pos.x + s.y * pos.y + s.z * pos.z);
    view.m[1] = u.x;
    view.m[5] = u.y;
    view.m[9] = u.z;
    view.m[13] = -(u.x * pos.x + u.y * pos.y + u.z * pos.z);
    view.m[2] = -f.x;
    view.m[6] = -f.y;
    view.m[10] = -f.z;
    view.m[14] = (f.x * pos.x + f.y * pos.y + f.z * pos.z);
    view.m[3] = 0;
    view.m[7] = 0;
    view.m[11] = 0;
    view.m[15] = 1;

    const float aspect = (g_screenW > 0.f && g_screenH > 0.f) ? (g_screenW / g_screenH) : (16.f / 9.f);
    const float nearP = 0.3f, farP = 2000.f;
    const float t = 1.f / std::tan((float)fov * 0.5f * 3.14159265f / 180.f);
    Matrix4x4 proj{};
    proj.m[0] = t / aspect;
    proj.m[5] = t;
    proj.m[10] = (farP + nearP) / (nearP - farP);
    proj.m[11] = -1.f;
    proj.m[14] = (2.f * farP * nearP) / (nearP - farP);
    proj.m[15] = 0.f;

    Matrix4x4 vp{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            vp.m[c * 4 + row] = proj.m[0 * 4 + row] * view.m[c * 4 + 0] +
                                 proj.m[1 * 4 + row] * view.m[c * 4 + 1] +
                                 proj.m[2 * 4 + row] * view.m[c * 4 + 2] +
                                 proj.m[3 * 4 + row] * view.m[c * 4 + 3];
        }
    }
    g_viewProj = vp;
    return true;
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
        if (!remote_session_call(g_sess, g_rtInvoke, {g_getMainMi, 0, 0, 0}, cam)) return false;
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
    if (!remote_session_call(g_sess, g_rtInvoke, {g_w2sMi, g_mainCam, paramSlot, excSlot}, ret))
        return false;

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
    Vec3 p{wx, wy, wz};
    float cx, cy, cz, cw;
    g_viewProj.multiply_point(p, cx, cy, cz, cw);
    if (cw <= 0.001f) return false;
    const float ndcX = cx / cw;
    const float ndcY = cy / cw;
    outX = (ndcX + 1.f) * 0.5f * g_screenW;
    outY = (1.f - ndcY) * 0.5f * g_screenH;
    return std::isfinite(outX) && std::isfinite(outY);
}

} // namespace

void esp_camera_reset() {
    g_sbcStatics = 0;
    g_gmStatics = 0;
    g_matrixReady = false;
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

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH) {
    g_screenW = screenW;
    g_screenH = screenH;
    normalize_landscape(g_screenW, g_screenH);
    g_matrixReady = false;
    g_useIl2 = false;
    g_mainCam = 0;
    g_w2sBudget = 0;

    if (g_inFrame) remote_session_end(g_sess);
    g_inFrame = false;

    /* External overlay: matrix-only W2S (mem read). Per-frame ptrace Il2 W2S caused
     * mixed radar/screen coords → flicker and game stutter. conRent in-process only. */
    if (pid > 0) {
        if (load_vp_game_method(pid))
            g_matrixReady = true;
        else if (load_vp_show_battle_camera(pid))
            g_matrixReady = true;
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
    if (g_useIl2 && il2_invoke_w2s(wx, wy, wz, outX, outY)) return true;
    return matrix_w2s(wx, wy, wz, outX, outY);
}
