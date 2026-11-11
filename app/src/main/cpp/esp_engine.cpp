#include "esp_engine.hpp"
#include "esp_camera.hpp"
#include "esp_retri_il2.hpp"
#include "conrent_esp.hpp"
#include "il2_layout.hpp"
#include "esp_monster.hpp"
#include "esp_runtime_off.hpp"
#include "proc.hpp"
#include "remote_il2cpp.hpp"
#include "remote_elf.hpp"
#include "ui_log.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static std::atomic<bool> g_tickHeroName{true};
static std::atomic<bool> g_tickEnemyCd{true};
static std::atomic<bool> g_tickMonsters{true};
static std::atomic<bool> g_tickFacing{false};
static std::atomic<bool> g_tickBuildTargets{true};

/* Reader tick index: stagger heavy per-entity reads (IL2 string + cooldown dict) so busy
 * fights do not run full cost for every entity every frame. */
static std::atomic<uint64_t> g_readerTickIndex{0};

/** Stagger IL2 hero-name / cooldown dict walks — higher = less CPU, slightly staler CD/name. */
static constexpr unsigned kHeavyReadInterval = 15;

void esp_tick_set_feature_flags(bool heroName, bool enemyCooldown, bool monsters, bool facing) {
    g_tickHeroName.store(heroName);
    g_tickEnemyCd.store(enemyCooldown);
    g_tickMonsters.store(monsters);
    g_tickFacing.store(facing);
}

void esp_tick_set_build_targets(bool on) { g_tickBuildTargets.store(on); }

static bool read_u32_pair(pid_t pid, uintptr_t offStart, uintptr_t offCool, uint32_t &startTime,
                        uint32_t &coolTime) {
    std::vector<std::pair<uintptr_t, size_t>> req = {{offStart, 4}, {offCool, 4}};
    std::vector<std::vector<uint8_t>> out;
    if (!mem_read_batch(pid, req, out) || out.size() != 2) return false;
    std::memcpy(&startTime, out[0].data(), sizeof(startTime));
    std::memcpy(&coolTime, out[1].data(), sizeof(coolTime));
    return true;
}

static std::mutex g_mu;
static pid_t g_pid = 0;
static uintptr_t g_bmKlass = 0;
static uintptr_t g_bmStatics = 0;
static uintptr_t g_bmInstance = 0;
static uintptr_t g_bmInstancePin = 0;
static bool g_resolved = false;
static std::atomic<bool> g_forceResolve{false};
static EspResolveReport g_lastReport{};
static const RemoteIl2CppInfo *g_il2_for_resolve = nullptr;
static RemoteIl2CppInfo g_il2Snap{};
static std::atomic<bool> g_il2SnapOk{false};

struct BmFindMeta {
    int nameStrings = 0;
    bool usedElf = false;
};

static bool name_at(pid_t pid, uintptr_t p, const char *want) {
    char buf[80] = {};
    if (!mem_read(pid, p, buf, sizeof(buf) - 1)) return false;
    return std::strcmp(buf, want) == 0;
}

static const char *tail_name(const char *full) {
    const char *d = std::strrchr(full, '.');
    return d ? d + 1 : full;
}

static bool map_for_strings(const MapsReg &r) {
    if (r.perms[0] != 'r' || r.end <= r.start) return false;
    const size_t sz = (size_t)(r.end - r.start);
    if (sz < 16 || sz > 128 * 1024 * 1024) return false;
    const std::string &p = r.path;
    if (p.find("[stack]") != std::string::npos) return false;
    if (p.find("global-metadata") != std::string::npos) return true;
    if (p.find("liblogic") != std::string::npos) return true;
    if (p.find("csharp") != std::string::npos) return true;
    if (p.find("il2cpp") != std::string::npos) return true;
    if (p.find(".apk") != std::string::npos) return true;
    if (p.find(".dat") != std::string::npos) return true;
    if (p.find(".so") != std::string::npos) return sz <= 48 * 1024 * 1024;
    if (p.empty() || p[0] == '[') return sz <= 32 * 1024 * 1024;
    return sz <= 16 * 1024 * 1024;
}

static int klass_region_prio(const MapsReg &r) {
    if (r.perms[0] != 'r' || r.perms[1] != 'w' || r.end <= r.start) return 0;
    const size_t sz = (size_t)(r.end - r.start);
    if (sz < 0x40 || sz > 96 * 1024 * 1024) return 0;
    const std::string &p = r.path;
    if (p.find("[stack]") != std::string::npos) return 0;
    if (p.find("liblogic") != std::string::npos) return 100;
    if (p.find("csharp") != std::string::npos) return 90;
    if (p.find("libil2cpp") != std::string::npos) return 85;
    if (p.find("il2cpp") != std::string::npos) return 80;
    if (p.find("libunity") != std::string::npos) return 70;
    if (p.find(".so") != std::string::npos) return 40;
    if (p.empty() || p.find("[anon:") != std::string::npos) {
        if (sz <= 12 * 1024 * 1024) return 30;
        return 0;
    }
    return 0;
}

