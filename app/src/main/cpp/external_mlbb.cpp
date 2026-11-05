#include "external_mlbb.hpp"
#include "external_mlbb_offsets.hpp"
#include "mem.hpp"
#include "proc.hpp"
#include <cmath>
#include <cstring>

namespace {

static pid_t g_pid = 0;
static uintptr_t g_libbase = 0;

static uintptr_t read_ptr(pid_t pid, uintptr_t addr) {
    uintptr_t v = 0;
    if (!mem_read(pid, addr, v)) return 0;
    return v;
}

static bool module_base(pid_t pid, const char *suffix, uintptr_t &base) {
    base = 0;
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return false;
    for (const auto &m : maps) {
        if (m.perms[0] != 'r' || m.path.empty()) continue;
        if (m.path.find(suffix) == std::string::npos) continue;
        if (m.path.find(".so") == std::string::npos) continue;
        if (base == 0 || m.start < base) base = m.start;
    }
    return base != 0;
}

struct CamMatrices {
    ExtMat4 worldToCamera;
    ExtMat4 projection;
};

static ExtMat4 mul_mat4(const ExtMat4 &proj, const ExtMat4 &view) {
    /* EXTERNAL-MLBB Matrix4x4::operator* — projection * worldToCamera. */
    ExtMat4 r{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r.m[row * 4 + col] = proj.m[row * 4 + 0] * view.m[0 + col] +
                                 proj.m[row * 4 + 1] * view.m[4 + col] +
                                 proj.m[row * 4 + 2] * view.m[8 + col] +
                                 proj.m[row * 4 + 3] * view.m[12 + col];
        }
    }
    return r;
}

static void normalize_landscape(float &w, float &h) {
    if (w > 0.f && h > 0.f && w < h) {
        const float t = w;
        w = h;
        h = t;
    }
}

static uintptr_t main_camera(pid_t pid, uintptr_t libbase) {
    uintptr_t main_cam = read_ptr(pid, libbase + ExtOff::Camera::MainCameraPtr);
    if (!main_cam) return 0;
    main_cam = read_ptr(pid, main_cam + ExtOff::Camera::CameraData);
    if (!main_cam) return 0;
    main_cam = read_ptr(pid, main_cam + ExtOff::Camera::CameraData2);
    return main_cam;
}

} // namespace

void external_mlbb_reset() {
    g_pid = 0;
    g_libbase = 0;
}

bool external_mlbb_attach(pid_t pid) {
    if (pid <= 0 || !proc_alive(pid)) {
        external_mlbb_reset();
        return false;
    }
    if (g_pid == pid && g_libbase) return true;

    uintptr_t base = 0;
    if (!module_base(pid, "libcsharp.so", base))
        module_base(pid, "libil2cpp.so", base);
    if (!base) {
        external_mlbb_reset();
        return false;
    }
    g_pid = pid;
    g_libbase = base;
    return true;
}

uintptr_t external_mlbb_libbase() { return g_libbase; }

uintptr_t external_mlbb_battle_instance(pid_t pid) {
    if (!external_mlbb_attach(pid)) return 0;
    const uintptr_t libbase = g_libbase;
    uintptr_t a1 = read_ptr(pid, libbase + ExtOff::BattleManager::BasePtr);
    if (!a1) return 0;
    const uintptr_t chainOff = (uintptr_t)((0x100u | ExtOff::BattleManager::PtrChain1) & 0xFFu);
    uintptr_t a2 = read_ptr(pid, a1 + chainOff);
    if (!a2) return 0;
    uintptr_t a32 = read_ptr(pid, (a2 << 1) >> 1);
    return a32;
}

bool external_mlbb_update_matrix(pid_t pid, float screenW, float screenH, ExtMat4 &outVp) {
    outVp = {};
    if (!external_mlbb_attach(pid)) return false;

    normalize_landscape(screenW, screenH);
    if (screenW < 1.f || screenH < 1.f) return false;

    const uintptr_t camRoot = main_camera(pid, g_libbase);
    if (!camRoot) return false;
    const uintptr_t camera = read_ptr(pid, camRoot + ExtOff::Camera::SmoothFollow);
    if (!camera) return false;

    CamMatrices cm{};
    if (!mem_read(pid, camera + ExtOff::Camera::ViewMatrix, cm)) return false;
    outVp = mul_mat4(cm.projection, cm.worldToCamera);
    return true;
}

bool external_mlbb_world_to_screen(const ExtMat4 &vp, float wx, float wy, float wz, float screenW,
                                   float screenH, float &outX, float &outY) {
    normalize_landscape(screenW, screenH);
    /* Same as EXTERNAL-MLBB Matrix4x4::MultiplyPoint + WorldToScreen. */
    const float w = vp.m[12] * wx + vp.m[13] * wy + vp.m[14] * wz + vp.m[15];
    if (w < 0.01f) return false;

    float cx = vp.m[0] * wx + vp.m[4] * wy + vp.m[8] * wz + vp.m[12];
    float cy = vp.m[1] * wx + vp.m[5] * wy + vp.m[9] * wz + vp.m[13];
    const float iw = w;
    cx /= iw;
    cy /= iw;

    const float sx = (cx + 1.f) * 0.5f;
    const float sy = (cy + 1.f) * 0.5f;
    outX = sx * screenW;
    outY = screenH - (sy * screenH);
    return std::isfinite(outX) && std::isfinite(outY);
}
