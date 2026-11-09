#include "esp_monid.hpp"
#include "esp_monster.hpp"
#include "esp_runtime_off.hpp"
#include "il2_layout.hpp"
#include "mem.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

static std::atomic<bool> g_on{false};

struct MonRow {
    int key = 0;
    int entId = 0;
    int cfgId = 0;
    int transId = 0;
    int transCfgId = 0;
    int hp = 0;
    int hpMax = 0;
    float dist = 1e9f;
    char name[40] = {};
    char prefab[48] = {};
};

struct MonSnap {
    bool ready = false;
    int total = 0;
    char note[96] = {};
    MonRow row[16]{};
    int shown = 0;
};

static std::mutex g_mu;
static MonSnap g_snap;

void esp_monid_set_enabled(bool on) { g_on.store(on); }

static bool dict_entries(pid_t pid, uintptr_t dict, std::vector<std::pair<int, uintptr_t>> &out) {
    out.clear();
    if (!dict) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!mem_read(pid, dict + Il2::DictEntries, entries) || !entries) return false;
    if (!mem_read(pid, dict + Il2::DictCount, count) || count <= 0 || count > 1024) return false;
    int maxLength = 0;
    if (!mem_read(pid, entries + Il2::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 4096)
        return false;
    const uintptr_t data = entries + Il2::ArrData;
    std::vector<uint8_t> raw((size_t)maxLength * Il2::EntryStride);
    if (!mem_read(pid, data, raw.data(), raw.size())) return false;
    for (int i = 0; i < maxLength; ++i) {
        const uint8_t *base = raw.data() + (size_t)i * Il2::EntryStride;
        int hash = 0, key = 0;
        uintptr_t value = 0;
        std::memcpy(&hash, base + Il2::EntryHash, sizeof(hash));
        if (hash < 0) continue;
        std::memcpy(&key, base + Il2::EntryKey, sizeof(key));
        std::memcpy(&value, base + Il2::EntryValue, sizeof(value));
        if (!value || !mem_ptr_ok(value)) continue;
        out.emplace_back(key, value);
        if ((int)out.size() >= count) break;
    }
    return !out.empty();
}

static bool read_str(pid_t pid, uintptr_t obj, uintptr_t off, char *out, size_t n) {
    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!obj || !off) return false;
    uintptr_t str = 0;
    if (!mem_read(pid, obj + off, str) || !mem_ptr_ok(str)) return false;
    alignas(4) uint8_t blob[4 + 40 * 2];
    if (!mem_read(pid, str + 0x10, blob, sizeof(blob))) return false;
    int32_t len = 0;
    std::memcpy(&len, blob, sizeof(len));
    if (len <= 0 || len > 40) return false;
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 1 < n; ++i) {
        uint16_t cu = 0;
        std::memcpy(&cu, blob + 4 + (size_t)i * 2, 2);
        if (cu < 0x80) out[o++] = (char)cu;
        else if (o + 2 < n) {
            out[o++] = '?';
        }
    }
    out[o < n ? o : n - 1] = '\0';
    return out[0] != '\0';
}

static int read_i32(pid_t pid, uintptr_t obj, uintptr_t off) {
    int v = 0;
    if (obj && off) mem_read(pid, obj + off, v);
    return v;
}