static void scan_strings(pid_t pid, const std::vector<MapsReg> &maps,
                         const char *needle, std::vector<uintptr_t> &out) {
    out.clear();
    const size_t nlen = std::strlen(needle) + 1;
    for (const auto &m : maps) {
        if (!map_for_strings(m)) continue;
        const size_t sz = (size_t)(m.end - m.start);
        const size_t step = 1024 * 1024;
        for (size_t off = 0; off < sz && out.size() < 32; off += step) {
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

static bool dict_entries(pid_t pid, uintptr_t dict,
                         std::vector<std::pair<int, uintptr_t>> &out) {
    out.clear();
    if (!dict) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!mem_read(pid, dict + Il2::DictEntries, entries) || !entries) return false;
    if (!mem_read(pid, dict + Il2::DictCount, count) || count <= 0 || count > 1024)
        return false;
    int maxLength = 0;
    if (!mem_read(pid, entries + Il2::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 4096)
        return false;
    const uintptr_t data = entries + Il2::ArrData;
    const size_t slotBytes = (size_t)maxLength * Il2::EntryStride;
    static thread_local std::vector<uint8_t> raw;
    raw.resize(slotBytes);
    if (!mem_read(pid, data, raw.data(), raw.size())) return false;
    for (int i = 0; i < maxLength; ++i) {
        const uint8_t *base = raw.data() + (size_t)i * Il2::EntryStride;
        int hash = 0, key = 0;
        uintptr_t value = 0;
        std::memcpy(&hash, base + Il2::EntryHash, sizeof(hash));
        if (hash < 0) continue;
        std::memcpy(&key, base + Il2::EntryKey, sizeof(key));
        std::memcpy(&value, base + Il2::EntryValue, sizeof(value));
        if (!value) continue;
        out.emplace_back(key, value);
        if ((int)out.size() >= count) break;
    }
    return !out.empty();
}

static bool utf16le_to_utf8(const std::vector<uint16_t> &units, char *out, size_t outSz) {
    if (!out || outSz == 0) return false;
    out[0] = '\0';
    size_t o = 0;
    const size_t n = units.size();
    for (size_t i = 0; i < n && o + 4 < outSz;) {
        uint32_t c = units[i];
        if (c >= 0xD800u && c <= 0xDBFFu && i + 1 < n) {
            const uint32_t low = units[i + 1];
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (low - 0xDC00u);
                i += 2;
            } else
                ++i;
        } else
            ++i;
        if (c < 0x80u) {
            out[o++] = static_cast<char>(c);
        } else if (c < 0x800u) {
            out[o++] = static_cast<char>(0xC0u | (c >> 6));
            out[o++] = static_cast<char>(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            out[o++] = static_cast<char>(0xE0u | (c >> 12));
            out[o++] = static_cast<char>(0x80u | ((c >> 6) & 0x3Fu));
            out[o++] = static_cast<char>(0x80u | (c & 0x3Fu));
        } else {
            out[o++] = static_cast<char>(0xF0u | (c >> 18));
            out[o++] = static_cast<char>(0x80u | ((c >> 12) & 0x3Fu));
            out[o++] = static_cast<char>(0x80u | ((c >> 6) & 0x3Fu));
            out[o++] = static_cast<char>(0x80u | (c & 0x3Fu));
        }
    }
    if (o < outSz) out[o] = '\0';
    else out[outSz - 1] = '\0';
    return o > 0;
}

static bool read_il2_managed_string(pid_t pid, uintptr_t strObj, char *out, size_t outSz) {
    if (!out || outSz == 0) return false;
    out[0] = '\0';
    if (!strObj || !mem_ptr_ok(strObj)) return false;
    /* One vm_read: length @ +0x10 then up to 96 UTF-16 chars (cap). */
    alignas(4) uint8_t blob[4 + 96 * 2];
    if (!mem_read(pid, strObj + 0x10, blob, sizeof(blob))) return false;
    int32_t len = 0;
    std::memcpy(&len, blob, sizeof(len));
    if (len <= 0 || len > 96) return false;
    const uint8_t *raw = blob + 4;
    std::vector<uint16_t> units((size_t)len);
    for (int32_t i = 0; i < len; ++i)
        units[(size_t)i] =
            (uint16_t)raw[(size_t)i * 2] | ((uint16_t)raw[(size_t)i * 2 + 1] << 8);
    return utf16le_to_utf8(units, out, outSz);
}

struct CoolDownTimerState {
    uint32_t startTime = 0;
    uint32_t durationMs = 0;
    std::chrono::steady_clock::time_point observedAt{};
    bool valid = false;
};

static std::unordered_map<uintptr_t, CoolDownTimerState> g_cooldownTimers;

struct HeroNameEntCache {
    char name[192] = {};
    bool valid = false;
};
static std::unordered_map<uintptr_t, HeroNameEntCache> g_heroNameByEnt;

struct CdEntCache {
    int s1 = 0, s2 = 0, s3 = 0, spell = 0;
    bool valid = false;
};
static std::unordered_map<uintptr_t, CdEntCache> g_cdByEnt;

struct DictSnap {
    uintptr_t bm = 0;
    uintptr_t pDict = 0;
    uintptr_t mDict = 0;
    int dictPlayers = 0;
    int dictMonsters = 0;
    std::vector<std::pair<int, uintptr_t>> players;
    std::vector<std::pair<int, uintptr_t>> monsters;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;
};
static DictSnap g_dictSnap;

struct MonSnap {
    pid_t pid = 0;
    uintptr_t bm = 0;
    std::vector<std::pair<int, uintptr_t>> entries;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;
};
static MonSnap g_monSnap;

static pid_t g_probePid = 0;
static bool g_probeOk = false;
static std::chrono::steady_clock::time_point g_probeAt{};

static void invalidate_read_caches() {
    g_dictSnap = {};
    g_monSnap = {};
    g_probePid = 0;
    g_probeAt = {};
}

static bool probe_read_cached(pid_t pid) {
    const auto now = std::chrono::steady_clock::now();
    if (g_probePid == pid && g_probeAt.time_since_epoch().count() != 0 &&
        now - g_probeAt < std::chrono::seconds(5))
        return g_probeOk;
    g_probeOk = proc_probe_read(pid);
    g_probePid = pid;
    g_probeAt = now;
    return g_probeOk;
}

static void apply_cooldown_skill_slot(int skillID, int playerSpellId, int timeLeftSec,
                                      int &s1, int &s2, int &s3, int &spell, int &regen) {
    const int fSkill = skillID % 100;
    if (skillID < 20000) {
        switch (fSkill) {
        case 10:
            if (timeLeftSec > s1) s1 = timeLeftSec;
            break;
        case 20:
            if (timeLeftSec > s2) s2 = timeLeftSec;
            break;
        case 30:
            if (timeLeftSec > s3) s3 = timeLeftSec;
            break;
        default:
            break;
        }
    } else {
        if (skillID == 21010 || skillID == 2101 || skillID == 20010) {
            if (timeLeftSec > regen) regen = timeLeftSec;
        } else if (skillID == playerSpellId || (playerSpellId > 0 && (skillID / 10) == playerSpellId)) {
            if (timeLeftSec > spell) spell = timeLeftSec;
        } else if (fSkill == 10 || (skillID <= 20500 && (skillID % 10) == 0)) {
            if (timeLeftSec > spell) spell = timeLeftSec;
        }
    }
}

static bool fill_show_cooldowns(pid_t pid, uintptr_t showEnt, int playerSpellId, EspTarget &t, bool doHeavyRead) {
    const EspRuntimeOff &R = esp_runtime_off();
    t.cdSkill1 = t.cdSkill2 = t.cdSkill3 = t.cdSpell = 0;
    t.cdValid = false;
    if (!g_tickEnemyCd.load()) return false;
    if (!doHeavyRead) {
        auto it = g_cdByEnt.find(showEnt);
        if (it != g_cdByEnt.end() && it->second.valid) {
            t.cdSkill1 = it->second.s1;
            t.cdSkill2 = it->second.s2;
            t.cdSkill3 = it->second.s3;
            t.cdSpell = it->second.spell;
            t.cdValid = true;
            return true;
        }
        return false;
    }
    if (!R.showCooldownComp || !R.showCdDic || !R.cddSpellId || !R.cddCoolTime || !R.cddStartTime)
        return false;
    uintptr_t cdComp = 0;
    if (!mem_read(pid, showEnt + R.showCooldownComp, cdComp) || !cdComp || !mem_ptr_ok(cdComp))
        return false;
    uintptr_t dic = 0;
    if (!mem_read(pid, cdComp + R.showCdDic, dic) || !dic || !mem_ptr_ok(dic)) return false;
    std::vector<std::pair<int, uintptr_t>> items;
    if (!dict_entries(pid, dic, items) || items.empty()) return false;
    struct CdWant {
        int skillID = 0;
        uintptr_t cdAddr = 0;
    };
    std::vector<CdWant> wanted;
    wanted.reserve(items.size());
    std::vector<std::pair<uintptr_t, size_t>> req;
    req.reserve(items.size() * 2);
    for (const auto &kv : items) {
        const int skillID = kv.first;
        const uintptr_t cdAddr = kv.second;
        if (skillID <= 0 || !cdAddr || !mem_ptr_ok(cdAddr)) continue;
        CdWant w{};
        w.skillID = skillID;
        w.cdAddr = cdAddr;
        wanted.push_back(w);
        req.emplace_back(cdAddr + R.cddStartTime, 4);
        req.emplace_back(cdAddr + R.cddCoolTime, 4);
    }
    std::vector<std::vector<uint8_t>> raw;
    const bool batched = !req.empty() && mem_read_batch(pid, req, raw) && raw.size() == req.size();
    int s1 = 0, s2 = 0, s3 = 0, spell = 0, regen = 0;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < wanted.size(); ++i) {
        const int skillID = wanted[i].skillID;
        const uintptr_t cdAddr = wanted[i].cdAddr;
        uint32_t startTime = 0, coolTime = 0;
        bool got = false;
        if (batched) {
            const size_t a = i * 2;
            const size_t b = a + 1;
            if (a < raw.size() && b < raw.size() && raw[a].size() >= 4 && raw[b].size() >= 4) {
                std::memcpy(&startTime, raw[a].data(), 4);
                std::memcpy(&coolTime, raw[b].data(), 4);
                got = true;
            }
        }
        if (!got &&
            !read_u32_pair(pid, cdAddr + R.cddStartTime, cdAddr + R.cddCoolTime, startTime, coolTime))
            continue;
        if (coolTime == 0) continue;
        auto &timer = g_cooldownTimers[cdAddr];
        if (timer.startTime != startTime || !timer.valid) {
            timer.startTime = startTime;
            timer.durationMs = coolTime;
            timer.observedAt = now;
            timer.valid = true;
        }
        const auto elapsedMs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - timer.observedAt).count());
        const uint32_t remainingMs =
            elapsedMs >= timer.durationMs ? 0u : (timer.durationMs - elapsedMs);
        if (remainingMs == 0) continue;
        const int timeLeftSec = static_cast<int>((remainingMs + 999u) / 1000u);
        apply_cooldown_skill_slot(skillID, playerSpellId, timeLeftSec, s1, s2, s3, spell, regen);
    }
    t.cdSkill1 = s1;
    t.cdSkill2 = s2;
    t.cdSkill3 = s3;
    t.cdSpell = spell;
    (void)regen;
    t.cdValid = true;
    g_cdByEnt[showEnt] = {s1, s2, s3, spell, true};
    return true;
}

