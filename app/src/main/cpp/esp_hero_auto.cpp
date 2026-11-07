#include "esp_hero_auto.hpp"
#include "esp_retri_il2.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"
#include "touch_uinput.hpp"
#include "ui_log.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

static EspHeroAutoSettings g_hero{};

EspHeroAutoSettings &esp_hero_auto_settings() { return g_hero; }

void esp_hero_auto_sync_skill_pixels(float screenW, float screenH) {
    if (screenW < 1.f || screenH < 1.f) return;
    if (g_hero.useSkillAnchorNorm) {
        g_hero.lingS2X = g_hero.lingS2NormX * screenW;
        g_hero.lingS2Y = g_hero.lingS2NormY * screenH;
        g_hero.gusionS1X = g_hero.gS1NormX * screenW;
        g_hero.gusionS1Y = g_hero.gS1NormY * screenH;
        g_hero.gusionS2X = g_hero.gS2NormX * screenW;
        g_hero.gusionS2Y = g_hero.gS2NormY * screenH;
        g_hero.gusionS3X = g_hero.gS3NormX * screenW;
        g_hero.gusionS3Y = g_hero.gS3NormY * screenH;
    }
}

void esp_hero_auto_reset_skill_anchors() {
    g_hero.useSkillAnchorNorm = true;
    g_hero.lingS2NormX = 0.85f;
    g_hero.lingS2NormY = 0.75f;
    g_hero.gS1NormX = 0.85f;
    g_hero.gS1NormY = 0.75f;
    g_hero.gS2NormX = 0.78f;
    g_hero.gS2NormY = 0.82f;
    g_hero.gS3NormX = 0.72f;
    g_hero.gS3NormY = 0.58f;
}

namespace {

static constexpr int kHeroLing = 84;
static constexpr int kHeroGusion = 56;
static constexpr int kLingSwordBulletId = 8452;
static constexpr int kLingSkill2Cast = 8420;
static constexpr int kGusionMarkBulletId = 5610;
static constexpr int kGusionS1 = 5610;
static constexpr int kGusionS2 = 5620;
static constexpr int kGusionS3 = 5630;

static uint64_t steady_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** EXTERNAL/VIP: ShowEntity.m_ID is hero id for players (<1000). */
static int read_local_hero_id(pid_t pid, uintptr_t localPlayer, const EspRuntimeOff &R) {
    if (!mem_ptr_ok(localPlayer)) return 0;
    int hid = 0;
    if (R.entityId && mem_read(pid, localPlayer + R.entityId, hid) && hid > 0 && hid < 1000)
        return hid;
    if (R.heroOriginId && mem_read(pid, localPlayer + R.heroOriginId, hid) && hid > 0) return hid;
    return 0;
}

static bool bullet_pos_ok(pid_t pid, uintptr_t bullet, uintptr_t bulPosOff, float &x, float &y,
                          float &z) {
    if (!bulPosOff) return false;
    float pos[3]{};
    if (!mem_read(pid, bullet + bulPosOff, pos, sizeof(pos))) return false;
    if (std::fabs(pos[0]) < 0.01f && std::fabs(pos[2]) < 0.01f) return false;
    x = pos[0];
    y = pos[1];
    z = pos[2];
    return true;
}

static void foreach_run_bullet(pid_t pid, uintptr_t bmStatics, const EspRuntimeOff &R,
                                 const std::function<bool(uintptr_t bullet)> &fn) {
    if (!bmStatics || !R.bulletId) return;

    auto scan_hashset = [&]() {
        if (!R.bmStaticRunBullets) return;
        uintptr_t hashSet = 0;
        if (!mem_read(pid, bmStatics + R.bmStaticRunBullets, hashSet) || !mem_ptr_ok(hashSet))
            return;
        uintptr_t entries = 0;
        if (!mem_read(pid, hashSet + 0x18, entries) || !mem_ptr_ok(entries)) return;
        int capacity = 0;
        if (!mem_read(pid, entries + 0x18, capacity) || capacity <= 0 || capacity > 2048) return;
        for (int i = 0; i < capacity; ++i) {
            const uintptr_t entryAddr = entries + 0x20 + (uintptr_t)i * 16;
            int hashCode = 0;
            mem_read(pid, entryAddr, hashCode);
            if (hashCode < 0) continue;
            uintptr_t bullet = 0;
            if (!mem_read(pid, entryAddr + 8, bullet) || !mem_ptr_ok(bullet)) continue;
            fn(bullet);
        }
    };

    auto scan_list = [&](uintptr_t staticOff) {
        if (!staticOff) return;
        uintptr_t listObj = 0;
        if (!mem_read(pid, bmStatics + staticOff, listObj) || !mem_ptr_ok(listObj)) return;
        std::vector<uintptr_t> items;
        if (!esp_il2_managed_list(pid, listObj, items)) return;
        for (uintptr_t bullet : items) {
            if (bullet) fn(bullet);
        }
    };

    scan_hashset();
    scan_list(R.bmStaticRunBulletsList);
}

static bool find_bullet_by_id(pid_t pid, uintptr_t bmStatics, const EspRuntimeOff &R, int wantId,
                              float selfX, float selfY, float selfZ, float maxRange, float *outX,
                              float *outY, float *outZ) {
    float bestD = maxRange;
    bool found = false;
    int seen = 0;
    foreach_run_bullet(pid, bmStatics, R, [&](uintptr_t bullet) {
        int bid = 0;
        if (!mem_read(pid, bullet + R.bulletId, bid)) return;
        if (bid != wantId) return;
        ++seen;
        float bx = 0, by = 0, bz = 0;
        if (!bullet_pos_ok(pid, bullet, R.bulletPos, bx, by, bz)) return;
        const float dx = bx - selfX;
        const float dy = by - selfY;
        const float dz = bz - selfZ;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > maxRange) return;
        if (d < bestD) {
            bestD = d;
            *outX = bx;
            *outY = by;
            *outZ = bz;
            found = true;
        }
    });
    g_hero.lastBulletScan = seen;
    return found;
}

