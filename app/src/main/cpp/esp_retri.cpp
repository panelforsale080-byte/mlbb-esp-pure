#include "esp_retri.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"

#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

static EspRetriSettings g_retri{};

/** VIP SoliceRetri.h — simplified (broken ternary in original). */
static int calculate_retri_damage(int level, int killWildTimes) {
    if (level < 1) level = 1;
    if (level > 30) level = 30;
    if (killWildTimes < 5) return 530 + 80 * level;
    return (int)(1.5f * (float)(530 + 80 * level));
}

static inline bool id_matches_vip(int id, const EspRetriSettings &s) {
    if (s.lord && id == 2002) return true;
    if (s.turtle && id == 2003) return true;
    if (s.buff && (id == 2004 || id == 2005)) return true;
    if (s.litho && (id == 2056 || id == 2072)) return true;
    if (s.crab && (id == 2011 || id == 2013)) return true;
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

static void touch_tap_screen(int x, int y) {
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "input tap %d %d 2>/dev/null", x, y);
    (void)std::system(cmd);
}

} // namespace

EspRetriSettings &esp_retri_settings() { return g_retri; }

void esp_retri_process(const EspFrame &frame) {
    if (!g_retri.enabled) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0 || !frame.localPlayerEnt) return;
    if (!esp_runtime_off_ready()) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.entityId || !R.hp || !R.hpMax) return;

    static uint64_t s_lastTapMs = 0;
    const uint64_t now = steady_ms();
    if (now - s_lastTapMs < 450) return;

    const int retriDmg = calculate_retri_damage(g_retri.assumedLevel, g_retri.assumedKillWildTimes);
    constexpr float kMaxDist = 5.4f; /* VIP UpdateRetribution */

    uintptr_t bestEnt = 0;
    float bestDist = 1e9f;
    int bestHp = 0;

    for (const EspTarget &t : frame.targets) {
        if (t.kind != 1 || !t.ent) continue;
        int hid = 0;
        if (!mem_read(frame.pid, t.ent + R.entityId, hid) || !id_matches_vip(hid, g_retri)) continue;
        int curHp = 0, maxHp = 0;
        if (!mem_read(frame.pid, t.ent + R.hp, curHp)) continue;
        if (!mem_read(frame.pid, t.ent + R.hpMax, maxHp)) continue;
        if (curHp <= 0 || curHp >= maxHp) continue;
        const float dist = world_dist_xz(frame, t);
        if (dist >= kMaxDist) continue;
        if (curHp > retriDmg) continue;
        if (dist < bestDist) {
            bestDist = dist;
            bestEnt = t.ent;
            bestHp = curHp;
        }
    }

    if (!bestEnt) return;
    int realHp = 0;
    if (!mem_read(frame.pid, bestEnt + R.hp, realHp)) return;
    if (realHp <= 0 || realHp > retriDmg) return;

    touch_tap_screen((int)g_retri.tapX, (int)g_retri.tapY);
    s_lastTapMs = now;
    __android_log_print(ANDROID_LOG_INFO, "MlbbEsp", "[retri] tap hp=%d dmg=%d dist=%.2f", realHp,
                        retriDmg, bestDist);
}

void esp_retri_draw_indicator(ImDrawList *draw) {
    if (!draw || !g_retri.showCircle) return;
    draw->AddCircleFilled(ImVec2(g_retri.tapX, g_retri.tapY), 18.f, IM_COL32(255, 255, 255, 180), 16);
}