static bool list_entities(pid_t pid, uintptr_t list, std::vector<uintptr_t> &out) {
    out.clear();
    if (!list || !mem_ptr_ok(list)) return false;
    uintptr_t arr = 0;
    int size = 0;
    if (!mem_read(pid, list + Il2::ListItems, arr) || !arr || !mem_ptr_ok(arr)) return false;
    if (!mem_read(pid, list + Il2::ListSize, size) || size <= 0 || size > 512) return false;
    int maxLength = 0;
    if (!mem_read(pid, arr + Il2::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 512)
        return false;
    const uintptr_t data = arr + Il2::ArrData;
    const int n = std::min(size, maxLength);
    std::vector<uint8_t> raw((size_t)n * sizeof(uintptr_t));
    if (!mem_read(pid, data, raw.data(), raw.size())) return false;
    for (int i = 0; i < n; ++i) {
        uintptr_t p = 0;
        std::memcpy(&p, raw.data() + (size_t)i * sizeof(uintptr_t), sizeof(p));
        if (p && mem_ptr_ok(p)) out.push_back(p);
    }
    return !out.empty();
}

static bool pos_ok(const Vec3 &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && !(std::fabs(p.x) < 0.01f && std::fabs(p.z) < 0.01f)
        && std::fabs(p.x) <= 10000.f && std::fabs(p.z) <= 10000.f;
}

static bool read_entity_pos(pid_t pid, uintptr_t ent, Vec3 &pos) {
    const EspRuntimeOff &R = esp_runtime_off();
    if (R.vCachePosition && mem_read(pid, ent + R.vCachePosition, pos) && pos_ok(pos))
        return true;
    if (R.cachePosition && mem_read(pid, ent + R.cachePosition, pos) && pos_ok(pos))
        return true;
    if (R.lastPosition && mem_read(pid, ent + R.lastPosition, pos) && pos_ok(pos)) return true;
    pos = {};
    return false;
}

static bool bm_instance_sane(pid_t pid, uintptr_t bm) {
    if (!esp_runtime_off_ready() || !bm || !mem_ptr_ok(bm)) return false;
    const EspRuntimeOff &R = esp_runtime_off();
    uintptr_t pDict = 0, mDict = 0;
    if (!mem_read(pid, bm + R.dicPlayerShow, pDict) || !mem_ptr_ok(pDict)) return false;
    mem_read(pid, bm + R.dicMonsterShow, mDict);
    if (mDict && !mem_ptr_ok(mDict)) return false;
    std::vector<std::pair<int, uintptr_t>> players;
    if (!dict_entries(pid, pDict, players) || players.empty() || players.size() > 20) return false;
    int usable = 0;
    for (const auto &kv : players) {
        if (!kv.second) continue;
        uint8_t dead = 0, camp = 0;
        if (mem_read(pid, kv.second + R.entityDeath, dead) && dead) continue;
        if (mem_read(pid, kv.second + R.sameCamp, camp) && camp) continue;
        Vec3 pos{};
        if (mem_read(pid, kv.second + R.lastPosition, pos) && pos_ok(pos)) ++usable;
    }
    return usable > 0;
}

static bool statics_for(pid_t pid, uintptr_t klass, uintptr_t instOff,
                        uintptr_t &statics, uintptr_t &instance) {
    for (uintptr_t off : Il2::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!mem_read(pid, klass + off, st) || !mem_ptr_ok(st)) continue;
        uintptr_t inst = 0;
        mem_read(pid, st + instOff, inst);
        statics = st;
        instance = mem_ptr_ok(inst) ? inst : 0;
        return true;
    }
    return false;
}

static bool klass_matches_name(pid_t pid, uintptr_t klass, const char *name,
                               const std::vector<uintptr_t> *knownStrs) {
    static const uintptr_t kOffs[] = {0x10, 0x18, 0x08, 0x20, 0x28, 0x30, 0x38};
    for (uintptr_t off : kOffs) {
        uintptr_t np = 0;
        if (!mem_read(pid, klass + off, np) || !mem_ptr_ok(np)) continue;
        if (knownStrs) {
            for (uintptr_t s : *knownStrs)
                if (np == s) return true;
        }
        if (name_at(pid, np, name)) return true;
    }
    return false;
}

static bool verify_klass(pid_t pid, uintptr_t klass, const char *name,
                         uintptr_t instOff, uintptr_t &statics, uintptr_t &instance,
                         const std::vector<uintptr_t> *knownStrs = nullptr) {
    if (!klass_matches_name(pid, klass, name, knownStrs)) return false;
    return statics_for(pid, klass, instOff, statics, instance);
}

static int score_bm(pid_t pid, uintptr_t bm) {
    if (!esp_runtime_off_ready() || !bm || !mem_ptr_ok(bm)) return -1;
    const EspRuntimeOff &R = esp_runtime_off();
    uintptr_t local = 0;
    mem_read(pid, bm + R.localPlayerShow, local);
    Vec3 self{};
    bool haveSelf = local && mem_read(pid, local + R.lastPosition, self) && pos_ok(self);
    uintptr_t pDict = 0, mDict = 0;
    mem_read(pid, bm + R.dicPlayerShow, pDict);
    mem_read(pid, bm + R.dicMonsterShow, mDict);
    std::vector<std::pair<int, uintptr_t>> players, monsters;
    dict_entries(pid, pDict, players);
    dict_entries(pid, mDict, monsters);
    int usable = 0;
    auto take = [&](uintptr_t v, bool mob, int key) {
        if (!v || v == local) return;
        uint8_t dead = 0, camp = 0;
        if (mem_read(pid, v + R.entityDeath, dead) && dead) return;
        if (mem_read(pid, v + R.sameCamp, camp) && camp) return;
        int id = key;
        mem_read(pid, v + R.entityId, id);
        if (mob && !esp_monster_id(id)) return;
        Vec3 pos{};
        if (!mem_read(pid, v + R.lastPosition, pos) || !pos_ok(pos)) return;
        usable++;
    };
    for (const auto &kv : players) take(kv.second, false, kv.first);
    for (const auto &kv : monsters) take(kv.second, true, kv.first);
    if (usable > 0) return 1000 + usable + (haveSelf ? 50 : 0);
    if (haveSelf) return 10;
    if (mem_ptr_ok(local) || mem_ptr_ok(pDict)) return 1;
    return 0;
}

/* conRent resolves Instance via il2cpp; externally scan game heap for the live object. */
static int score_bm_heap(pid_t pid, uintptr_t bm) {
    const int s = score_bm(pid, bm);
    if (s > 0) return s;
    if (!esp_runtime_off_ready()) return -1;
    const EspRuntimeOff &R = esp_runtime_off();
    uintptr_t pDict = 0;
    if (!mem_read(pid, bm + R.dicPlayerShow, pDict) || !mem_ptr_ok(pDict)) return -1;
    std::vector<std::pair<int, uintptr_t>> players;
    if (!dict_entries(pid, pDict, players) || players.empty()) return -1;
    return 100 + (int)players.size();
}

static bool map_is_game_heap(const MapsReg &m) {
    if (m.perms[0] != 'r' || m.perms[1] != 'w' || m.end <= m.start) return false;
    const size_t sz = (size_t)(m.end - m.start);
    if (sz < 64 * 1024 || sz > 256 * 1024 * 1024) return false;
    if (m.start < 0x100000000ULL) return false;
    const std::string &p = m.path;
    if (p.find(".so") != std::string::npos) return false;
    if (p.find("liblogic") != std::string::npos) return false;
    if (p.find("il2cpp") != std::string::npos) return false;
    if (p.find("stack") != std::string::npos) return false;
    if (p.find("/dev/") == 0) return false;
    if (m.start >= 0x7000000000ULL) return true;
    if (p.empty() || p[0] == '[') return true;
    if (p.find("[anon:") != std::string::npos) return true;
    if (p.find("dalvik") != std::string::npos) return true;
    if (p.find("gspace") != std::string::npos || p.find("GSpace") != std::string::npos)
        return true;
    return false;
}

