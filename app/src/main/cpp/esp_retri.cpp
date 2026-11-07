#include "esp_retri.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"

#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

static EspRetriSettings g_retri{};

static inline int retri_damage_for_level(int lv) {
    if (lv < 1) lv = 1;
    if (lv > 30) lv = 30;
    if (lv < 4) return 520 + 80 * lv;
    return (int)(1.5f * (float)(520 + 80 * lv));
}

static inline bool id_is_target(int id, const EspRetriSettings &s) {
    if (s.targetLord && id == 2002) return true;
    if (s.targetTurtle && (id == 2003 || id == 2110)) return true;
    if (s.targetBlue && (id == 2005 || id == 2221)) return true;
    if (s.targetLito && (id == 2056 || id == 2072)) return true;
    if (s.targetCrab && (id == 2222 || id == 2223)) return true;
    if (s.targetRed && (id == 2004 || id == 2220)) return true;
    return false;
}

static inline float world_dist_xz(const EspFrame &f, const EspTarget &t) {
    const float dx = t.worldX - f.selfX;
    const float dz = t.worldZ - f.selfZ;
    return std::sqrt(dx * dx + dz * dz);
}

static uint64_t steady_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

} // namespace

EspRetriSettings &esp_retri_settings() { return g_retri; }

void esp_retri_fire_tap(float x, float y) {
    esp_retri_jni_tap(x, y);
}

void esp_retri_process(const EspFrame &frame) {
    if (!g_retri.enabled) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0 || !frame.localPlayerEnt) return;
    if (!esp_runtime_off_ready()) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.entityId || !R.hp) return;

    static uint64_t s_lastTapMs = 0;
    const uint64_t now = steady_ms();
    if (now - s_lastTapMs < 450) return;

    const int retriDmg = retri_damage_for_level(g_retri.assumedLevel);

    uintptr_t closestEnt = 0;
    float closestDist = 1e9f;
    int closestHp = 0;

    for (const EspTarget &t : frame.targets) {
        if (t.kind != 1 || !t.ent) continue;
        int hid = 0;
        if (!mem_read(frame.pid, t.ent + R.entityId, hid) || !id_is_target(hid, g_retri)) continue;
        const float dist = world_dist_xz(frame, t);
        int hp = 0;
        if (!mem_read(frame.pid, t.ent + R.hp, hp)) continue;
        if (hp <= 0) continue;
        if (dist < closestDist) {
            closestDist = dist;
            closestEnt = t.ent;
            closestHp = hp;
        }
    }

    if (!closestEnt || closestDist > 8.f) return;
    if (closestHp <= 0 || closestHp > retriDmg) return;

    int realHp = 0;
    if (!mem_read(frame.pid, closestEnt + R.hp, realHp)) return;
    if (realHp <= 0 || realHp > retriDmg) return;

    esp_retri_fire_tap(g_retri.tapX, g_retri.tapY);
    s_lastTapMs = now;
    __android_log_print(ANDROID_LOG_INFO, "MlbbEsp",
                        "[retri] TAP hp=%d dmg=%d dist=%.2f @(%.0f,%.0f)", realHp, retriDmg,
                        closestDist, g_retri.tapX, g_retri.tapY);
}

void esp_retri_draw_indicator(ImDrawList *draw) {
    if (!draw || !g_retri.showCircle) return;
    draw->AddCircleFilled(ImVec2(g_retri.tapX, g_retri.tapY), 18.f, IM_COL32(255, 255, 255, 180), 16);
    draw->AddCircle(ImVec2(g_retri.tapX, g_retri.tapY), 20.f, IM_COL32(255, 120, 80, 220), 16, 2.f);
}
