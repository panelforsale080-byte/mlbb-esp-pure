#include "esp_retri.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"
#include "touch_helper.hpp"
#include "ui_log.hpp"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

static EspRetriSettings g_retri{};
static std::atomic<bool> g_touchInited{false};
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

static void ensure_touch_device() {
    if (g_touchInited.load()) return;
    const int w = g_dispW.load();
    const int h = g_dispH.load();
    if (w < 1 || h < 1) return;
    if (Touch_Init(w, h, 0, false)) {
        g_touchInited.store(true);
        __android_log_print(ANDROID_LOG_INFO, "MlbbEsp", "[retri] Touch_Init OK %dx%d", w, h);
    }
}

static void fire_retri_tap() {
    const int x = (int)g_retri.tapX;
    const int y = (int)g_retri.tapY;
    ensure_touch_device();
    if (g_touchInited.load()) {
        Touch_Tap(x, y);
        return;
    }
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "input tap %d %d 2>/dev/null", x, y);
    (void)std::system(cmd);
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
        if (g_touchInited.load()) UpdateScreenData(w, h, 0);
    }
}

void esp_retri_process(const EspFrame &frame) {
    if (!g_retri.enabled) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0) return;
    if (!esp_runtime_off_ready()) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.entityId || !R.hp) return;

    static uint64_t s_lastTapMs = 0;
    const uint64_t now = steady_ms();
    if (now - s_lastTapMs < 500) return;

    const int playerLevel = read_player_level(frame, R);
    const int retriDmg = calculate_retri_damage(playerLevel);
    constexpr float kMaxDist = 8.0f;

    uintptr_t closestEnt = 0;
    float closestDist = 1e9f;
    int closestHp = 0;

    for (const EspTarget &t : frame.targets) {
        if (t.kind != 1 || !t.ent) continue;
        int hid = 0;
        if (!mem_read(frame.pid, t.ent + R.entityId, hid) || !id_matches_external(hid, g_retri)) continue;
        bool dead = false;
        if (R.entityDeath && mem_read(frame.pid, t.ent + R.entityDeath, dead) && dead) continue;
        int hp = t.hp > 0 ? t.hp : 0;
        if (hp <= 0 && !mem_read(frame.pid, t.ent + R.hp, hp)) continue;
        if (hp <= 0) continue;
        const float dist = world_dist_xz(frame, t);
        if (dist < closestDist) {
            closestDist = dist;
            closestEnt = t.ent;
            closestHp = hp;
        }
    }

    static uint64_t s_lastDbgMs = 0;
    if (now - s_lastDbgMs > 3000) {
        s_lastDbgMs = now;
        ui_log("[retri] lvl=%d dmg=%d off_level=0x%llx tap=(%.0f,%.0f) mobs_in_frame=%d",
               playerLevel, retriDmg, (unsigned long long)R.level, g_retri.tapX, g_retri.tapY,
               frame.dictMonsters);
    }

    if (!closestEnt || closestDist > kMaxDist) return;
    if (closestHp <= 0 || closestHp > retriDmg) return;

    int realHp = 0;
    if (!mem_read(frame.pid, closestEnt + R.hp, realHp) || realHp <= 0 || realHp > retriDmg) return;

    fire_retri_tap();
    ui_log("[retri] TAP hp=%d dist=%.1f", realHp, closestDist);
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
