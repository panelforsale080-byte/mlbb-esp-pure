#include "esp_camera.hpp"
#include "mem.hpp"
#include "proc.hpp"
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
static int g_w2sMethodTag = 0; /* debug: which memory path won */

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

static bool matrix_finite(const Matrix4x4 &mat) {
    for (float v : mat.m) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

static bool matrix_w2s_impl(const Matrix4x4 &vp, float wx, float wy, float wz, float sw, float sh,
                            float &outX, float &outY) {
    Vec3 p{wx, wy, wz};
    float cx, cy, cz, cw;
    vp.multiply_point(p, cx, cy, cz, cw);
    if (cw <= 0.001f) return false;
    const float ndcX = cx / cw;
    const float ndcY = cy / cw;
    outX = (ndcX + 1.f) * 0.5f * sw;
    outY = (1.f - ndcY) * 0.5f * sh;
    return std::isfinite(outX) && std::isfinite(outY);
}

static bool build_vp_from_pos_quat(const Vec3 &pos, const Quat &rot, int fovDeg, float aspect,
                                   Matrix4x4 &out) {
    if (fovDeg < 10 || fovDeg > 120) fovDeg = 40;
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

    const float nearP = 0.3f, farP = 2000.f;
    const float t = 1.f / std::tan((float)fovDeg * 0.5f * 3.14159265f / 180.f);
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
    out = vp;
    return matrix_finite(out);
}

static bool read_sbc_fov(pid_t pid, int &fov) {
    fov = 40;
    if (!g_sbcStatics) return false;
    mem_read(pid, g_sbcStatics + 0x8C, fov);
    if (fov < 10 || fov > 120) fov = 40;
    return true;
}

static bool read_smooth_follow(pid_t pid, uintptr_t &smooth, Vec3 &pos, Quat &rot) {
    smooth = 0;
    if (!g_gmStatics) return false;
    if (!mem_read(pid, g_gmStatics + 0x10, smooth) || !mem_ptr_ok(smooth)) return false;
    if (!mem_read(pid, smooth + 0x1E4, pos)) return false;
    if (!mem_read(pid, smooth + 0x7C, rot)) return false;
    return std::isfinite(pos.x) && std::isfinite(rot.w);
}

static bool read_sbc_camera(pid_t pid, Vec3 &pos, Quat &rot) {
    if (!g_sbcStatics) return false;
    if (!mem_read(pid, g_sbcStatics + 0x10, pos)) return false;
    if (!mem_read(pid, g_sbcStatics + 0x58, rot)) return false;
    return std::isfinite(pos.x) && std::isfinite(rot.w);
}

static bool resolve_camera_object(pid_t pid, uintptr_t &camObj) {
    camObj = 0;
    if (!g_gmStatics) return false;
    mem_read(pid, g_gmStatics + 0x40, camObj); /* currentCameraInFight */
    if (mem_ptr_ok(camObj)) return true;
    mem_read(pid, g_gmStatics + 0x8, camObj); /* m_Camra */
    if (mem_ptr_ok(camObj)) return true;
    uintptr_t smooth = 0;
    if (mem_read(pid, g_gmStatics + 0x10, smooth) && mem_ptr_ok(smooth))
        mem_read(pid, smooth + 0x48, camObj); /* m_MainCamera */
    return mem_ptr_ok(camObj);
}

static bool matrix_looks_like_clip(const Matrix4x4 &m) {
    if (!matrix_finite(m)) return false;
    float sum = 0.f;
    for (float v : m.m) sum += std::fabs(v);
    if (sum < 0.05f) return false;
    if (std::fabs(m.m[15]) > 2.f && m.m[15] != 0.f) return false;
    return true;
}

static float calibrate_vp_score(const Matrix4x4 &vp, float rx, float ry, float rz, float sw,
                                float sh) {
    float ox = 0.f, oy = 0.f;
    if (!matrix_w2s_impl(vp, rx, ry, rz, sw, sh, ox, oy)) return 1e6f;
    const float ex = sw * 0.5f;
    const float ey = sh * 0.78f;
    return std::fabs(ox - ex) + std::fabs(oy - ey);
}

struct VpCandidate {
    Matrix4x4 vp{};
    int tag = 0;
};

static bool try_native_clip_matrix(pid_t pid, float rx, float ry, float rz, bool haveRef,
                                   Matrix4x4 &out, int &tag) {
    uintptr_t camObj = 0;
    if (!resolve_camera_object(pid, camObj)) return false;
    uintptr_t native = 0;
    if (!mem_read(pid, camObj + 0x10, native) || native < 0x10000) return false;

    float bestScore = 1e6f;
    Matrix4x4 best{};
    bool found = false;

    for (uintptr_t off = 0x80; off <= 0x720; off += 0x10) {
        Matrix4x4 cand{};
        if (!mem_read(pid, native + off, cand)) continue;
        if (!matrix_looks_like_clip(cand)) continue;

        if (haveRef) {
            const float sc = calibrate_vp_score(cand, rx, ry, rz, g_screenW, g_screenH);
            if (sc < bestScore) {
                bestScore = sc;
                best = cand;
                found = true;
            }
        } else if (std::fabs(cand.m[11]) > 0.5f || std::fabs(cand.m[10]) > 0.01f) {
            best = cand;
            found = true;
            break;
        }
    }

    if (!found) return false;
    if (haveRef && bestScore > 420.f) return false;
    out = best;
    tag = 4;
    return true;
}

static bool pick_memory_vp(pid_t pid, float rx, float ry, float rz, bool haveRef) {
    const float aspect =
        (g_screenW > 0.f && g_screenH > 0.f) ? (g_screenW / g_screenH) : (16.f / 9.f);

    int fov = 40;
    read_sbc_fov(pid, fov);

    Vec3 sbcPos{}, sfPos{};
    Quat sbcRot{}, sfRot{};
    const bool haveSbc = read_sbc_camera(pid, sbcPos, sbcRot);
    uintptr_t smooth = 0;
    const bool haveSf = read_smooth_follow(pid, smooth, sfPos, sfRot);

    VpCandidate cands[6];
    int n = 0;

    auto push = [&](int tag, const Vec3 &p, const Quat &q) {
        if (n >= 6) return;
        if (build_vp_from_pos_quat(p, q, fov, aspect, cands[n].vp)) {
            cands[n].tag = tag;
            ++n;
        }
    };

    if (haveSf) push(1, sfPos, sfRot);
    if (haveSbc) push(2, sbcPos, sbcRot);
    if (haveSf && haveSbc) {
        push(3, sfPos, sbcRot);
        push(5, sbcPos, sfRot);
    }

    Matrix4x4 nativeVp{};
    int nativeTag = 0;
    if (try_native_clip_matrix(pid, rx, ry, rz, haveRef, nativeVp, nativeTag) && n < 6) {
        cands[n].vp = nativeVp;
        cands[n].tag = nativeTag;
        ++n;
    }

    if (n == 0) return false;

    int pick = 0;
    if (haveRef && std::isfinite(rx) && std::isfinite(rz) &&
        (std::fabs(rx) > 0.5f || std::fabs(rz) > 0.5f)) {
        float best = 1e6f;
        for (int i = 0; i < n; ++i) {
            const float sc = calibrate_vp_score(cands[i].vp, rx, ry, rz, g_screenW, g_screenH);
            if (sc < best) {
                best = sc;
                pick = i;
            }
        }
        if (best > 480.f && haveSf) {
            for (int i = 0; i < n; ++i) {
                if (cands[i].tag == 1) {
                    pick = i;
                    break;
                }
            }
        }
    } else {
        for (int i = 0; i < n; ++i) {
            if (cands[i].tag == 1) {
                pick = i;
                break;
            }
        }
    }

    g_viewProj = cands[pick].vp;
    g_w2sMethodTag = cands[pick].tag;
    return true;
}

static bool matrix_w2s(float wx, float wy, float wz, float &outX, float &outY) {
    if (!g_matrixReady) return false;
    return matrix_w2s_impl(g_viewProj, wx, wy, wz, g_screenW, g_screenH, outX, outY);
}

} // namespace

