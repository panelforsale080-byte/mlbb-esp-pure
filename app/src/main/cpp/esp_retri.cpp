#include "esp_retri.hpp"
#include "esp_engine.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"
#include "ui_log.hpp"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

static EspRetriSettings g_retri{};
static std::atomic<int> g_dispW{0};
static std::atomic<int> g_dispH{0};

/** EXTERNAL-MLBB main.cpp CalculateRetriDamage */
static int calculate_retri_damage(int level) {
    if (level < 4) return 520 + 80 * level;
    return (int)(1.5 * (double)(520 + 80 * level));
}

static inline bool id_matches_external(int id, const EspRetriSettings &s) {
    if (s.targetLord && id == 2002) return true;
    if (s.targetTurtle && (id == 2003 || id == 2110)) return true;
    if (s.targetBlue && (id == 2005 || id == 2221)) return true;
    if (s.targetLito && id == 2056) return true;
    if (s.targetCrab && (id == 2223 || id == 2222)) return true;
    if (s.targetRed && (id == 2004 || id == 2220)) return true;
    return false;
}

static uint64_t steady_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

/** Overlay APK: never Touch_Init (EVIOCGRAB + input threads). Crashes menu / steals touch. */
static void fire_retri_tap() {
    const int x = (int)g_retri.tapX;
    const int y = (int)g_retri.tapY;
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "input tap %d %d 2>/dev/null", x, y);
    (void)std::system(cmd);
}

static int read_entity_hp(pid_t pid, uintptr_t ent, uintptr_t hpOff) {
    int64_t hp64 = 0;
    if (mem_read(pid, ent + hpOff, hp64) && hp64 > 0 && hp64 < 10000000) return (int)hp64;
    int hp32 = 0;
    if (mem_read(pid, ent + hpOff, hp32) && hp32 > 0) return hp32;
    return 0;
}

static int read_player_level(const EspFrame &frame, const EspRuntimeOff &R) {
    if (!frame.localPlayerEnt || !R.level) return g_retri.assumedLevel;
    int lv = 0;
    if (mem_read(frame.pid, frame.localPlayerEnt + R.level, lv) && lv >= 1 && lv <= 20) return lv;
    return g_retri.assumedLevel;
}

static void update_norm_from_pixels(float sw, float sh) {
    if (sw > 1.f) g_retri.tapNormX = g_retri.tapX / sw;
    if (sh > 1.f) g_retri.tapNormY = g_retri.tapY / sh;
}

/** EXTERNAL ProcessAutoRetribution: scan full monster list, not ESP target cap. */
static bool find_closest_retri_target(const EspFrame &frame, const EspRuntimeOff &R, float selfX,
                                      float selfY, float selfZ, uintptr_t &outEnt,
                                      float &outDist, int &outHp, int &outId) {
    outEnt = 0;
    outDist = 1e9f;
    outHp = 0;
    outId = 0;
    if (!frame.bmInstance) return false;

    std::vector<std::pair<int, uintptr_t>> monsters;
    if (!esp_monster_dict_entries(frame.pid, frame.bmInstance, monsters)) return false;

    for (const auto &kv : monsters) {
        const uintptr_t ent = kv.second;
        if (!ent) continue;
        uint8_t dead = 0;
        if (mem_read(frame.pid, ent + R.entityDeath, dead) && dead) continue;
        int hid = 0;
        if (!mem_read(frame.pid, ent + R.entityId, hid) || !id_matches_external(hid, g_retri))
            continue;
        float mx = 0, my = 0, mz = 0;
        if (!esp_read_entity_world(frame.pid, ent, mx, my, mz)) continue;
        const float dx = mx - selfX;
        const float dy = my - selfY;
        const float dz = mz - selfZ;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int hp = read_entity_hp(frame.pid, ent, R.hp);
        if (hp <= 0) continue;
        if (dist < outDist) {
            outDist = dist;
            outEnt = ent;
            outHp = hp;
            outId = hid;
        }
    }
    return outEnt != 0;
}

} // namespace