static bool heap_find_battle_manager(pid_t pid, const std::vector<MapsReg> &maps,
                                     uintptr_t &bmOut, int &bestScoreOut) {
    bmOut = 0;
    bestScoreOut = -1;
    int candidates = 0;
    size_t budget = 0;

    for (const auto &m : maps) {
        if (!map_is_game_heap(m)) continue;
        const size_t sz = (size_t)(m.end - m.start);
        if ((budget += sz) > 96 * 1024 * 1024) break;

        for (uintptr_t a = (m.start + 0x3f) & ~0xFULL; a + 0x300 < m.end; a += 0x80) {
            ++candidates;
            const int sc = score_bm_heap(pid, a);
            if (sc > bestScoreOut) {
                bestScoreOut = sc;
                bmOut = a;
            }
        }
    }

    if (bmOut && bestScoreOut >= 100) {
        for (uintptr_t a = bmOut - 0x200; a < bmOut + 0x200; a += 0x10) {
            const int sc = score_bm_heap(pid, a);
            if (sc > bestScoreOut) {
                bestScoreOut = sc;
                bmOut = a;
            }
        }
    }

    ui_log("[resolve] heap scan (EspTab-style) tried=%d best=%d inst=0x%llx",
           candidates, bestScoreOut, (unsigned long long)bmOut);
    return bmOut != 0 && bestScoreOut >= 100;
}

static bool typeinfo_to_klass(pid_t pid, uintptr_t addr, uintptr_t &klass) {
    klass = 0;
    if (!addr) return false;
    uintptr_t k = 0;
    if (mem_read(pid, addr, k) && mem_ptr_ok(k)) {
        klass = k;
        return true;
    }
    if (mem_ptr_ok(addr)) {
        klass = addr;
        return true;
    }
    return false;
}

static bool elf_bm_klass_in(pid_t pid, uintptr_t base, uintptr_t /*soEnd*/, uintptr_t &klass) {
    klass = 0;
    if (!base) return false;

    const char *names[] = {"BattleManager_TypeInfo", "BattleManager_c"};
    for (const char *sym : names) {
        uintptr_t addr = 0;
        if (!elf_remote_sym(pid, base, sym, addr) || !addr) continue;
        if (typeinfo_to_klass(pid, addr, klass)) return true;
    }

    if (g_il2_for_resolve && g_il2_for_resolve->libPath[0]) {
        ElfRemote er{};
        if (elf_remote_bias(pid, base, er)) {
            char matched[128] = {};
            uintptr_t addr = 0;
            if (elf_file_sym_contains(g_il2_for_resolve->libPath, er.load_bias, "BattleManager",
                                      addr, matched, sizeof(matched))) {
                ui_log("[resolve] ELF sym %s -> 0x%llx", matched,
                       (unsigned long long)addr);
                if (typeinfo_to_klass(pid, addr, klass)) return true;
            }
        }
    }
    return false;
}

static bool elf_bm_klass(pid_t pid, const std::vector<MapsReg> &maps, uintptr_t &klass) {
    klass = 0;
    struct Cand {
        uintptr_t base = 0, end = 0;
        int prio = 0;
    };
    std::vector<Cand> cands;
    for (const auto &m : maps) {
        if (m.fileOff != 0 || m.perms[0] != 'r') continue;
        if (m.path.find(".so") == std::string::npos) continue;
        const std::string &p = m.path;
        int prio = 0;
        if (p.find("liblogic") != std::string::npos) prio = 100;
        else if (p.find("csharp") != std::string::npos) prio = 90;
        else if (p.find("il2cpp") != std::string::npos) prio = 85;
        else if (p.find("GameAssembly") != std::string::npos) prio = 80;
        else continue;
        bool merged = false;
        for (auto &c : cands) {
            if (c.base == m.start) {
                c.end = std::max(c.end, m.end);
                merged = true;
                break;
            }
        }
        if (!merged) cands.push_back({m.start, m.end, prio});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b) { return a.prio > b.prio; });
    for (const Cand &c : cands) {
        if (elf_bm_klass_in(pid, c.base, c.end, klass) && klass) return true;
    }
    return false;
}

static bool find_bm(pid_t pid, const std::vector<MapsReg> &maps,
                    uintptr_t &klass, uintptr_t &statics, uintptr_t &instance,
                    BmFindMeta *meta) {
    klass = statics = instance = 0;
    const char *name = "BattleManager";
    if (meta) {
        meta->nameStrings = 0;
        meta->usedElf = false;
    }

    if (elf_bm_klass(pid, maps, klass)) {
        if (meta) meta->usedElf = true;
        uintptr_t st = 0, inst = 0;
        if (verify_klass(pid, klass, name, 0, st, inst, nullptr)) {
            statics = st;
            instance = inst;
            if (klass) return true;
        }
        klass = 0;
    }

    std::vector<uintptr_t> strs;
    scan_strings(pid, maps, name, strs);
    if (meta) meta->nameStrings = (int)strs.size();
    if (strs.empty()) return false;

    int bestScore = -1;
    static const uintptr_t kNameOffs[] = {0x10, 0x18, 0x08, 0x20};
    for (uintptr_t s : strs) {
        for (uintptr_t nameOff : kNameOffs) {
            const uintptr_t cand = s - nameOff;
            uintptr_t st = 0, inst = 0;
            if (!verify_klass(pid, cand, name, 0, st, inst, &strs)) continue;
            int sc = inst ? score_bm(pid, inst) : (st ? 2 : 0);
            if (sc > bestScore) {
                bestScore = sc;
                klass = cand;
                statics = st;
                instance = inst;
            }
            if (bestScore >= 1000) return true;
        }
    }
    if (klass) return true;

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
                    if (p == s) { hit = true; break; }
                if (!hit) continue;
                for (uintptr_t nameOff : kNameOffs) {
                    const uintptr_t cand = m.start + off + i - nameOff;
                    uintptr_t st = 0, inst = 0;
                    if (!verify_klass(pid, cand, name, 0, st, inst, &strs))
                        continue;
                    int sc = inst ? score_bm(pid, inst) : (st ? 2 : 0);
                    if (sc > bestScore) {
                        bestScore = sc;
                        klass = cand;
                        statics = st;
                        instance = inst;
                    }
                }
            }
        }
    }
    if (klass) return true;

    size_t bruteBudget = 0;
    for (const auto &m : maps) {
        if (m.perms[0] != 'r' || m.perms[1] != 'w') continue;
        const size_t sz = (size_t)(m.end - m.start);
        if ((bruteBudget += sz) > 40 * 1024 * 1024) break;
        for (uintptr_t addr = m.start; addr + 0x100 < m.end; addr += 0x10) {
            if (!klass_matches_name(pid, addr, name, &strs)) continue;
            uintptr_t st = 0, inst = 0;
            if (!statics_for(pid, addr, 0, st, inst)) continue;
            int sc = inst ? score_bm(pid, inst) : 1;
            if (sc > bestScore) {
                bestScore = sc;
                klass = addr;
                statics = st;
                instance = inst;
            }
            if (bestScore >= 1000) return true;
        }
    }
    if (klass) {
        ui_log("[resolve] BattleManager klass brute/static @ 0x%llx inst=0x%llx",
               (unsigned long long)klass, (unsigned long long)instance);
        return true;
    }

    std::vector<const MapsReg *> order;
    for (const auto &m : maps)
        if (klass_region_prio(m) > 0) order.push_back(&m);
    std::sort(order.begin(), order.end(), [](const MapsReg *a, const MapsReg *b) {
        return klass_region_prio(*a) > klass_region_prio(*b);
    });

    size_t scanned = 0;
    for (const MapsReg *mp : order) {
        const MapsReg &m = *mp;
        const size_t sz = (size_t)(m.end - m.start);
        const size_t scan = std::min(sz, (size_t)12 * 1024 * 1024);
        if ((scanned += scan) > 128 * 1024 * 1024) break;
        const size_t chunk = 256 * 1024;
        for (size_t off = 0; off < scan; off += chunk) {
            const size_t n = std::min(chunk, scan - off);
            std::vector<uint8_t> buf(n);
            if (!mem_read(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t p = 0;
                std::memcpy(&p, buf.data() + i, 8);
                bool hit = false;
                for (uintptr_t s : strs)
                    if (p == s) { hit = true; break; }
                if (!hit) continue;
                const uintptr_t cand = m.start + off + i - Il2::KlassName;
                uintptr_t st = 0, inst = 0;
                if (!verify_klass(pid, cand, name, 0, st, inst, &strs))
                    continue;
                int sc = inst ? score_bm(pid, inst) : (st ? 2 : 0);
                if (sc > bestScore) {
                    bestScore = sc;
                    klass = cand;
                    statics = st;
                    instance = inst;
                }
                if (bestScore >= 1000) return klass != 0;
            }
        }
    }
    return klass != 0;
}

