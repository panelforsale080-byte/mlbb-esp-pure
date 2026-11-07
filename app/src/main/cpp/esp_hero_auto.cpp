#include "esp_hero_auto.hpp"
#include "esp_retri_il2.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"
#include "touch_uinput.hpp"
#include "ui_log.hpp"

#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_set>
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

static bool bullet_world_pos(pid_t pid, uintptr_t bullet, const EspRuntimeOff &R, float selfX,
                             float selfY, float selfZ, float &x, float &y, float &z) {
    if (R.bulletPos) {
        float pos[3]{};
        if (mem_read(pid, bullet + R.bulletPos, pos, sizeof(pos)) &&
            !(std::fabs(pos[0]) < 0.01f && std::fabs(pos[2]) < 0.01f)) {
            x = pos[0];
            y = pos[1];
            z = pos[2];
            return true;
        }
    }
    if (R.bulletForward) {
        float fwd[3]{};
        if (mem_read(pid, bullet + R.bulletForward, fwd, sizeof(fwd))) {
            const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
            if (fl > 0.05f) {
                x = selfX + fwd[0] * 4.f;
                y = selfY;
                z = selfZ + fwd[2] * 4.f;
                return true;
            }
        }
    }
    return false;
}

static void scan_hashset_bullets(pid_t pid, uintptr_t hashSet,
                                 const std::function<void(uintptr_t)> &fn) {
    if (!mem_ptr_ok(hashSet)) return;
    const uintptr_t slotOffs[] = {0x10u, 0x18u};
    const uintptr_t dataOffs[] = {0x18u, 0x20u};
    const int strides[] = {16, 24};
    for (uintptr_t slotOff : slotOffs) {
        uintptr_t entries = 0;
        if (!mem_read(pid, hashSet + slotOff, entries) || !mem_ptr_ok(entries)) continue;
        for (uintptr_t dataOff : dataOffs) {
            int capacity = 0;
            if (!mem_read(pid, entries + 0x18, capacity) || capacity <= 0 || capacity > 4096)
                continue;
            for (int stride : strides) {
                for (int i = 0; i < capacity; ++i) {
                    const uintptr_t entryAddr = entries + dataOff + (uintptr_t)i * (uintptr_t)stride;
                    int hashCode = 0;
                    mem_read(pid, entryAddr, hashCode);
                    if (hashCode < 0) continue;
                    uintptr_t bullet = 0;
                    if (!mem_read(pid, entryAddr + 8, bullet) || !mem_ptr_ok(bullet)) continue;
                    fn(bullet);
                }
            }
        }
    }
}

static int foreach_run_bullet(pid_t pid, uintptr_t bmStatics, const EspRuntimeOff &R,
                              const std::function<void(uintptr_t bullet)> &fn) {
    if (!bmStatics || !R.bulletId) return 0;
    int total = 0;
    std::unordered_set<uintptr_t> dedupe;
    auto mark = [&](uintptr_t b) {
        if (!b || dedupe.count(b)) return;
        dedupe.insert(b);
        ++total;
        fn(b);
    };

    /* VIP _RunBullets BetterList first (often live while HashSet lags). */
    if (R.bmStaticRunBulletsList) {
        uintptr_t listObj = 0;
        if (mem_read(pid, bmStatics + R.bmStaticRunBulletsList, listObj) && mem_ptr_ok(listObj)) {
            std::vector<uintptr_t> items;
            if (esp_il2_managed_list(pid, listObj, items)) {
                for (size_t i = 0; i < items.size(); ++i) {
                    if (items[i]) mark(items[i]);
                }
            }
        }
    }

    if (R.bmStaticRunBullets) {
        uintptr_t hashSet = 0;
        if (mem_read(pid, bmStatics + R.bmStaticRunBullets, hashSet) && mem_ptr_ok(hashSet))
            scan_hashset_bullets(pid, hashSet, mark);
    }
    return total;
}

