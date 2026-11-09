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
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

static EspRetriSettings g_retri{};
static std::atomic<int> g_dispW{0};
static std::atomic<int> g_dispH{0};

/** Retribution true damage: 750 + 150×Lv. Lv1 = 900, Lv15 = 3000. */
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

/** One bad m_Level sample (15 → 1) was shrinking retri damage and missing the secure. */
static int stable_hero_level(uintptr_t ent, int readLv) {
    static uintptr_t seen = 0;
    static int held = 0;
    static int pending = 0;
    static int pendingHits = 0;
    if (!ent) return readLv;
    if (ent != seen) {
        seen = ent;
        held = readLv;
        pending = 0;
        pendingHits = 0;
        return held;
    }
    if (readLv == held || readLv == held + 1) {
        held = readLv;
        pending = 0;
        pendingHits = 0;
        return held;
    }
    if (readLv == pending) {
        if (++pendingHits >= 3) held = readLv;
    } else {
        pending = readLv;
        pendingHits = 1;
    }
    return held;
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
    return false;
}

static int resolve_retri_damage(const EspFrame &frame, const EspRuntimeOff &R, int &outLevel,
                                int &outKillWild, bool &outHaveKw) {
    outHaveKw = read_kill_wild_times(frame.pid, frame.localPlayerEnt, R, outKillWild);
    outLevel = stable_hero_level(
        frame.localPlayerEnt,
        read_local_level(frame.pid, frame.localPlayerEnt, R.level, g_retri.assumedLevel));
    if (outLevel < 1) outLevel = 1;
    if (outLevel > 15) outLevel = 15;
    return calculate_retri_damage(outLevel);
}

static uint64_t steady_ms();

enum RetriKind { kRetriNone = 0, kRetriLord = 1, kRetriTurtle = 2, kRetriRed = 3,
                 kRetriBlue = 4, kRetriCrab = 5, kRetriLitho = 6, kRetriBuff = 7 };