static void refresh_instance(pid_t pid) {
    if (!g_bmStatics) return;
    uintptr_t inst = 0;
    /* BattleManager.Instance is the first static. A new match replaces this pointer. */
    if (mem_read(pid, g_bmStatics + 0, inst) && mem_ptr_ok(inst))
        g_bmInstance = inst;
}

/** Reject a stale match-1 BattleManager once the static Instance has moved on. */
static bool bm_container_live(pid_t pid, uintptr_t bm) {
    if (!esp_runtime_off_ready() || !mem_ptr_ok(bm)) return false;
    const EspRuntimeOff &R = esp_runtime_off();
    uintptr_t local = 0, mdict = 0, mlist = 0;
    if (R.localPlayerShow) mem_read(pid, bm + R.localPlayerShow, local);
    if (R.dicMonsterShow) mem_read(pid, bm + R.dicMonsterShow, mdict);
    if (R.listMonsterShow) mem_read(pid, bm + R.listMonsterShow, mlist);
    return mem_ptr_ok(local) && (mem_ptr_ok(mdict) || mem_ptr_ok(mlist));
}

void esp_reset() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_pid > 0)
        remote_il2cpp_invalidate_cache(g_pid);
    g_pid = 0;
    g_bmKlass = g_bmStatics = g_bmInstance = g_bmInstancePin = 0;
    g_resolved = false;
    g_il2SnapOk = false;
    g_tickHeroName.store(true);
    g_tickEnemyCd.store(true);
    g_tickMonsters.store(true);
    g_readerTickIndex.store(0);
    g_cooldownTimers.clear();
    g_heroNameByEnt.clear();
    g_cdByEnt.clear();
    invalidate_read_caches();
    esp_runtime_off_clear();
    esp_camera_reset();
}

EspStatus esp_attach(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (pid != g_pid) {
        if (g_pid > 0)
            remote_il2cpp_invalidate_cache(g_pid);
        g_bmKlass = g_bmStatics = g_bmInstance = g_bmInstancePin = 0;
        g_resolved = false;
        g_il2SnapOk = false;
        g_readerTickIndex.store(0);
        g_cooldownTimers.clear();
        g_heroNameByEnt.clear();
        g_cdByEnt.clear();
        invalidate_read_caches();
    }
    g_pid = pid;
    if (pid <= 0) return EspStatus::NoProcess;
    return EspStatus::Idle;
}

void esp_request_force_resolve() { g_forceResolve.store(true); }

bool esp_is_resolved() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_resolved;
}

static void fill_report(EspResolveReport &r, EspStatus status, int mapsCount, bool probe,
                        const BmFindMeta &meta, uintptr_t klass, uintptr_t statics,
                        uintptr_t inst) {
    r.status = status;
    r.mapsCount = mapsCount;
    r.readProbe = probe;
    r.bmNameStrings = meta.nameStrings;
    r.usedElfTypeInfo = meta.usedElf;
    r.klass = klass;
    r.statics = statics;
    r.instance = inst;
    std::snprintf(r.detail, sizeof(r.detail),
                  "maps=%d probe=%s strings=%d elf=%s klass=0x%llx statics=0x%llx inst=0x%llx",
                  mapsCount, probe ? "OK" : "FAIL", meta.nameStrings,
                  meta.usedElf ? "yes" : "no",
                  (unsigned long long)klass, (unsigned long long)statics,
                  (unsigned long long)inst);
}

static void merge_il2cpp_into_report(EspResolveReport &rep, const RemoteIl2CppInfo &il2) {
    std::snprintf(rep.il2cppLib, sizeof(rep.il2cppLib), "%s",
                  il2.libBaseName[0] ? il2.libBaseName : "(none)");
    rep.il2cppBase = il2.base;
    rep.il2cppDomainGet = il2.sym_domain_get;
    rep.il2cppDomainPtr = il2.domain_ptr;
    std::snprintf(rep.il2cppNote, sizeof(rep.il2cppNote), "%s", il2.note);
}

EspStatus esp_resolve(EspResolveReport *reportOut) {
    std::lock_guard<std::mutex> lock(g_mu);
    EspResolveReport rep{};
    if (g_pid <= 0) {
        rep.status = EspStatus::NoProcess;
        std::snprintf(rep.detail, sizeof(rep.detail), "no pid attached");
        g_lastReport = rep;
        if (reportOut) *reportOut = rep;
        return rep.status;
    }
    std::vector<MapsReg> maps;
    if (!mem_maps(g_pid, maps)) {
        rep.status = EspStatus::NoRead;
        rep.mapsCount = 0;
        rep.readProbe = false;
        std::snprintf(rep.detail, sizeof(rep.detail),
                      "cannot read /proc/%d/maps (root? process dead?)", (int)g_pid);
        g_lastReport = rep;
        if (reportOut) *reportOut = rep;
        return rep.status;
    }
    const bool probe = proc_probe_read(g_pid);
    if (!g_forceResolve.exchange(false) && g_resolved) {
        if (reportOut) *reportOut = g_lastReport;
        return g_lastReport.status;
    }
    RemoteIl2CppInfo il2{};
    remote_detect_il2cpp(g_pid, il2);
    if (!il2.domain_ptr)
        remote_il2cpp_prime_domain(g_pid, il2);
    merge_il2cpp_into_report(rep, il2);
    g_il2_for_resolve = &il2;

    /* conRentV1 fast path only — no slow mem klass walk. */
    ConrentResolve cr{};
    const bool crRan = conrent_resolve_battle_manager(g_pid, maps, il2, cr);
    g_il2_for_resolve = nullptr;
    esp_camera_set_static_bases(cr.show_battle_camera_statics, cr.game_method_statics);
    EspCameraRuntime camRt{};
    camRt.gm_mainCamera = cr.runtime_fields.gm_mainCamera;
    camRt.sf_m_MainCamera = cr.runtime_fields.sf_m_MainCamera;
    camRt.sbc_m_CamPos = cr.runtime_fields.sbc_m_CamPos;
    camRt.sbc_m_CamRot = cr.runtime_fields.sbc_m_CamRot;
    camRt.sbc_m_iFieldOfView = cr.runtime_fields.sbc_m_iFieldOfView;
    camRt.camera_world_to_camera_method = cr.camera_world_to_camera_method;
    camRt.camera_projection_matrix_method = cr.camera_projection_matrix_method;
    camRt.matrix_lib_base = cr.main_camera_matrix_lib_base;
    camRt.matrix_main_camera_rva = cr.auto_rva_main_camera_root;
    camRt.main_camera_global_slot_va = cr.main_camera_global_slot_va;
    camRt.main_camera_slot_live_ok = cr.main_camera_slot_live_ok;
    camRt.game_method_main_camera_obj = cr.game_method_main_camera_obj;
    esp_camera_set_runtime(camRt);
    invalidate_read_caches();
    esp_camera_set_il2cpp_w2s(g_pid, il2.sym_runtime_invoke, cr.camera_get_main_method,
                              cr.camera_w2s_method);
    esp_retri_il2_bind(il2.sym_runtime_invoke, cr.retri_try_use_skill_mi);
    if (cr.retri_try_use_skill_mi) {
        ui_log("[retri] IL2 ShowSelfPlayer.TryUseSkill mi=0x%llx (no root / no a11y)",
               (unsigned long long)cr.retri_try_use_skill_mi);
    } else {
        ui_log("[retri] WARN: TryUseSkill not resolved — enable Resolve in match");
    }

    if (crRan && (cr.battle_manager || cr.battle_manager_klass || cr.metadata_ready)) {
        g_bmKlass = cr.battle_manager_klass;
        g_bmStatics = cr.battle_manager_statics;
        g_bmInstance = cr.battle_manager;
        if (cr.battle_manager) g_bmInstancePin = cr.battle_manager;
        g_il2Snap = il2;
        g_il2SnapOk = crRan && il2.sym_field_static_get_value != 0;
        esp_runtime_off_apply(cr);
        g_resolved = esp_runtime_off_ready();
        BmFindMeta meta{};
        const EspStatus st = !g_resolved ? EspStatus::NoBattleManager
                            : cr.battle_manager ? EspStatus::Ready
                                                : EspStatus::NoInstance;
        fill_report(rep, st, (int)maps.size(), probe, meta, cr.battle_manager_klass,
                    cr.battle_manager_statics, cr.battle_manager);
        std::snprintf(rep.detail, sizeof(rep.detail), "conRentV1 fast: %s", cr.detail);
        ui_log("[resolve] OK %s", cr.detail);
        g_lastReport = rep;
        if (reportOut) *reportOut = rep;
        return rep.status;
    }

    g_resolved = false;
    g_il2SnapOk = false;
    BmFindMeta meta{};
    fill_report(rep, EspStatus::NoBattleManager, (int)maps.size(), probe, meta, 0, 0, 0);
    if (cr.detail[0]) ui_log("[resolve] conRent: %s", cr.detail);
    ui_log("[resolve] failed — open classic match, tap Resolve again");
    g_lastReport = rep;
    if (reportOut) *reportOut = rep;
    return rep.status;
}

