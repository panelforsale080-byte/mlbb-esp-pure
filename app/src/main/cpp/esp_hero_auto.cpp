#include "esp_hero_auto.hpp"
#include "esp_retri_il2.hpp"
#include "esp_runtime_off.hpp"
#include "mem.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

static EspHeroAutoSettings g_hero{};

EspHeroAutoSettings &esp_hero_auto_settings() { return g_hero; }

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

static int read_origin_hero_id(pid_t pid, uintptr_t localPlayer, const EspRuntimeOff &R) {
    if (!R.heroOriginId || !mem_ptr_ok(localPlayer)) return 0;
    int hid = 0;
    if (!mem_read(pid, localPlayer + R.heroOriginId, hid)) return 0;
    return hid;
}

static bool hashset_find_bullet(pid_t pid, uintptr_t bmStatics, uintptr_t runBulletsOff,
                                uintptr_t bulIdOff, uintptr_t bulPosOff, int wantId, float selfX,
                                float selfY, float selfZ, float maxRange, float *outX, float *outY,
                                float *outZ, bool *outAnyId) {
    if (!bmStatics || !runBulletsOff || !bulIdOff || !bulPosOff) return false;
    uintptr_t hashSet = 0;
    if (!mem_read(pid, bmStatics + runBulletsOff, hashSet) || !mem_ptr_ok(hashSet)) return false;
    uintptr_t entries = 0;
    if (!mem_read(pid, hashSet + 0x18, entries) || !mem_ptr_ok(entries)) return false;
    int capacity = 0;
    if (!mem_read(pid, entries + 0x18, capacity) || capacity <= 0 || capacity > 2048) return false;

    float bestD = maxRange;
    bool found = false;
    for (int i = 0; i < capacity; ++i) {
        const uintptr_t entryAddr = entries + 0x20 + (uintptr_t)i * 16;
        int hashCode = 0;
        mem_read(pid, entryAddr, hashCode);
        if (hashCode < 0) continue;
        uintptr_t bullet = 0;
        if (!mem_read(pid, entryAddr + 8, bullet) || !mem_ptr_ok(bullet)) continue;
        int bid = 0;
        if (!mem_read(pid, bullet + bulIdOff, bid)) continue;
        if (bid == wantId) *outAnyId = true;
        if (bid != wantId) continue;
        float pos[3]{};
        if (!mem_read(pid, bullet + bulPosOff, pos, sizeof(pos))) continue;
        if (pos[0] == 0.f && pos[2] == 0.f) continue;
        const float dx = pos[0] - selfX;
        const float dy = pos[1] - selfY;
        const float dz = pos[2] - selfZ;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > maxRange || d < 0.35f) continue;
        if (d < bestD) {
            bestD = d;
            *outX = pos[0];
            *outY = pos[1];
            *outZ = pos[2];
            found = true;
        }
    }
    return found;
}

