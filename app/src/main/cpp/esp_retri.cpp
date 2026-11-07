#include "esp_retri.hpp"
#include "esp_engine.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"
#include "esp_retri_il2.hpp"
#include "touch_uinput.hpp"
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

/** In-game punish table: 750 + 150*Lv (Lv1=900 … Lv15=3000). */
static int calculate_retri_damage(int level) {
    int lv = level;
    if (lv < 1) lv = 1;
    if (lv > 15) lv = 15;
    return 750 + (150 * lv);
}

static int read_local_level(pid_t pid, uintptr_t localEnt, uintptr_t levelOff, int fallback) {
    if (!localEnt || !levelOff) return fallback;
    int lv = 0;
    if (mem_read(pid, localEnt + levelOff, lv) && lv >= 1 && lv <= 20) return lv;
    return fallback;
}

static bool read_int_field(pid_t pid, uintptr_t base, uintptr_t off, int &out) {
    if (!base || !off) return false;
    int v = 0;
    if (!mem_read(pid, base + off, v) || v < 0 || v > 5000) return false;
    out = v;
    return true;
}

static bool read_kill_wild_times(pid_t pid, uintptr_t localEnt, const EspRuntimeOff &R, int &outKw) {
    outKw = 0;
    if (!localEnt) return false;

    if (R.showKillWildTimes && read_int_field(pid, localEnt, R.showKillWildTimes, outKw))
        return true;

    if (!R.logicFighter || !R.logicKillWildTimes) return false;
    uintptr_t logic = 0;
    if (!mem_read(pid, localEnt + R.logicFighter, logic) || !mem_ptr_ok(logic)) return false;
    return read_int_field(pid, logic, R.logicKillWildTimes, outKw);
}

static int resolve_retri_damage(const EspFrame &frame, const EspRuntimeOff &R, int &outLevel,
                                int &outKillWild, bool &outHaveKw) {
    outHaveKw = read_kill_wild_times(frame.pid, frame.localPlayerEnt, R, outKillWild);
    outLevel = read_local_level(frame.pid, frame.localPlayerEnt, R.level, g_retri.assumedLevel);
    return calculate_retri_damage(outLevel);
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

static void fire_retri_tap() {
    const int x = (int)g_retri.tapX;
    const int y = (int)g_retri.tapY;
    const int sw = g_dispW.load() > 0 ? g_dispW.load() : (int)g_retri.tapX + 1;
    const int sh = g_dispH.load() > 0 ? g_dispH.load() : (int)g_retri.tapY + 1;
    const char *how = touch_inject_screen_tap(x, y, sw, sh);
    ui_log("[retri] inject %s @ %d,%d", how, x, y);
}

static int read_entity_hp(pid_t pid, uintptr_t ent, uintptr_t hpOff) {
    int64_t hp64 = 0;
    if (mem_read(pid, ent + hpOff, hp64) && hp64 > 0 && hp64 < 10000000) return (int)hp64;
    int hp32 = 0;
    if (mem_read(pid, ent + hpOff, hp32) && hp32 > 0) return hp32;
    return 0;
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

struct CastBusyGuard {
    std::atomic<bool> &busy;
    explicit CastBusyGuard(std::atomic<bool> &b) : busy(b) {}
    ~CastBusyGuard() { busy.store(false, std::memory_order_release); }
};

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
        (void)touch_uinput_open(w, h);
    }
}

void esp_retri_process(const EspFrame &frame) {
    if (!g_retri.enabled) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0) return;
    if (!esp_runtime_off_ready()) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.entityId || !R.hp) return;

    static std::atomic<bool> s_castBusy{false};
    const uint64_t now = steady_ms();

    int playerLevel = 0;
    int killWild = 0;
    bool haveKw = false;
    const int retriDmg = resolve_retri_damage(frame, R, playerLevel, killWild, haveKw);
    g_retri.liveRetriDamage = retriDmg;
    g_retri.livePlayerLevel = playerLevel;
    g_retri.liveKillWildTimes = haveKw ? killWild : -1;
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
            ui_log("[retri] lvl=%d kw=%d dmg=%d cand id=%d hp=%d dist=%.2f (hp<=%d)",
                   playerLevel, haveKw ? killWild : -1, retriDmg, closestId, closestHp,
                   closestDist, retriDmg);
        } else {
            ui_log("[retri] lvl=%d kw=%d dmg=%d — no retri mob (Targets?)",
                   playerLevel, haveKw ? killWild : -1, retriDmg);
        }
    }

    if (!closestEnt || closestDist > kMaxDist) return;
    if (closestHp <= 0 || closestHp > retriDmg) return;

    const int realHp = read_entity_hp(frame.pid, closestEnt, R.hp);
    if (realHp <= 0 || realHp > retriDmg) return;

    static uintptr_t s_lastCastEnt = 0;
    static int s_lastCastHp = 0;
    static uint64_t s_lastCastMs = 0;
    if (closestEnt == s_lastCastEnt && realHp == s_lastCastHp && now - s_lastCastMs < 250) return;

    if (s_castBusy.exchange(true, std::memory_order_acq_rel)) return;
    CastBusyGuard castGuard(s_castBusy);

    bool castOk = false;
    if (g_retri.useIl2SummonCast && esp_retri_il2_ready() && frame.localPlayerEnt) {
        float mx = 0, my = 0, mz = 0;
        if (esp_read_entity_world(frame.pid, closestEnt, mx, my, mz)) {
            float dx = mx - frame.selfX;
            float dy = my - frame.selfY;
            float dz = mz - frame.selfZ;
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len > 1e-4f) {
                dx /= len;
                dy /= len;
                dz /= len;
            } else {
                dx = 1.f;
                dy = 0.f;
                dz = 0.f;
            }
            castOk = esp_retri_il2_cast_summon(frame.pid, frame.localPlayerEnt, dx, dy, dz);
        }
        static uint64_t s_lastCastLogMs = 0;
        if (!castOk || now - s_lastCastLogMs > 200) {
            s_lastCastLogMs = now;
            ui_log("[retri] %s id=%d hp=%d dist=%.1f (IL2 TryUseSkill 20020)",
                   castOk ? "CAST" : "CAST-FAIL", closestId, realHp, closestDist);
        }
        if (!castOk)
            fire_retri_tap();
    } else {
        fire_retri_tap();
        castOk = true;
        ui_log("[retri] TAP id=%d hp=%d dist=%.1f (shell / uinput)", closestId, realHp, closestDist);
    }
    s_lastCastEnt = closestEnt;
    s_lastCastHp = realHp;
    s_lastCastMs = now;
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