static bool has_bullet_id(pid_t pid, uintptr_t bmStatics, const EspRuntimeOff &R, int wantId) {
    bool hit = false;
    foreach_run_bullet(pid, bmStatics, R, [&](uintptr_t bullet) {
        if (hit) return;
        int bid = 0;
        if (mem_read(pid, bullet + R.bulletId, bid) && bid == wantId) hit = true;
    });
    return hit;
}

static void norm_dir(float dx, float dy, float dz, float &ox, float &oy, float &oz) {
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len > 1e-4f) {
        ox = dx / len;
        oy = dy / len;
        oz = dz / len;
    } else {
        ox = 1.f;
        oy = 0.f;
        oz = 0.f;
    }
}

static std::atomic<bool> s_castBusy{false};

struct CastGuard {
    std::atomic<bool> &b;
    explicit CastGuard(std::atomic<bool> &x) : b(x) {}
    ~CastGuard() { b.store(false, std::memory_order_release); }
};

static void ling_touch_swipe(int scrW, int scrH, float tx, float tz, float selfX, float selfZ) {
    if (scrW < 1) scrW = 2400;
    if (scrH < 1) scrH = 1080;
    const float dx = tx - selfX;
    const float dz = tz - selfZ;
    const float angle = std::atan2(-dz, dx) - 0.7854f;
    const float moveX = g_hero.lingS2X + std::cos(angle) * 300.f;
    const float moveY = g_hero.lingS2Y + std::sin(angle) * 300.f;
    touch_inject_screen_swipe((int)g_hero.lingS2X, (int)g_hero.lingS2Y, (int)moveX, (int)moveY,
                              45, scrW, scrH);
}

static void process_ling(const EspFrame &frame, const EspRuntimeOff &R, int heroId, int scrW,
                         int scrH) {
    if (!g_hero.lingSword || heroId != kHeroLing) return;
    if (!R.bulletId || (!R.bmStaticRunBullets && !R.bmStaticRunBulletsList)) return;

    static uint64_t s_lastCastMs = 0;
    const uint64_t now = steady_ms();
    if (now - s_lastCastMs < 160) return;

    float tx = 0, ty = 0, tz = 0;
    if (!find_bullet_by_id(frame.pid, frame.bmStatics, R, kLingSwordBulletId, frame.selfX,
                           frame.selfY, frame.selfZ, g_hero.lingMaxRange, &tx, &ty, &tz))
        return;

    float dx = tx - frame.selfX;
    float dy = ty - frame.selfY;
    float dz = tz - frame.selfZ;
    float ndx, ndy, ndz;
    norm_dir(dx, dy, dz, ndx, ndy, ndz);
    const float dist =
        std::sqrt(dx * dx + dy * dy + dz * dz);

    bool ok = false;
    if (g_hero.useIl2Cast && esp_retri_il2_ready() && frame.localPlayerEnt) {
        if (!s_castBusy.exchange(true, std::memory_order_acq_rel)) {
            CastGuard guard(s_castBusy);
            ok = esp_il2_try_use_skill(frame.pid, frame.localPlayerEnt, kLingSkill2Cast, ndx, ndy,
                                       ndz, dist, 0.f, 0.f);
        }
    }
    if (!ok) ling_touch_swipe(scrW, scrH, tx, tz, frame.selfX, frame.selfZ);
    s_lastCastMs = now;
    static uint64_t s_log = 0;
    if (now - s_log > 2500) {
        s_log = now;
        ui_log("[ling] sword cast %s dist=%.1f bullets=%d", ok ? "IL2" : "swipe", dist,
               g_hero.lastBulletScan);
    }
}