static bool hashset_has_bullet_id(pid_t pid, uintptr_t bmStatics, uintptr_t runBulletsOff,
                                  uintptr_t bulIdOff, int wantId) {
    if (!bmStatics || !runBulletsOff || !bulIdOff) return false;
    uintptr_t hashSet = 0;
    if (!mem_read(pid, bmStatics + runBulletsOff, hashSet) || !mem_ptr_ok(hashSet)) return false;
    uintptr_t entries = 0;
    if (!mem_read(pid, hashSet + 0x18, entries) || !mem_ptr_ok(entries)) return false;
    int capacity = 0;
    if (!mem_read(pid, entries + 0x18, capacity) || capacity <= 0 || capacity > 2048) return false;
    for (int i = 0; i < capacity; ++i) {
        const uintptr_t entryAddr = entries + 0x20 + (uintptr_t)i * 16;
        int hashCode = 0;
        mem_read(pid, entryAddr, hashCode);
        if (hashCode < 0) continue;
        uintptr_t bullet = 0;
        if (!mem_read(pid, entryAddr + 8, bullet) || !mem_ptr_ok(bullet)) continue;
        int bid = 0;
        if (!mem_read(pid, bullet + bulIdOff, bid)) continue;
        if (bid == wantId) return true;
    }
    return false;
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

static void process_ling(const EspFrame &frame, const EspRuntimeOff &R, int heroId) {
    if (!g_hero.lingSword || heroId != kHeroLing) return;
    if (!g_hero.useIl2Cast || !esp_retri_il2_ready() || !frame.localPlayerEnt) return;
    if (!R.bmStaticRunBullets || !R.bulletId || !R.bulletPos) return;

    static uint64_t s_lastCastMs = 0;
    const uint64_t now = steady_ms();
    if (now - s_lastCastMs < 180) return;

    float tx = 0, ty = 0, tz = 0;
    bool any = false;
    if (!hashset_find_bullet(frame.pid, frame.bmStatics, R.bmStaticRunBullets, R.bulletId,
                             R.bulletPos, kLingSwordBulletId, frame.selfX, frame.selfY,
                             frame.selfZ, g_hero.lingMaxRange, &tx, &ty, &tz, &any))
        return;

    if (s_castBusy.exchange(true, std::memory_order_acq_rel)) return;
    CastGuard guard(s_castBusy);

    float dx = tx - frame.selfX;
    float dy = ty - frame.selfY;
    float dz = tz - frame.selfZ;
    float ndx, ndy, ndz;
    norm_dir(dx, dy, dz, ndx, ndy, ndz);
    if (esp_il2_try_use_skill(frame.pid, frame.localPlayerEnt, kLingSkill2Cast, ndx, ndy, ndz, tx,
                              ty, tz))
        s_lastCastMs = now;
}

static void process_gusion(const EspFrame &frame, const EspRuntimeOff &R, int heroId) {
    if (!g_hero.gusionCombo || heroId != kHeroGusion) return;
    if (!g_hero.useIl2Cast || !esp_retri_il2_ready() || !frame.localPlayerEnt) return;
    if (!R.bmStaticRunBullets || !R.bulletId) return;

    static int s_state = 0;
    static uint64_t s_lastActionMs = 0;
    const uint64_t now = steady_ms();
    const int delay = g_hero.comboDelayMs < 40 ? 40 : g_hero.comboDelayMs;
    if (now - s_lastActionMs < (uint64_t)delay) return;

    const bool markAlive = hashset_has_bullet_id(frame.pid, frame.bmStatics, R.bmStaticRunBullets,
                                                 R.bulletId, kGusionMarkBulletId);

    float mx = frame.selfX, my = frame.selfY, mz = frame.selfZ;
    float markX = mx, markY = my, markZ = mz;
    bool hasMarkPos = false;
    if (markAlive && R.bulletPos) {
        bool any = false;
        hasMarkPos = hashset_find_bullet(frame.pid, frame.bmStatics, R.bmStaticRunBullets, R.bulletId,
                                         R.bulletPos, kGusionMarkBulletId, frame.selfX,
                                         frame.selfY, frame.selfZ, 25.f, &markX, &markY, &markZ,
                                         &any);
    }

    float dx = markX - mx;
    float dz = markZ - mz;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    float ndx, ndy, ndz;
    norm_dir(dx, 0.f, dz, ndx, ndy, ndz);

    auto cast = [&](int skill) -> bool {
        if (s_castBusy.exchange(true, std::memory_order_acq_rel)) return false;
        CastGuard guard(s_castBusy);
        return esp_il2_try_use_skill(frame.pid, frame.localPlayerEnt, skill, ndx, ndy, ndz, markX,
                                     markY, markZ);
    };

    bool acted = false;
    if (s_state == 0) {
        if (markAlive) {
            if (cast(kGusionS2)) acted = true;
            if (cast(kGusionS1)) {
                acted = true;
                s_state = 1;
            }
        }
    } else if (s_state == 1) {
        if (!markAlive || distXZ < 1.0f) {
            if (cast(kGusionS2)) acted = true;
            if (cast(kGusionS3)) {
                acted = true;
                s_state = 2;
            }
        }
    } else if (s_state == 2) {
        if (markAlive) {
            if (cast(kGusionS2)) acted = true;
            if (cast(kGusionS1)) {
                acted = true;
                s_state = 3;
            }
        } else {
            s_state = 0;
        }
    } else if (s_state == 3) {
        if (!markAlive || distXZ < 1.0f) {
            if (cast(kGusionS2)) acted = true;
            s_state = 0;
        }
    }

    if (acted) s_lastActionMs = now;
    (void)hasMarkPos;
}

} // namespace

void esp_hero_auto_process(const EspFrame &frame) {
    if (!g_hero.lingSword && !g_hero.gusionCombo) return;
    if (frame.status != EspStatus::Ready || frame.pid <= 0) return;
    if (!esp_runtime_off_ready()) return;
    if (!frame.bmStatics) return;

    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.bmStaticRunBullets || !R.bulletId) return;

    const int heroId = read_origin_hero_id(frame.pid, frame.localPlayerEnt, R);
    if (!heroId) return;

    process_ling(frame, R, heroId);
    process_gusion(frame, R, heroId);
}
