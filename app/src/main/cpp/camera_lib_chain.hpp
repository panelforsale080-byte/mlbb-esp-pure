#pragma once
#include "mem.hpp"
#include <cmath>
#include <cstdint>

inline uintptr_t mem_ptr_normalize(uintptr_t p) {
    if (!p) return 0;
    if (mem_ptr_ok(p)) return p;
    const uintptr_t masks[] = {~7ULL, ~0xFULL, 0x0000FFFFFFFFFFFFULL};
    for (uintptr_t m : masks) {
        const uintptr_t q = p & m;
        if (mem_ptr_ok(q)) return q;
    }
    return 0;
}

struct BattleCameraMatricesRaw {
    float worldToCamera[16];
    float projection[16];
};

inline bool camera_matrix4_sane(const float m[16]) {
    if (!std::isfinite(m[0]) || !std::isfinite(m[15])) return false;
    const float mag =
        std::fabs(m[0]) + std::fabs(m[5]) + std::fabs(m[10]) + std::fabs(m[15]);
    return mag > 1e-4f && mag < 1e8f;
}

inline bool camera_vp_sane_at_holder(pid_t pid, uintptr_t holder) {
    holder = mem_ptr_normalize(holder);
    if (!holder) return false;
    BattleCameraMatricesRaw cam{};
    if (!mem_read(pid, holder + 0x5C, cam)) return false;
    return camera_matrix4_sane(cam.worldToCamera) && camera_matrix4_sane(cam.projection);
}

inline bool camera_vp_from_cam3(pid_t pid, uintptr_t cam3) {
    cam3 = mem_ptr_normalize(cam3);
    if (!cam3) return false;
    uintptr_t inner = 0;
    mem_read(pid, cam3 + 0x10, inner);
    inner = mem_ptr_normalize(inner);
    if (inner && camera_vp_sane_at_holder(pid, inner)) return true;
    return camera_vp_sane_at_holder(pid, cam3);
}

struct LibMainCamChain {
    uintptr_t root = 0;
    uintptr_t afterA8 = 0;
    uintptr_t rawAtA8Plus8 = 0;
    uintptr_t cam3 = 0;
    const char *variant = "none";
    bool vpOk = false;
};

inline LibMainCamChain camera_resolve_lib_main_cam_chain(pid_t pid, uintptr_t root) {
    LibMainCamChain out{};
    out.root = mem_ptr_normalize(root);
    if (!out.root) return out;

    uintptr_t a8raw = 0;
    mem_read(pid, out.root + 0xa8, a8raw);
    out.afterA8 = mem_ptr_normalize(a8raw);
    if (!out.afterA8) return out;

    mem_read(pid, out.afterA8 + 0x08, out.rawAtA8Plus8);
    const uintptr_t camFrom8 = mem_ptr_normalize(out.rawAtA8Plus8);

    if (camFrom8 && camera_vp_from_cam3(pid, camFrom8)) {
        out.cam3 = camFrom8;
        out.variant = "+A8+0x08";
        out.vpOk = true;
        return out;
    }
    if (camera_vp_from_cam3(pid, out.afterA8)) {
        out.cam3 = out.afterA8;
        out.variant = "+A8_direct";
        out.vpOk = true;
        return out;
    }
    if (camFrom8) {
        out.cam3 = camFrom8;
        out.variant = "+A8+0x08(no_vp)";
        return out;
    }
    out.cam3 = out.afterA8;
    out.variant = "+A8_direct(no_vp)";
    return out;
}

inline bool camera_main_camera_live_matches(pid_t pid, uintptr_t cam3, uintptr_t expectedLive) {
    expectedLive = mem_ptr_normalize(expectedLive);
    cam3 = mem_ptr_normalize(cam3);
    if (!expectedLive) return true;
    if (!cam3) return camera_vp_sane_at_holder(pid, expectedLive);
    if (cam3 == expectedLive) return true;
    uintptr_t inner = 0;
    mem_read(pid, cam3 + 0x10, inner);
    inner = mem_ptr_normalize(inner);
    if (inner == expectedLive) return true;
    return camera_vp_sane_at_holder(pid, expectedLive);
}