const EspResolveReport &esp_last_resolve_report() {
    return g_lastReport;
}

EspStatus esp_tick(EspFrame &out) {
    std::lock_guard<std::mutex> lock(g_mu);
    out = EspFrame{};
    out.pid = g_pid;
    if (g_pid <= 0) {
        out.status = EspStatus::NoProcess;
        std::snprintf(out.hint, sizeof(out.hint), "No MLBB pid");
        return out.status;
    }
    if (!g_resolved) {
        out.status = EspStatus::NoBattleManager;
        std::snprintf(out.hint, sizeof(out.hint), "Tap Resolve (in match)");
        return out.status;
    }
    refresh_instance(g_pid);
    uintptr_t bm = 0;
    if (g_bmInstance && bm_container_live(g_pid, g_bmInstance)) {
        if (g_bmInstancePin && g_bmInstance != g_bmInstancePin) {
            ui_log("[bm] match instance 0x%llx -> 0x%llx",
                   (unsigned long long)g_bmInstancePin, (unsigned long long)g_bmInstance);
            invalidate_read_caches();
        }
        g_bmInstancePin = g_bmInstance;
        bm = g_bmInstance;
    } else {
        bm = g_bmInstancePin ? g_bmInstancePin : g_bmInstance;
    }
    if (bm) g_bmInstance = bm;
    if (!bm) {
        out.status = EspStatus::NoInstance;
        std::snprintf(out.hint, sizeof(out.hint), "BattleManager null — enter match");
        return out.status;
    }
    if (!esp_runtime_off_ready()) {
        out.status = EspStatus::NoBattleManager;
        std::snprintf(out.hint, sizeof(out.hint), "Resolve IL2CPP field offsets (in match)");
        return out.status;
    }
    const EspRuntimeOff &R = esp_runtime_off();

    uintptr_t local = 0;
    mem_read(g_pid, bm + R.localPlayerShow, local);
    out.localPlayerEnt = local;
    Vec3 self{};
    if (local) read_entity_pos(g_pid, local, self);
    out.selfX = self.x;
    out.selfY = self.y;
    out.selfZ = self.z;
    out.bmInstance = bm;
    out.bmKlass = g_bmKlass;
    out.bmStatics = g_bmStatics;
    out.status = EspStatus::Ready;
    if (!g_tickBuildTargets.load()) {
        std::snprintf(out.hint, sizeof(out.hint), "OK");
        return out.status;
    }

    uintptr_t pDict = 0;
    mem_read(g_pid, bm + R.dicPlayerShow, pDict);
    const bool wantMonsters = g_tickMonsters.load();
    const auto dictNow = std::chrono::steady_clock::now();
    const bool dictDue = !g_dictSnap.valid || g_dictSnap.bm != bm ||
                         dictNow - g_dictSnap.at >= std::chrono::milliseconds(250);
    std::vector<std::pair<int, uintptr_t>> players, monsters;
    if (dictDue) {
        dict_entries(g_pid, pDict, players);
        if (wantMonsters) esp_monster_dict_entries(g_pid, bm, monsters);
        if (players.empty() && R.listPlayerShow) {
            uintptr_t pList = 0;
            mem_read(g_pid, bm + R.listPlayerShow, pList);
            std::vector<uintptr_t> listP;
            if (list_entities(g_pid, pList, listP))
                for (size_t i = 0; i < listP.size(); ++i) players.emplace_back((int)i, listP[i]);
        }
        g_dictSnap.bm = bm;
        g_dictSnap.pDict = pDict;
        g_dictSnap.players = players;
        g_dictSnap.monsters = monsters;
        g_dictSnap.dictPlayers = (int)players.size();
        g_dictSnap.dictMonsters = (int)monsters.size();
        g_dictSnap.at = dictNow;
        g_dictSnap.valid = true;
    } else {
        pDict = g_dictSnap.pDict ? g_dictSnap.pDict : pDict;
        players = g_dictSnap.players;
        monsters = g_dictSnap.monsters;
    }
    const uint64_t tick = g_readerTickIndex.fetch_add(1, std::memory_order_relaxed);
    const bool slowNames = dictDue;

    std::vector<uintptr_t> fastEnts;
    fastEnts.reserve(players.size() + monsters.size());
    for (const auto &kv : players)
        if (kv.second) fastEnts.push_back(kv.second);
    for (const auto &kv : monsters)
        if (kv.second) fastEnts.push_back(kv.second);
    struct EntFastRead {
        bool have = false;
        uint8_t dead = 0;
        uint8_t camp = 0;
        int id = 0;
        int hp = 0;
        int hpMax = 0;
        Vec3 pos{};
        bool posOk = false;
    };
    std::unordered_map<uintptr_t, EntFastRead> pref;
    {
        const uintptr_t posOff = R.vCachePosition ? R.vCachePosition
                                : R.cachePosition ? R.cachePosition
                                                  : R.lastPosition;
        std::vector<std::pair<uintptr_t, size_t>> req;
        struct Ix {
            int dead = -1, camp = -1, id = -1, hp = -1, hpMax = -1, pos = -1;
        };
        std::vector<Ix> ix(fastEnts.size());
        auto add = [&](int &slot, uintptr_t addr, size_t n) {
            if (!addr) return;
            slot = (int)req.size();
            req.emplace_back(addr, n);
        };
        for (size_t i = 0; i < fastEnts.size(); ++i) {
            const uintptr_t e = fastEnts[i];
            if (R.entityDeath) add(ix[i].dead, e + R.entityDeath, 1);
            if (R.sameCamp) add(ix[i].camp, e + R.sameCamp, 1);
            if (R.entityId) add(ix[i].id, e + R.entityId, 4);
            if (R.hp) add(ix[i].hp, e + R.hp, 4);
            if (R.hpMax) add(ix[i].hpMax, e + R.hpMax, 4);
            if (posOff) add(ix[i].pos, e + posOff, sizeof(Vec3));
        }
        std::vector<std::vector<uint8_t>> raw;
        if (!req.empty() && mem_read_batch(g_pid, req, raw) && raw.size() == req.size()) {
            auto take = [&](int slot, void *dst, size_t n) -> bool {
                if (slot < 0 || (size_t)slot >= raw.size() || raw[(size_t)slot].size() < n)
                    return false;
                std::memcpy(dst, raw[(size_t)slot].data(), n);
                return true;
            };
            for (size_t i = 0; i < fastEnts.size(); ++i) {
                EntFastRead f{};
                f.have = true;
                take(ix[i].dead, &f.dead, 1);
                take(ix[i].camp, &f.camp, 1);
                take(ix[i].id, &f.id, 4);
                take(ix[i].hp, &f.hp, 4);
                take(ix[i].hpMax, &f.hpMax, 4);
                if (take(ix[i].pos, &f.pos, sizeof(Vec3)) && pos_ok(f.pos)) f.posOk = true;
                pref[fastEnts[i]] = f;
            }
        }
    }

    auto push = [&](uintptr_t v, int kind, const char *labelOverride, int key) {
        if (!v) return;
        const EntFastRead *fast = nullptr;
        const auto fit = pref.find(v);
        if (fit != pref.end() && fit->second.have) fast = &fit->second;
        uint8_t dead = 0, camp = 0;
        if (fast) {
            dead = fast->dead;
            camp = fast->camp;
        } else {
            if (mem_read(g_pid, v + R.entityDeath, dead) && dead) return;
            if (mem_read(g_pid, v + R.sameCamp, camp) && camp) return;
        }
        if (dead) return;
        if (camp) return;
        int id = key;
        if (!(kind == 1 && esp_monster_id(key))) {
            int live = fast ? fast->id : 0;
            if (!fast && R.entityId) mem_read(g_pid, v + R.entityId, live);
            if (live) id = live;
        }
        if (kind == 1 && !esp_monster_id(id)) return;
        Vec3 pos{};
        if (fast && fast->posOk) pos = fast->pos;
        else if (!read_entity_pos(g_pid, v, pos)) return;
        EspTarget t{};
        t.ent = v;
        t.worldX = pos.x;
        t.worldY = pos.y;
        t.worldZ = pos.z;
        t.relX = pos.x - self.x;
        t.relZ = pos.z - self.z;
        t.meters = (int)std::sqrt(t.relX * t.relX + t.relZ * t.relZ);
        if (fast) {
            t.hp = fast->hp;
            t.hpMax = fast->hpMax;
        } else {
            mem_read(g_pid, v + R.hp, t.hp);
            mem_read(g_pid, v + R.hpMax, t.hpMax);
        }
        t.kind = kind;
        if (kind == 0 && g_tickFacing.load() && R.faceDir && !(local && v == local)) {
            float dir[3] = {};
            if (mem_read(g_pid, v + R.faceDir, dir, sizeof(dir))) {
                const float h = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
                if (h > 0.05f) {
                    t.faceX = dir[0] / h;
                    t.faceZ = dir[2] / h;
                    t.faceValid = true;
                }
            }
        }
        if (labelOverride)
            std::snprintf(t.label, sizeof(t.label), "%s", labelOverride);
        else if (kind == 1)
            std::snprintf(t.label, sizeof(t.label), "%s", esp_monster_label(id));
        else {
            const bool isLocal = local && (v == local);
            const unsigned phase = static_cast<unsigned>(tick % kHeavyReadInterval);
            const unsigned mix =
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(v) ^ static_cast<uintptr_t>(tick >> 2u));
            const bool doHeavyName = slowNames && g_tickHeroName.load() && R.heroName
                && (isLocal || ((mix % kHeavyReadInterval) == phase));
            const bool doHeavyCd = slowNames && g_tickEnemyCd.load() && !isLocal
                && (((mix + 1u) % kHeavyReadInterval) == phase);

            t.label[0] = '\0';
            if (doHeavyName && R.heroName) {
                uintptr_t strPtr = 0;
                if (mem_read(g_pid, v + R.heroName, strPtr) && strPtr && mem_ptr_ok(strPtr)) {
                    char hn[180] = {};
                    if (read_il2_managed_string(g_pid, strPtr, hn, sizeof(hn))) {
                        std::snprintf(t.label, sizeof(t.label), "%s", hn);
                        HeroNameEntCache hc{};
                        std::snprintf(hc.name, sizeof(hc.name), "%s", hn);
                        hc.valid = true;
                        g_heroNameByEnt[v] = hc;
                    }
                }
            } else if (g_tickHeroName.load() && R.heroName && !isLocal) {
                const auto it = g_heroNameByEnt.find(v);
                if (it != g_heroNameByEnt.end() && it->second.valid && it->second.name[0])
                    std::snprintf(t.label, sizeof(t.label), "%s", it->second.name);
            }
            if (!t.label[0])
                std::snprintf(t.label, sizeof(t.label), "P%d", id);
            int spellId = 0;
            if (doHeavyCd && R.summonSkillId) mem_read(g_pid, v + R.summonSkillId, spellId);
            if (!isLocal) {
                if (doHeavyCd) {
                    if (!fill_show_cooldowns(g_pid, v, spellId, t, true)) g_cdByEnt.erase(v);
                } else {
                    (void)fill_show_cooldowns(g_pid, v, spellId, t, false);
                }
            }
        }
        out.targets.push_back(t);
    };

    for (const auto &kv : players) push(kv.second, 0, nullptr, kv.first);
    for (const auto &kv : monsters) {
        int id = kv.first;
        if (!esp_monster_id(id) && kv.second && R.entityId) {
            int live = 0;
            if (mem_read(g_pid, kv.second + R.entityId, live)) id = live;
        }
        if (!esp_monster_id(id)) continue;
        push(kv.second, 1, esp_monster_label(id), id);
    }

    {
        std::unordered_set<uintptr_t> live;
        live.reserve(out.targets.size() * 2 + 8);
        for (const EspTarget &tg : out.targets) {
            if (tg.ent) live.insert(tg.ent);
        }
        auto prune = [&live](std::unordered_map<uintptr_t, HeroNameEntCache> &m) {
            for (auto it = m.begin(); it != m.end();) {
                if (live.find(it->first) == live.end()) it = m.erase(it);
                else ++it;
            }
        };
        auto pruneCd = [&live](std::unordered_map<uintptr_t, CdEntCache> &m) {
            for (auto it = m.begin(); it != m.end();) {
                if (live.find(it->first) == live.end()) it = m.erase(it);
                else ++it;
            }
        };
        prune(g_heroNameByEnt);
        pruneCd(g_cdByEnt);
    }

    out.mapsCount = 0;
    out.readProbe = probe_read_cached(g_pid);
    out.bmKlass = g_bmKlass;
    out.bmStatics = g_bmStatics;
    out.bmInstance = bm;
    out.playerDict = pDict;
    uintptr_t mDict = 0;
    if (!dictDue && g_dictSnap.mDict) mDict = g_dictSnap.mDict;
    else if (R.dicMonsterShow) mem_read(g_pid, bm + R.dicMonsterShow, mDict);
    out.monsterDict = mDict;
    out.dictPlayers = (int)players.size();
    out.dictMonsters = (int)monsters.size();

    out.status = EspStatus::Ready;
    std::snprintf(out.hint, sizeof(out.hint), "OK  %zu targets  dict P%d M%d",
                  out.targets.size(), out.dictPlayers, out.dictMonsters);
    return out.status;
}