void esp_camera_reset() {
    g_sbcStatics = 0;
    g_gmStatics = 0;
    g_matrixReady = false;
    g_w2sMethodTag = 0;
}

void esp_camera_set_static_bases(uintptr_t showBattleCameraStatics, uintptr_t gameMethodStatics) {
    g_sbcStatics = showBattleCameraStatics;
    g_gmStatics = gameMethodStatics;
}

void esp_camera_set_il2cpp_w2s(pid_t, uintptr_t, uintptr_t, uintptr_t) {
    /* IL2 ptrace W2S disabled — memory-only path (UnityKillsMe stability). */
}

void esp_camera_frame_begin(pid_t pid, float screenW, float screenH, float refWorldX,
                            float refWorldY, float refWorldZ, bool haveRefWorldPos) {
    g_screenW = screenW;
    g_screenH = screenH;
    normalize_landscape(g_screenW, g_screenH);
    g_matrixReady = false;
    g_w2sMethodTag = 0;

    if (pid > 0 && proc_alive(pid)) {
        if (pick_memory_vp(pid, refWorldX, refWorldY, refWorldZ, haveRefWorldPos))
            g_matrixReady = true;
    }
}

void esp_camera_frame_end() {}

bool esp_camera_ready() { return g_matrixReady; }

bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY) {
    return matrix_w2s(wx, wy, wz, outX, outY);
}
