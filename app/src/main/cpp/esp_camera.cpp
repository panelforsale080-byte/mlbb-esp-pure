#include "esp_camera.hpp"
#include "mem.hpp"
#include "offsets.hpp"
#include "ui_log.hpp"
#include <cmath>
#include <cstring>
#include <vector>

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
static bool g_classesSearched = false;
static bool g_camReady = false;
static Matrix4x4 g_viewProj;
static float g_screenW = 0.f;
static float g_screenH = 0.f;

static bool name_at(pid_t pid, uintptr_t p, const char *want) {
    char buf[80] = {};
    if (!mem_read(pid, p, buf, sizeof(buf) - 1)) return false;
    return std::strcmp(buf, want) == 0;
}

static bool statics_for(pid_t pid, uintptr_t klass, uintptr_t &statics) {
    for (uintptr_t off : Off::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!mem_read(pid, klass + off, st) || !mem_ptr_ok(st)) continue;
        statics = st;
        return true;
    }
    return false;
}

static bool klass_matches_name(pid_t pid, uintptr_t klass, const char *name,
                               const std::vector<uintptr_t> &strs) {
    static const uintptr_t kOffs[] = {0x10, 0x18, 0x08, 0x20, 0x28, 0x30, 0x38};
    for (uintptr_t off : kOffs) {
        uintptr_t np = 0;
        if (!mem_read(pid, klass + off, np) || !mem_ptr_ok(np)) continue;
        for (uintptr_t s : strs)
            if (np == s) return true;
        if (name_at(pid, np, name)) return true;
    }
    return false;
}

static void scan_strings(pid_t pid, const std::vector<MapsReg> &maps, const char *needle,
                         std::vector<uintptr_t> &out) {
    out.clear();
    const size_t nlen = std::strlen(needle) + 1;
    for (const auto &m : maps) {
        if (m.perms[0] != 'r' || m.end <= m.start) continue;
        const size_t sz = (size_t)(m.end - m.start);
        if (sz < 16 || sz > 64 * 1024 * 1024) continue;
        const size_t step = 512 * 1024;
        for (size_t off = 0; off < sz && out.size() < 24; off += step) {
            const size_t n = std::min(step + nlen, sz - off);
            std::vector<char> buf(n);
            if (!mem_read(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + nlen <= n; ++i) {
                if (std::memcmp(buf.data() + i, needle, nlen) == 0)
                    out.push_back(m.start + off + i);
            }
        }
    }
}

static bool find_class_statics(pid_t pid, const char *name, uintptr_t &staticsOut) {
    staticsOut = 0;
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return false;
    std::vector<uintptr_t> strs;
    scan_strings(pid, maps, name, strs);
    if (strs.empty()) return false;

    for (const auto &m : maps) {
        if (m.perms[0] != 'r' || m.perms[1] != 'w') continue;
        const size_t sz = (size_t)(m.end - m.start);
        if (sz > 24 * 1024 * 1024) continue;
        const size_t chunk = 256 * 1024;
        for (size_t off = 0; off < sz; off += chunk) {
            const size_t n = std::min(chunk, sz - off);
            std::vector<uint8_t> buf(n);
            if (!mem_read(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t p = 0;
                std::memcpy(&p, buf.data() + i, 8);
                bool hit = false;
                for (uintptr_t s : strs)
                    if (p == s) {
                        hit = true;
                        break;
                    }
                if (!hit) continue;
                const uintptr_t cand = m.start + off + i - Off::KlassName;
                if (!klass_matches_name(pid, cand, name, strs)) continue;
                uintptr_t st = 0;
                if (statics_for(pid, cand, st)) {
                    staticsOut = st;
                    return true;
                }
            }
        }
    }
    return false;
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

    const float aspect =
        (g_screenW > 0.f && g_screenH > 0.f) ? (g_screenW / g_screenH) : (16.f / 9.f);
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

} // namespace

void esp_camera_reset() {
    g_sbcStatics = 0;
    g_gmStatics = 0;
    g_classesSearched = false;
    g_camReady = false;
}

void esp_camera_update(pid_t pid, float screenW, float screenH) {
    g_screenW = screenW;
    g_screenH = screenH;
    g_camReady = false;
    if (pid <= 0 || screenW < 1.f || screenH < 1.f) return;

    if (!g_classesSearched) {
        g_classesSearched = true;
        find_class_statics(pid, "ShowBattleCamera", g_sbcStatics);
        find_class_statics(pid, "GameMethod", g_gmStatics);
        ui_log("[esp] camera statics sbc=0x%llx gm=0x%llx",
               (unsigned long long)g_sbcStatics, (unsigned long long)g_gmStatics);
    }

    if (load_vp_game_method(pid))
        g_camReady = true;
    else if (load_vp_show_battle_camera(pid))
        g_camReady = true;
}

bool esp_camera_ready() { return g_camReady; }

bool esp_camera_world_to_screen(float wx, float wy, float wz, float &outX, float &outY) {
    if (!g_camReady || g_screenW <= 0.f || g_screenH <= 0.f) return false;
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