static bool find_bullet_by_id(pid_t pid, uintptr_t bmStatics, const EspRuntimeOff &R, int wantId,
                              float selfX, float selfY, float selfZ, float maxRange, float *outX,
                              float *outY, float *outZ) {
    float bestD = maxRange;
    bool found = false;
    int seen = 0;
    g_hero.lastBulletTotal =
        foreach_run_bullet(pid, bmStatics, R, [&](uintptr_t bullet) {
        int bid = 0;
        if (!mem_read(pid, bullet + R.bulletId, bid)) return;
        if (bid != wantId) return;
        ++seen;
        float bx = 0, by = 0, bz = 0;
        if (!bullet_world_pos(pid, bullet, R, selfX, selfY, selfZ, bx, by, bz)) return;
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

static bool ling_nearest_enemy(const EspFrame &frame, float maxDist, float &ex, float &ey,
                               float &ez) {
    float best = maxDist;
    bool ok = false;
    for (const auto &t : frame.targets) {
        if (t.kind != 0) continue;
        if (t.hp <= 0) continue;
        const float dx = t.worldX - frame.selfX;
        const float dy = t.worldY - frame.selfY;
        const float dz = t.worldZ - frame.selfZ;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best && d > 0.5f) {
            best = d;
            ex = t.worldX;
            ey = t.worldY;
            ez = t.worldZ;
            ok = true;
        }
    }
    return ok;
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
    const bool hasSword =
        find_bullet_by_id(frame.pid, frame.bmStatics, R, kLingSwordBulletId, frame.selfX,
                          frame.selfY, frame.selfZ, g_hero.lingMaxRange, &tx, &ty, &tz);
    if (!hasSword) {
        static uint64_t s_noSwordLog = 0;
        if (now - s_noSwordLog > 3000) {
            s_noSwordLog = now;
            ui_log("[ling] no sword 8452 (total bullets=%d); need S2 on ground or enemy fallback",
                   g_hero.lastBulletTotal);
        }
        if (!ling_nearest_enemy(frame, 10.f, tx, ty, tz)) return;
    }

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

    if (acted) {
        s_lastActionMs = now;
        static uint64_t s_gLog = 0;
        if (now - s_gLog > 2500) {
            s_gLog = now;
            ui_log("[gusion] combo step state=%d mark=%d", s_state, markAlive ? 1 : 0);
        }
    } else if (markAlive && now - s_lastActionMs > 2000) {
        static uint64_t s_waitLog = 0;
        if (now - s_waitLog > 4000) {
            s_waitLog = now;
            ui_log("[gusion] mark 5610 alive, waiting step (state=%d)", s_state);
        }
    }
}

static void draw_skill_dot(ImDrawList *draw, float x, float y, ImU32 col, const char *tag) {
    if (!draw) return;
    const ImVec2 p(x, y);
    draw->AddCircleFilled(p, 16.f, col, 14);
    draw->AddCircle(p, 20.f, IM_COL32(255, 255, 255, 200), 14, 2.f);
    if (tag && tag[0])
        draw->AddText(ImVec2(x - 8.f, y - 28.f), IM_COL32(255, 255, 255, 230), tag);
}

} // namespace

void esp_hero_auto_draw_indicator(ImDrawList *draw) {
    if (!draw || !g_hero.showSkillCircles) return;
    if (!g_hero.lingSword && !g_hero.gusionCombo) return;
    if (g_hero.lingSword && g_hero.lastHeroId == kHeroLing)
        draw_skill_dot(draw, g_hero.lingS2X, g_hero.lingS2Y, IM_COL32(80, 200, 255, 210), "L-S2");
    if (g_hero.gusionCombo && g_hero.lastHeroId == kHeroGusion) {
        draw_skill_dot(draw, g_hero.gusionS1X, g_hero.gusionS1Y, IM_COL32(255, 180, 80, 210),
                      "G-S1");
        draw_skill_dot(draw, g_hero.gusionS2X, g_hero.gusionS2Y, IM_COL32(255, 120, 180, 210),
                      "G-S2");
        draw_skill_dot(draw, g_hero.gusionS3X, g_hero.gusionS3Y, IM_COL32(180, 255, 120, 210),
                      "G-S3");
    }
}

void esp_hero_auto_anchor_drag(bool menuOpen, float screenW, float screenH) {
    if (!menuOpen || !g_hero.showSkillCircles) return;
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;
    const float r = 26.f;
    auto drag = [&](float &x, float &y) {
        const float dx = io.MousePos.x - x;
        const float dy = io.MousePos.y - y;
        if (dx * dx + dy * dy <= r * r && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            x = io.MousePos.x;
            y = io.MousePos.y;
            g_hero.useSkillAnchorNorm = false;
            if (screenW > 1.f && screenH > 1.f) {
                g_hero.lingS2NormX = x / screenW;
                g_hero.lingS2NormY = y / screenH;
            }
        }
    };
    if (g_hero.lingSword && g_hero.lastHeroId == kHeroLing) drag(g_hero.lingS2X, g_hero.lingS2Y);
    if (g_hero.gusionCombo && g_hero.lastHeroId == kHeroGusion) {
        drag(g_hero.gusionS1X, g_hero.gusionS1Y);
        drag(g_hero.gusionS2X, g_hero.gusionS2Y);
        drag(g_hero.gusionS3X, g_hero.gusionS3Y);
    }
}

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
    if (now - s_dbg > 5000) {
        s_dbg = now;
        ui_log("[hero] id=%d bullets=%d sword/mark=%d (Resolve+pick 8452/5610)",
               heroId, g_hero.lastBulletTotal, g_hero.lastBulletScan);
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