namespace {

constexpr int kScreenHoldFrames = 0;
/** Beyond this distance (m), W2S runs on a staggered schedule to cut vm_read / matrix work. */
constexpr int kW2sNearMeters = 22;
constexpr unsigned kW2sFarStagger = 1u;

static pid_t g_screenPid = 0;
static uint32_t g_w2sFrame = 0;
static float g_selfSx = 0.f, g_selfSy = 0.f;
static bool g_haveSelf = false;
static int g_selfMiss = 0;

struct EntScreenHold {
    float x = 0.f;
    float y = 0.f;
    int miss = 0;
};
static std::unordered_map<uintptr_t, EntScreenHold> g_entScreen;

} // namespace

void esp_project_screens(EspFrame &frame, float screenW, float screenH) {
    frame.selfScreenValid = false;
    for (EspTarget &t : frame.targets) t.screenValid = false;
    if (frame.status != EspStatus::Ready || frame.pid <= 0 || screenW < 1.f || screenH < 1.f)
        return;

    if (g_screenPid != frame.pid) {
        g_screenPid = frame.pid;
        g_entScreen.clear();
        g_haveSelf = false;
        g_selfMiss = 0;
    }

    static uintptr_t s_projBm = 0;
    static uint32_t s_sinceRefresh = 6;
    static bool s_matrixOk = false;
    bool refresh = !s_matrixOk || frame.bmInstance != s_projBm || s_sinceRefresh >= 6;
    if (frame.bmInstance != s_projBm) s_projBm = frame.bmInstance;

    ++g_w2sFrame;
    esp_camera_frame_begin(frame.pid, screenW, screenH, refresh);
    if (refresh) s_sinceRefresh = 0;
    else ++s_sinceRefresh;
    EspCameraProjSnap camSnap{};
    esp_camera_acquire_proj_for_draw(frame.pid, camSnap);
    s_matrixOk = camSnap.validMatrixProj;
    float sx = 0.f, sy = 0.f;
    const bool selfOk =
        camSnap.validMatrixProj
            ? esp_camera_proj_snap_w2s(camSnap, frame.selfX, frame.selfY, frame.selfZ, sx, sy)
            : esp_camera_world_to_screen(frame.selfX, frame.selfY, frame.selfZ, sx, sy);
    if (selfOk) {
        frame.selfScreenX = sx;
        frame.selfScreenY = sy;
        g_selfSx = sx;
        g_selfSy = sy;
        g_haveSelf = true;
        g_selfMiss = 0;
        frame.selfScreenValid = true;
    } else if (g_haveSelf && g_selfMiss < kScreenHoldFrames) {
        ++g_selfMiss;
        frame.selfScreenX = g_selfSx;
        frame.selfScreenY = g_selfSy;
        frame.selfScreenValid = true;
    }

    /* Two passes (players then monsters) — avoids per-frame sort; order within a kind stays stable. */
    std::unordered_map<uintptr_t, EntScreenHold> live;
    for (int kindPass = 0; kindPass <= 1; ++kindPass) {
        for (size_t idx = 0; idx < frame.targets.size(); ++idx) {
            if (frame.targets[idx].kind != kindPass) continue;
            EspTarget &t = frame.targets[idx];
            if (!t.ent) continue;
            const auto it = g_entScreen.find(t.ent);
            const bool hadPrev = it != g_entScreen.end();
            const unsigned mix = static_cast<unsigned>(t.ent >> 4) ^ g_w2sFrame;
            const bool needW2s =
                t.meters <= kW2sNearMeters || (mix % kW2sFarStagger) == 0u;
            if (!needW2s && hadPrev) {
                t.screenX = it->second.x;
                t.screenY = it->second.y;
                t.screenValid = true;
                live[t.ent] = it->second;
            } else {
            const bool ok =
                camSnap.validMatrixProj
                    ? esp_camera_proj_snap_w2s(camSnap, t.worldX, t.worldY, t.worldZ, sx, sy)
                    : esp_camera_world_to_screen(t.worldX, t.worldY, t.worldZ, sx, sy);
            if (ok) {
                t.screenX = sx;
                t.screenY = sy;
                t.screenValid = true;
                live[t.ent] = {sx, sy, 0};
            } else if (hadPrev && it->second.miss < kScreenHoldFrames) {
                EntScreenHold st = it->second;
                ++st.miss;
                t.screenX = st.x;
                t.screenY = st.y;
                t.screenValid = true;
                live[t.ent] = st;
            }
            }
            if (t.faceValid && t.screenValid) {
                const float tipX = t.worldX + t.faceX * 4.5f;
                const float tipZ = t.worldZ + t.faceZ * 4.5f;
                float fx = 0.f, fy = 0.f;
                const bool tipOk =
                    camSnap.validMatrixProj
                        ? esp_camera_proj_snap_w2s(camSnap, tipX, t.worldY, tipZ, fx, fy)
                        : esp_camera_world_to_screen(tipX, t.worldY, tipZ, fx, fy);
                if (tipOk) {
                    t.faceScreenX = fx;
                    t.faceScreenY = fy;
                    t.faceScreenValid = true;
                }
            }
        }
    }
    g_entScreen.swap(live);
    esp_camera_frame_end();
}