static void process_gusion(const EspFrame &frame, const EspRuntimeOff &R, int heroId, int scrW,
                           int scrH) {
    if (!g_hero.gusionCombo || heroId != kHeroGusion) return;
    if (!R.bulletId || (!R.bmStaticRunBullets && !R.bmStaticRunBulletsList)) return;

    static int s_state = 0;
    static uint64_t s_lastActionMs = 0;
    const uint64_t now = steady_ms();
    const int delay = g_hero.comboDelayMs < 40 ? 40 : g_hero.comboDelayMs;
    if (now - s_lastActionMs < (uint64_t)delay) return;

    const bool markAlive = has_bullet_id(frame.pid, frame.bmStatics, R, kGusionMarkBulletId);
    float markX = frame.selfX, markY = frame.selfY, markZ = frame.selfZ;
    if (markAlive)
        find_bullet_by_id(frame.pid, frame.bmStatics, R, kGusionMarkBulletId, frame.selfX,
                          frame.selfY, frame.selfZ, 25.f, &markX, &markY, &markZ);

    const float dx = markX - frame.selfX;
    const float dz = markZ - frame.selfZ;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    float ndx, ndy, ndz;
    norm_dir(dx, 0.f, dz, ndx, ndy, ndz);

    auto il2_cast = [&](int skill) -> bool {
        if (!g_hero.useIl2Cast || !esp_retri_il2_ready() || !frame.localPlayerEnt) return false;
        if (s_castBusy.exchange(true, std::memory_order_acq_rel)) return false;
        CastGuard guard(s_castBusy);
        return esp_il2_try_use_skill(frame.pid, frame.localPlayerEnt, skill, ndx, ndy, ndz, markX,
                                     markY, markZ);
    };

    auto tap_skill = [&](float x, float y) {
        touch_inject_screen_tap((int)x, (int)y, scrW, scrH);
    };
    auto cast_or_tap = [&](int skill, float x, float y) {
        if (!il2_cast(skill)) tap_skill(x, y);
    };

    bool acted = false;
    if (s_state == 0) {
        if (markAlive) {
            cast_or_tap(kGusionS2, g_hero.gusionS2X, g_hero.gusionS2Y);
            cast_or_tap(kGusionS1, g_hero.gusionS1X, g_hero.gusionS1Y);
            s_state = 1;
            acted = true;
        }
    } else if (s_state == 1) {
        if (!markAlive || distXZ < 1.0f) {
            cast_or_tap(kGusionS2, g_hero.gusionS2X, g_hero.gusionS2Y);
            cast_or_tap(kGusionS3, g_hero.gusionS3X, g_hero.gusionS3Y);
            s_state = 2;
            acted = true;
        }
    } else if (s_state == 2) {
        if (markAlive) {
            cast_or_tap(kGusionS2, g_hero.gusionS2X, g_hero.gusionS2Y);
            cast_or_tap(kGusionS1, g_hero.gusionS1X, g_hero.gusionS1Y);
            s_state = 3;
            acted = true;
        } else {
            s_state = 0;
        }
    } else if (s_state == 3) {
        if (!markAlive || distXZ < 1.0f) {
            cast_or_tap(kGusionS2, g_hero.gusionS2X, g_hero.gusionS2Y);
            s_state = 0;
            acted = true;
        }
    }

    if (acted) s_lastActionMs = now;
}

} // namespace

void esp_hero_auto_process(const EspFrame &frame) {
    if (!g_hero.lingSword && !g_hero.gusionCombo) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0) return;
    if (!esp_runtime_off_ready()) return;
    if (!frame.bmStatics) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.bulletId) return;

    const int heroId = read_local_hero_id(frame.pid, frame.localPlayerEnt, R);
    g_hero.lastHeroId = heroId;

    static uint64_t s_dbg = 0;
    const uint64_t now = steady_ms();
    if (now - s_dbg > 4000) {
        s_dbg = now;
        ui_log("[hero] heroId=%d (need 84/56) m_RunBullets=0x%llx _RunBullets=0x%llx bullet=0x%llx",
               heroId, (unsigned long long)R.bmStaticRunBullets,
               (unsigned long long)R.bmStaticRunBulletsList, (unsigned long long)R.bulletId);
    }

    if (heroId != kHeroLing && heroId != kHeroGusion) return;

    int scrW = 2400, scrH = 1080;
    if (g_hero.lingS2NormX > 0.01f && g_hero.lingS2X > 1.f)
        scrW = (int)(g_hero.lingS2X / g_hero.lingS2NormX);
    if (g_hero.lingS2NormY > 0.01f && g_hero.lingS2Y > 1.f)
        scrH = (int)(g_hero.lingS2Y / g_hero.lingS2NormY);

    process_ling(frame, R, heroId, scrW, scrH);
    process_gusion(frame, R, heroId, scrW, scrH);
}