void esp_monid_tick(const EspFrame &frame) {
    if (!g_on.load()) return;
    MonSnap snap;
    const EspRuntimeOff &R = esp_runtime_off();
    if (!esp_runtime_off_ready() || !R.monsterConfig || !R.monsterTableId || !R.entityId) {
        std::snprintf(snap.note, sizeof(snap.note), "Resolve first. Turn ESP off, then Resolve.");
    } else if (frame.status != EspStatus::Ready || frame.pid <= 0 || !frame.bmInstance) {
        std::snprintf(snap.note, sizeof(snap.note), "Not in a match.");
    } else {
        snap.ready = true;
        uintptr_t dict = 0;
        std::vector<std::pair<int, uintptr_t>> rows;
        if (!mem_read(frame.pid, frame.bmInstance + R.dicMonsterShow, dict) ||
            !dict_entries(frame.pid, dict, rows)) {
            std::snprintf(snap.note, sizeof(snap.note), "m_dicMonsterShow is empty.");
        } else {
            snap.total = (int)rows.size();
            std::vector<MonRow> all;
            all.reserve(rows.size());
            for (const auto &kv : rows) {
                MonRow m;
                m.key = kv.first;
                m.entId = read_i32(frame.pid, kv.second, R.entityId);
                m.hp = read_i32(frame.pid, kv.second, R.hp);
                m.hpMax = read_i32(frame.pid, kv.second, R.hpMax);
                uintptr_t cfg = 0;
                if (mem_read(frame.pid, kv.second + R.monsterConfig, cfg) && mem_ptr_ok(cfg)) {
                    m.cfgId = read_i32(frame.pid, cfg, R.monsterTableId);
                    m.transId = read_i32(frame.pid, cfg, R.monsterTransId);
                    read_str(frame.pid, cfg, R.monsterName, m.name, sizeof(m.name));
                    read_str(frame.pid, cfg, R.monsterPrefab, m.prefab, sizeof(m.prefab));
                }
                if (R.monsterTransConfig) {
                    uintptr_t tcfg = 0;
                    if (mem_read(frame.pid, kv.second + R.monsterTransConfig, tcfg) &&
                        mem_ptr_ok(tcfg))
                        m.transCfgId = read_i32(frame.pid, tcfg, R.monsterTableId);
                }
                float x = 0, y = 0, z = 0;
                if (esp_read_entity_world(frame.pid, kv.second, x, y, z)) {
                    const float dx = x - frame.selfX;
                    const float dz = z - frame.selfZ;
                    m.dist = std::sqrt(dx * dx + dz * dz);
                }
                all.push_back(m);
            }
            std::sort(all.begin(), all.end(),
                      [](const MonRow &a, const MonRow &b) { return a.dist < b.dist; });
            snap.shown = (int)std::min(all.size(), sizeof(snap.row) / sizeof(snap.row[0]));
            for (int i = 0; i < snap.shown; ++i) snap.row[i] = all[(size_t)i];
            std::snprintf(snap.note, sizeof(snap.note), "%d in m_dicMonsterShow, closest %d",
                          snap.total, snap.shown);
        }
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_snap = snap;
}

void esp_monid_draw() {
    MonSnap snap;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        snap = g_snap;
    }
    ImGui::SeparatorText("Developer — monster ids");
    ImGui::TextWrapped(
        "Live m_dicMonsterShow. Dump fields: ShowEntity.m_ID, ShowMonster.m_Config -> "
        "CData_Monster_Element.m_ID, m_TransId, m_MonsterName, m_Prefabs, m_TransConfig.");
    ImGui::TextDisabled("This build maps 2002 Lord, 2003/2110 Turtle, 2004/2220 Buff Red,");
    ImGui::TextDisabled("2005/2221 Buff Blue, 2011/2013/2222/2223 Crab, 2056/2072 Litho.");
    ImGui::Text("%s", snap.note[0] ? snap.note : "Open this page in a match.");
    if (!snap.ready) return;
    ImGui::BeginChild("monid", ImVec2(0, 0), true);
    for (int i = 0; i < snap.shown; ++i) {
        const MonRow &m = snap.row[i];
        const char *tag = esp_monster_id(m.cfgId) ? esp_monster_label(m.cfgId) :
                          esp_monster_id(m.entId) ? esp_monster_label(m.entId) : "unmapped";
        if (m.dist < 9000.f)
            ImGui::Text("%s   %.1fm", tag, m.dist);
        else
            ImGui::Text("%s   dist —", tag);
        ImGui::Text("  cfg %d   ent %d   trans %d   transCfg %d", m.cfgId, m.entId, m.transId,
                    m.transCfgId);
        ImGui::Text("  hp %d/%d   key %d", m.hp, m.hpMax, m.key);
        if (m.name[0] || m.prefab[0])
            ImGui::Text("  name \"%s\"  prefab \"%s\"", m.name, m.prefab);
        ImGui::Separator();
    }
    ImGui::EndChild();
}