bool esp_read_entity_world(pid_t pid, uintptr_t ent, float &x, float &y, float &z) {
    Vec3 p{};
    if (!read_entity_pos(pid, ent, p)) return false;
    x = p.x;
    y = p.y;
    z = p.z;
    return true;
}

bool esp_il2_managed_list(pid_t pid, uintptr_t listObj, std::vector<uintptr_t> &out) {
    return list_entities(pid, listObj, out);
}

uintptr_t esp_bm_static_fields_ptr(pid_t pid, uintptr_t bmKlass) {
    if (!bmKlass) return 0;
    for (uintptr_t off : Il2::StaticFieldOffs) {
        uintptr_t st = 0;
        if (mem_read(pid, bmKlass + off, st) && mem_ptr_ok(st)) return st;
    }
    return 0;
}

bool esp_il2_array_obj_ptrs(pid_t pid, uintptr_t arrObj, std::vector<uintptr_t> &out,
                            int maxItems) {
    out.clear();
    if (!arrObj || !mem_ptr_ok(arrObj) || maxItems <= 0) return false;
    int maxLength = 0;
    if (!mem_read(pid, arrObj + Il2::ArrMaxLen, maxLength) || maxLength <= 0 ||
        maxLength > 4096)
        return false;
    const int n = std::min(maxLength, maxItems);
    const uintptr_t data = arrObj + Il2::ArrData;
    std::vector<uint8_t> raw((size_t)n * sizeof(uintptr_t));
    if (!mem_read(pid, data, raw.data(), raw.size())) return false;
    for (int i = 0; i < n; ++i) {
        uintptr_t p = 0;
        std::memcpy(&p, raw.data() + (size_t)i * sizeof(uintptr_t), sizeof(p));
        if (p && mem_ptr_ok(p)) out.push_back(p);
    }
    return !out.empty();
}

bool esp_il2_better_list_ptrs(pid_t pid, uintptr_t listObj, std::vector<uintptr_t> &out) {
    out.clear();
    if (!listObj || !mem_ptr_ok(listObj)) return false;
    if (esp_il2_managed_list(pid, listObj, out)) return true;
    uintptr_t buffer = 0;
    int size = 0;
    if (mem_read(pid, listObj + 0x10, buffer) && mem_read(pid, listObj + 0x18, size) && buffer &&
        size > 0 && size <= 512) {
        if (esp_il2_array_obj_ptrs(pid, buffer, out, size)) return true;
    }
    if (mem_ptr_ok(listObj))
        return esp_il2_array_obj_ptrs(pid, listObj, out, 256);
    return false;
}

/** Template id from the resolved ShowMonster.m_Config → CData_Monster_Element.m_ID. */
static int read_monster_template_id(pid_t pid, uintptr_t ent, const EspRuntimeOff &R) {
    if (R.monsterConfig && R.monsterTableId) {
        uintptr_t cfg = 0;
        int cid = 0;
        if (mem_read(pid, ent + R.monsterConfig, cfg) && mem_ptr_ok(cfg) &&
            mem_read(pid, cfg + R.monsterTableId, cid) && esp_monster_id(cid))
            return cid;
    }
    int id = 0;
    if (R.entityId) mem_read(pid, ent + R.entityId, id);
    return esp_monster_id(id) ? id : 0;
}

bool esp_monster_dict_entries(pid_t pid, uintptr_t bmInstance,
                              std::vector<std::pair<int, uintptr_t>> &out) {
    if (!esp_runtime_off_ready() || !bmInstance) return false;
    const auto now = std::chrono::steady_clock::now();
    if (g_monSnap.valid && g_monSnap.pid == pid && g_monSnap.bm == bmInstance &&
        !g_monSnap.entries.empty() && now - g_monSnap.at < std::chrono::milliseconds(250)) {
        out = g_monSnap.entries;
        return true;
    }
    const EspRuntimeOff &R = esp_runtime_off();
    out.clear();
    std::unordered_set<uintptr_t> seen;

    auto add_ent = [&](uintptr_t ent) {
        if (!ent || !mem_ptr_ok(ent) || !seen.insert(ent).second) return;
        out.emplace_back(read_monster_template_id(pid, ent, R), ent);
    };

    if (!R.dicMonsterShow) return false;
    uintptr_t mDict = 0;
    std::vector<std::pair<int, uintptr_t>> dictM;
    if (!mem_read(pid, bmInstance + R.dicMonsterShow, mDict) || !mDict ||
        !dict_entries(pid, mDict, dictM))
        return false;
    for (const auto &kv : dictM) add_ent(kv.second);
    if (!out.empty()) {
        g_monSnap.pid = pid;
        g_monSnap.bm = bmInstance;
        g_monSnap.entries = out;
        g_monSnap.at = now;
        g_monSnap.valid = true;
        if (g_dictSnap.bm == bmInstance) g_dictSnap.mDict = mDict;
    }
    return !out.empty();
}

bool esp_resolved_il2(RemoteIl2CppInfo &out) {
    if (!g_il2SnapOk.load(std::memory_order_acquire)) return false;
    out = g_il2Snap;
    return out.sym_field_static_get_value != 0;
}