EspRetriSettings &esp_retri_settings() { return g_retri; }

void esp_retri_set_default_spell_anchor() {
    g_retri.useScreenAnchor = true;
    g_retri.tapNormX = 0.65625f;
    g_retri.tapNormY = 0.612f;
    const float sw = (float)g_dispW.load();
    const float sh = (float)g_dispH.load();
    if (sw > 1.f && sh > 1.f) esp_retri_sync_anchor_pixels(sw, sh);
}

void esp_retri_sync_anchor_pixels(float screenW, float screenH) {
    if (!g_retri.useScreenAnchor || screenW < 1.f || screenH < 1.f) return;
    g_retri.tapX = g_retri.tapNormX * screenW;
    g_retri.tapY = g_retri.tapNormY * screenH;
}

void esp_retri_notify_display(int w, int h) {
    if (w > 0 && h > 0) {
        g_dispW.store(w);
        g_dispH.store(h);
        esp_retri_sync_anchor_pixels((float)w, (float)h);
    }
}

void esp_retri_process(const EspFrame &frame) {
    if (!g_retri.enabled) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0) return;
    if (!esp_runtime_off_ready()) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.entityId || !R.hp) return;

    static uint64_t s_lastScanMs = 0;
    static uint64_t s_lastTapMs = 0;
    const uint64_t now = steady_ms();
    if (now - s_lastScanMs < 120) return;
    s_lastScanMs = now;
    if (now - s_lastTapMs < 500) return;

    const int playerLevel = read_player_level(frame, R);
    const int retriDmg = calculate_retri_damage(playerLevel);
    constexpr float kMaxDist = 8.0f;

    uintptr_t closestEnt = 0;
    float closestDist = 1e9f;
    int closestHp = 0;
    int closestId = 0;
    find_closest_retri_target(frame, R, frame.selfX, frame.selfY, frame.selfZ, closestEnt,
                              closestDist, closestHp, closestId);

    static uint64_t s_lastDbgMs = 0;
    if (now - s_lastDbgMs > 3000) {
        s_lastDbgMs = now;
        if (closestEnt) {
            ui_log("[retri] lvl=%d dmg=%d cand id=%d hp=%d dist=%.2f (need hp<=%d dist<=%.0f)",
                   playerLevel, retriDmg, closestId, closestHp, closestDist, retriDmg, kMaxDist);
        } else {
            ui_log("[retri] lvl=%d dmg=%d — no retri mob in dict (check Targets toggles)",
                   playerLevel, retriDmg);
        }
    }

    if (!closestEnt || closestDist > kMaxDist) return;
    if (closestHp <= 0 || closestHp > retriDmg) return;

    const int realHp = read_entity_hp(frame.pid, closestEnt, R.hp);
    if (realHp <= 0 || realHp > retriDmg) return;

    fire_retri_tap();
    ui_log("[retri] TAP id=%d hp=%d dist=%.1f", closestId, realHp, closestDist);
    s_lastTapMs = now;
}

void esp_retri_draw_indicator(ImDrawList *draw) {
    if (!draw || !g_retri.showCircle) return;
    const ImVec2 p(g_retri.tapX, g_retri.tapY);
    draw->AddCircleFilled(p, 18.f, IM_COL32(255, 255, 255, 180), 16);
    draw->AddCircle(p, 22.f, IM_COL32(120, 200, 255, 200), 16, 2.f);
}

void esp_retri_anchor_drag(bool menuOpen, float screenW, float screenH) {
    if (!menuOpen || !g_retri.showCircle) return;
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        const float dx = io.MousePos.x - g_retri.tapX;
        const float dy = io.MousePos.y - g_retri.tapY;
        const float r = 28.f;
        if (dx * dx + dy * dy <= r * r && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_retri.tapX = io.MousePos.x;
            g_retri.tapY = io.MousePos.y;
            g_retri.useScreenAnchor = true;
            update_norm_from_pixels(screenW, screenH);
        }
    }
}