static bool ascii_has(const char *s, const char *needle) {
    if (!s || !needle || !needle[0]) return false;
    const size_t nlen = std::strlen(needle);
    for (const char *p = s; *p; ++p) {
        size_t i = 0;
        for (; i < nlen; ++i) {
            unsigned char a = (unsigned char)p[i];
            if (!a) return false;
            unsigned char b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool text_has(const char *s, const char *needle) {
    if (!s || !s[0] || !needle) return false;
    if (std::strstr(s, needle)) return true;
    return ascii_has(s, needle);
}

/** Name ids used as the retri filter. Prefab paths are not these names. */
static int kind_from_template(int id) {
    switch (id) {
    case 2002: return kRetriLord;
    case 2003:
    case 2110: return kRetriTurtle;
    case 2004:
    case 2220: return kRetriRed; /* Fiend */
    case 2005:
    case 2221: return kRetriBlue; /* Serpent */
    case 2056:
    case 2072: return kRetriLitho;
    case 2011:
    case 2013:
    case 2222:
    case 2223: return kRetriCrab;
    default: return kRetriNone;
    }
}

static int read_cfg_template(pid_t pid, uintptr_t ent) {
    const EspRuntimeOff &R = esp_runtime_off();
    const uintptr_t cfgOff = R.monsterConfig ? R.monsterConfig : 0x8b8;
    const uintptr_t idOff = R.monsterTableId ? R.monsterTableId : 0x14;
    uintptr_t cfg = 0;
    int id = 0;
    if (!mem_read(pid, ent + cfgOff, cfg) || !mem_ptr_ok(cfg)) return 0;
    if (!mem_read(pid, cfg + idOff, id)) return 0;
    return kind_from_template(id) ? id : 0;
}

/** Display name only. A prefab like mon_PatrolTurtle_new is not the filter. */
static int kind_from_display_name(const char *table, const char *role) {
    if (text_has(table, "soldier") || text_has(role, "soldier") || text_has(table, "minion") ||
        text_has(role, "minion") || text_has(table, "tower") || text_has(role, "tower"))
        return kRetriNone;
    if (text_has(table, "litho") || text_has(role, "litho")) return kRetriLitho;
    if (text_has(table, "turtle") || text_has(role, "turtle")) return kRetriTurtle;
    if (text_has(table, "lord") || text_has(role, "lord")) return kRetriLord;
    if (text_has(table, "crab") || text_has(role, "crab")) return kRetriCrab;
    if (text_has(table, "fiend") || text_has(role, "fiend")) return kRetriRed;
    if (text_has(table, "serpent") || text_has(role, "serpent")) return kRetriBlue;
    if (text_has(table, "buff") || text_has(role, "buff")) {
        if (text_has(table, "blue") || text_has(role, "blue")) return kRetriBlue;
        if (text_has(table, "red") || text_has(role, "red")) return kRetriRed;
        return kRetriBuff;
    }
    return kRetriNone;
}

static bool kind_enabled(int kind) {
    switch (kind) {
    case kRetriLord: return g_retri.targetLord;
    case kRetriTurtle: return g_retri.targetTurtle;
    case kRetriRed: return g_retri.targetRed;
    case kRetriBlue: return g_retri.targetBlue;
    case kRetriBuff: return g_retri.targetRed || g_retri.targetBlue;
    case kRetriCrab: return g_retri.targetCrab;
    case kRetriLitho: return g_retri.targetLito;
    default: return false;
    }
}

static const char *kind_label(int kind) {
    switch (kind) {
    case kRetriLord: return "Lord";
    case kRetriTurtle: return "Turtle";
    case kRetriRed: return "Buff Red";
    case kRetriBlue: return "Buff Blue";
    case kRetriBuff: return "Buff";
    case kRetriCrab: return "Crab";
    case kRetriLitho: return "Litho";
    default: return "?";
    }
}

struct NameCacheEntry {
    int kind = 0;
    int cfg = 0;
    uint64_t atMs = 0;
    char raw[48] = {};
};
static uintptr_t g_nameCacheBm = 0;
static std::unordered_map<uintptr_t, NameCacheEntry> g_nameCache;

static int kind_of_entity(pid_t pid, uintptr_t bm, uintptr_t ent, bool fresh, char *saw,
                          size_t sawSz) {
    if (bm != g_nameCacheBm) {
        g_nameCache.clear();
        g_nameCacheBm = bm;
    }
    const uint64_t now = steady_ms();
    if (!fresh) {
        const auto it = g_nameCache.find(ent);
        if (it != g_nameCache.end()) {
            if (it->second.kind != kRetriNone) return it->second.kind;
            if (now - it->second.atMs < 500) return kRetriNone;
        }
    }
    char table[96], role[96], prefab[96];
    table[0] = role[0] = prefab[0] = '\0';
    if (!esp_read_dict_monster_names(pid, ent, table, role, prefab, sizeof(table)))
        return kRetriNone;
    if (saw && sawSz && !saw[0]) {
        const char *raw = table[0] ? table : (role[0] ? role : prefab);
        if (raw[0]) std::snprintf(saw, sawSz, "%s", raw);
    }
    const EspRuntimeOff &R = esp_runtime_off();
    const int cfg = read_cfg_template(pid, ent);
    int kind = kind_from_template(cfg);
    if (!kind && R.wildType) {
        int wt = 0;
        const uintptr_t wtOff = R.wildType ? R.wildType : 0x8f0;
        if (mem_read(pid, ent + wtOff, wt)) {
            if (wt == 4) kind = kRetriLord;
            else if (wt == 3) kind = kRetriTurtle;
        }
    }
    if (!kind && R.entityId) {
        int live = 0;
        if (mem_read(pid, ent + R.entityId, live)) kind = kind_from_template(live);
    }
    if (!kind) kind = kind_from_display_name(table, role);
    if (g_nameCache.size() > 256) g_nameCache.clear();
    NameCacheEntry e;
    e.kind = kind;
    e.cfg = cfg;
    e.atMs = now;
    std::snprintf(e.raw, sizeof(e.raw), "%s", kind ? kind_label(kind) : (table[0] ? table : role));
    g_nameCache[ent] = e;
    return kind;
}

/** Own red/blue only. Enemy buff camp stays targetable. Neutral is not allied. */
static bool allied_monster_camp(pid_t pid, uintptr_t local, uintptr_t ent) {
    int selfCamp = 0, mobCamp = 0;
    if (!local || !ent) return false;
    if (!mem_read(pid, local + 0xd0, selfCamp)) return false;
    if (!mem_read(pid, ent + 0xd0, mobCamp)) return false;
    if (selfCamp == 1 && mobCamp == 3) return true;
    if (selfCamp == 2 && mobCamp == 4) return true;
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

/** m_Hp is a 32-bit field. An 8-byte read picks up the next field and looks like a huge HP. */
static int read_entity_hp(pid_t pid, uintptr_t ent, uintptr_t hpOff) {
    int hp32 = 0;
    const bool ok32 = mem_read(pid, ent + hpOff, hp32) && hp32 > 0 && hp32 < 250000;
    int64_t hp64 = 0;
    const bool ok64 = mem_read(pid, ent + hpOff, hp64) && hp64 > 0 && hp64 < 250000;
    if (ok32 && ok64 && (int)hp64 != hp32) return hp32;
    if (ok32) return hp32;
    if (ok64) return (int)hp64;
    return 0;
}

/** Drop stale dict rows: dead flag, bad HP vs max, or invalid world pos. */
static bool retri_mob_alive(pid_t pid, uintptr_t ent, const EspRuntimeOff &R, int *outHp,
                            int *outHpMax) {
    if (!ent || !outHp) return false;
    *outHp = 0;
    if (outHpMax) *outHpMax = 0;
    uint8_t dead = 0;
    if (mem_read(pid, ent + R.entityDeath, dead) && dead) return false;
    const int hp = read_entity_hp(pid, ent, R.hp);
    if (hp <= 0) return false;
    int hpMax = 0;
    if (R.hpMax) mem_read(pid, ent + R.hpMax, hpMax);
    if (hpMax >= 100 && hp > hpMax) return false;
    if (hpMax < 100) return false;
    float wx = 0, wy = 0, wz = 0;
    if (!esp_read_entity_world(pid, ent, wx, wy, wz)) return false;
    *outHp = hp;
    if (outHpMax) *outHpMax = hpMax;
    return true;
}

/** Current bar only. No previous-sample wait. */
static bool hp_ready_to_secure(uintptr_t, int hp, int hpMax, int retriDmg) {
    return hp > 0 && hpMax >= 100 && hp <= hpMax && hp <= retriDmg;
}

static void update_norm_from_pixels(float sw, float sh) {
    if (sw > 1.f) g_retri.tapNormX = g_retri.tapX / sw;
    if (sh > 1.f) g_retri.tapNormY = g_retri.tapY / sh;
}

static bool self_pos_ok(float x, float y, float z) {
    (void)y;
    return std::isfinite(x) && std::isfinite(z) &&
           !(std::fabs(x) < 0.01f && std::fabs(z) < 0.01f) && std::fabs(x) <= 10000.f &&
           std::fabs(z) <= 10000.f;
}

/** Other battle spells. 0 / 20020 / unknown still attempts the cast. */
static bool summon_is_other_spell(int sid) {
    switch (sid) {
    case 20030:
    case 20040:
    case 20050:
    case 20060:
    case 20070:
    case 20080:
    case 20100:
    case 20140:
    case 20150:
        return true;
    default:
        return false;
    }
}

/**
 * Pick a mob that retri can actually secure.
 * The closest mob is often a full-HP buff; stopping on it skipped a lord/turtle
 * a few meters farther that was already in execute range.
 * Same-camp jungle is skipped so the spell is not spent on our own camp.
 * Range is horizontal (XZ). Skill reach does not include height.
 */
static bool find_executable_retri_target(const EspFrame &frame, const EspRuntimeOff &R, float selfX,
                                         float selfY, float selfZ, int retriDmg, float maxDist,
                                         uintptr_t &outEnt, float &outDist, int &outHp, int &outKind,
                                         int &dbgKind, int &dbgHp, float &dbgDist, char *dbgRaw,
                                         size_t dbgRawSz, char *sawName, size_t sawSz) {
    outEnt = 0;
    outDist = 1e9f;
    outHp = 0;
    outKind = 0;
    dbgKind = 0;
    dbgHp = 0;
    dbgDist = 1e9f;
    if (dbgRaw && dbgRawSz) dbgRaw[0] = '\0';
    if (sawName && sawSz) sawName[0] = '\0';
    if (!frame.bmInstance) return false;

    std::vector<std::pair<int, uintptr_t>> monsters;
    if (!esp_monster_dict_entries(frame.pid, frame.bmInstance, monsters)) return false;

    struct Mob {
        uintptr_t ent;
        int kind;
        int hp;
        int hpMax;
        float x, y, z, dist;
    };
    std::vector<Mob> alive;
    alive.reserve(monsters.size());

    for (const auto &kv : monsters) {
        const uintptr_t ent = kv.second;
        if (!ent) continue;
        const int kind = kind_of_entity(frame.pid, frame.bmInstance, ent, false, sawName, sawSz);
        if (!kind_enabled(kind)) continue;
        /* Skip only our own buff camp. Enemy Fiend/Serpent stay targetable. */
        const bool campBuff = kind == kRetriRed || kind == kRetriBlue || kind == kRetriBuff;
        if (campBuff && allied_monster_camp(frame.pid, frame.localPlayerEnt, ent)) continue;
        int hp = 0, hpMax = 0;
        if (!retri_mob_alive(frame.pid, ent, R, &hp, &hpMax)) continue;
        float mx = 0, my = 0, mz = 0;
        if (!esp_read_entity_world(frame.pid, ent, mx, my, mz)) continue;
        const float dy = my - selfY;
        if (!std::isfinite(dy) || std::fabs(dy) > 25.f) continue;
        const float dx = mx - selfX;
        const float dz = mz - selfZ;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < dbgDist) {
            dbgDist = dist;
            dbgKind = kind;
            dbgHp = hp;
            if (dbgRaw && dbgRawSz) {
                const auto it = g_nameCache.find(ent);
                if (it != g_nameCache.end())
                    std::snprintf(dbgRaw, dbgRawSz, "%s cfg=%d", it->second.raw, it->second.cfg);
            }
        }
        alive.push_back({ent, kind, hp, hpMax, mx, my, mz, dist});
    }

    for (const Mob &m : alive) {
        if (m.dist > maxDist) continue;
        if (!hp_ready_to_secure(m.ent, m.hp, m.hpMax, retriDmg)) continue;
        /* One Lord and one Turtle. A second copy of that name is the old body. */
        bool ghost = false;
        const bool uniqueBoss = m.kind == kRetriLord || m.kind == kRetriTurtle;
        for (const Mob &other : alive) {
            if (other.ent == m.ent || other.kind != m.kind || other.hp <= m.hp) continue;
            if (uniqueBoss) {
                ghost = true;
                break;
            }
            const float ox = other.x - m.x;
            const float oz = other.z - m.z;
            if (ox * ox + oz * oz < 18.f * 18.f) {
                ghost = true;
                break;
            }
        }
        if (ghost) continue;
        if (!outEnt || m.hp < outHp || (m.hp == outHp && m.dist < outDist)) {
            outDist = m.dist;
            outEnt = m.ent;
            outHp = m.hp;
            outKind = m.kind;
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

    float selfX = frame.selfX;
    float selfY = frame.selfY;
    float selfZ = frame.selfZ;
    if (!self_pos_ok(selfX, selfY, selfZ) && frame.localPlayerEnt)
        esp_read_entity_world(frame.pid, frame.localPlayerEnt, selfX, selfY, selfZ);

    uintptr_t execEnt = 0;
    float execDist = 1e9f;
    int execHp = 0;
    int execKind = 0;
    int dbgKind = 0, dbgHp = 0;
    float dbgDist = 1e9f;
    char sawName[96];
    char dbgRaw[48];
    sawName[0] = '\0';
    dbgRaw[0] = '\0';
    if (self_pos_ok(selfX, selfY, selfZ)) {
        find_executable_retri_target(frame, R, selfX, selfY, selfZ, retriDmg, kMaxDist, execEnt,
                                     execDist, execHp, execKind, dbgKind, dbgHp, dbgDist, dbgRaw,
                                     sizeof(dbgRaw), sawName, sizeof(sawName));
    }

    static uint64_t s_lastDbgMs = 0;
    if (now - s_lastDbgMs > 3000) {
        s_lastDbgMs = now;
        if (execEnt) {
            ui_log("[retri] lvl=%d kw=%d dmg=%d EXEC %s hp=%d dist=%.2f",
                   playerLevel, haveKw ? killWild : -1, retriDmg, kind_label(execKind), execHp,
                   execDist);
        } else if (dbgKind) {
            const char *why = dbgDist > kMaxDist ? "out of range" :
                              (dbgHp > retriDmg ? "hp above damage" : "not executable");
            ui_log("[retri] lvl=%d kw=%d dmg=%d closest %s name=\"%s\" hp=%d dist=%.2f (%s)",
                   playerLevel, haveKw ? killWild : -1, retriDmg, kind_label(dbgKind), dbgRaw,
                   dbgHp, dbgDist, why);
        } else if (sawName[0]) {
            ui_log("[retri] lvl=%d kw=%d dmg=%d — no named retri mob (saw %s)",
                   playerLevel, haveKw ? killWild : -1, retriDmg, sawName);
        } else {
            ui_log("[retri] lvl=%d kw=%d dmg=%d — no named retri mob",
                   playerLevel, haveKw ? killWild : -1, retriDmg);
        }
    }

    if (!execEnt) return;

    const int realHp = read_entity_hp(frame.pid, execEnt, R.hp);
    if (realHp <= 0 || realHp > retriDmg) return;
    const int liveKind =
        kind_of_entity(frame.pid, frame.bmInstance, execEnt, true, nullptr, 0);
    if (liveKind != kRetriNone && liveKind != execKind) return;

    if (R.summonSkillId && frame.localPlayerEnt) {
        int sid = 0;
        if (mem_read(frame.pid, frame.localPlayerEnt + R.summonSkillId, sid) &&
            summon_is_other_spell(sid)) {
            static uint64_t s_lastSpellLog = 0;
            if (now - s_lastSpellLog > 3000) {
                s_lastSpellLog = now;
                ui_log("[retri] summon id %d is not retribution — skip", sid);
            }
            return;
        }
    }

    /* First cast is immediate. Later attempts wait so a ptrace retry cannot cancel the spell. */
    static uint64_t s_nextTryMs = 0;
    if (now < s_nextTryMs) return;

    if (s_castBusy.exchange(true, std::memory_order_acq_rel)) return;
    CastBusyGuard castGuard(s_castBusy);

    if (g_retri.useIl2SummonCast && esp_retri_il2_ready() && frame.localPlayerEnt) {
        float mx = 0, my = 0, mz = 0;
        EspIl2Cast st = EspIl2Cast::Failed;
        if (esp_read_entity_world(frame.pid, execEnt, mx, my, mz)) {
            float dx = mx - selfX;
            float dy = my - selfY;
            float dz = mz - selfZ;
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
            st = esp_retri_il2_cast_summon(frame.pid, frame.localPlayerEnt, dx, dy, dz);
        }
        /* TryUseSkill still fires the spell when the boxed code is not 0.
         * Logging that as REJECT/FAIL was a false failure. One line per send. */
        if (st == EspIl2Cast::Failed) {
            s_nextTryMs = now + 80;
        } else {
            ui_log("[retri] CAST %s hp=%d dist=%.1f", kind_label(execKind), realHp, execDist);
            s_nextTryMs = now + 220;
        }
    } else {
        fire_retri_tap();
        s_nextTryMs = now + 650;
        ui_log("[retri] TAP %s hp=%d dist=%.1f (shell / uinput)", kind_label(execKind), realHp,
               execDist);
    }
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
