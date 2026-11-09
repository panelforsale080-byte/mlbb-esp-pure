#include "esp_monid.hpp"
#include "clip_android.hpp"
#include "esp_camera.hpp"
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
#include <unordered_map>
#include <vector>

static std::atomic<bool> g_on{false};
static std::atomic<bool> g_world{false};

struct MonRow {
    uintptr_t ent = 0;
    int key = 0;
    int entId = 0;
    int cfgId = 0;
    int transId = 0;
    int transCfgId = 0;
    int hp = 0;
    int hpMax = 0;
    float dist = 1e9f;
    float wx = 0, wy = 0, wz = 0;
    float sx = 0, sy = 0;
    bool havePos = false;
    bool onScreen = false;
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
bool esp_monid_world_on() { return g_world.load(); }

struct NameCache {
    char name[40] = {};
    char prefab[48] = {};
};
static std::unordered_map<uintptr_t, NameCache> g_names;

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
            /* Dump layout next to the already-resolved fields. No extra class walk.
             * CData_Monster_Element: m_ID, m_TransId +4, m_MonsterName +0xC, m_Prefabs +0x1C.
             * ShowMonster.m_TransConfig is the pointer after m_Config. */
            const uintptr_t idOff = R.monsterTableId;
            const uintptr_t transOff = idOff + 4;
            const uintptr_t nameOff = idOff + 0x0C;
            const uintptr_t prefabOff = idOff + 0x1C;
            const uintptr_t transCfgOff = R.monsterConfig + 8;
            for (const auto &kv : rows) {
                MonRow m;
                m.ent = kv.second;
                m.key = kv.first;
                m.entId = read_i32(frame.pid, kv.second, R.entityId);
                m.hp = read_i32(frame.pid, kv.second, R.hp);
                m.hpMax = read_i32(frame.pid, kv.second, R.hpMax);
                uintptr_t cfg = 0;
                if (mem_read(frame.pid, kv.second + R.monsterConfig, cfg) && mem_ptr_ok(cfg)) {
                    m.cfgId = read_i32(frame.pid, cfg, idOff);
                    m.transId = read_i32(frame.pid, cfg, transOff);
                }
                uintptr_t tcfg = 0;
                if (mem_read(frame.pid, kv.second + transCfgOff, tcfg) && mem_ptr_ok(tcfg))
                    m.transCfgId = read_i32(frame.pid, tcfg, idOff);
                if (esp_read_entity_world(frame.pid, kv.second, m.wx, m.wy, m.wz)) {
                    m.havePos = true;
                    const float dx = m.wx - frame.selfX;
                    const float dz = m.wz - frame.selfZ;
                    m.dist = std::sqrt(dx * dx + dz * dz);
                }
                all.push_back(m);
            }
            std::sort(all.begin(), all.end(),
                      [](const MonRow &a, const MonRow &b) { return a.dist < b.dist; });
            snap.shown = (int)std::min(all.size(), sizeof(snap.row) / sizeof(snap.row[0]));
            for (int i = 0; i < snap.shown; ++i) {
                MonRow m = all[(size_t)i];
                if (m.dist < 40.f) {
                    auto it = g_names.find(m.ent);
                    if (it == g_names.end()) {
                        NameCache c;
                        uintptr_t cfg = 0;
                        if (mem_read(frame.pid, m.ent + R.monsterConfig, cfg) && mem_ptr_ok(cfg)) {
                            read_str(frame.pid, cfg, nameOff, c.name, sizeof(c.name));
                            read_str(frame.pid, cfg, prefabOff, c.prefab, sizeof(c.prefab));
                        }
                        if (g_names.size() > 128) g_names.clear();
                        it = g_names.emplace(m.ent, c).first;
                    }
                    std::snprintf(m.name, sizeof(m.name), "%s", it->second.name);
                    std::snprintf(m.prefab, sizeof(m.prefab), "%s", it->second.prefab);
                }
                snap.row[i] = m;
            }
            std::snprintf(snap.note, sizeof(snap.note), "%d in m_dicMonsterShow, closest %d",
                          snap.total, snap.shown);
        }
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_snap = snap;
}

void esp_monid_project(const EspCameraProjSnap &snap) {
    if (!g_world.load()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < g_snap.shown; ++i) {
        MonRow &m = g_snap.row[i];
        m.onScreen = false;
        if (!m.havePos || m.dist > 22.f) continue;
        float sx = 0.f, sy = 0.f;
        const bool ok = snap.validMatrixProj
                            ? esp_camera_proj_snap_w2s(snap, m.wx, m.wy, m.wz, sx, sy)
                            : esp_camera_world_to_screen(m.wx, m.wy, m.wz, sx, sy);
        if (!ok) continue;
        m.sx = sx;
        m.sy = sy;
        m.onScreen = true;
    }
}

static const char *row_tag(const MonRow &m) {
    if (esp_monster_id(m.cfgId)) return esp_monster_label(m.cfgId);
    if (esp_monster_id(m.entId)) return esp_monster_label(m.entId);
    return "unmapped";
}

static void copy_rows(const MonSnap &snap) {
    char buf[4096];
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < snap.shown && o + 180 < sizeof(buf); ++i) {
        const MonRow &m = snap.row[i];
        const int n = std::snprintf(
            buf + o, sizeof(buf) - o,
            "%s cfg=%d ent=%d trans=%d transCfg=%d hp=%d/%d dist=%.1f name=\"%s\" prefab=\"%s\"\n",
            row_tag(m), m.cfgId, m.entId, m.transId, m.transCfgId, m.hp, m.hpMax,
            m.dist < 9000.f ? m.dist : -1.f, m.name, m.prefab);
        if (n <= 0) break;
        o += (size_t)n;
    }
    android_copy_to_clipboard(buf);
}

void esp_monid_draw_world(ImDrawList *draw) {
    if (!draw || !g_world.load()) return;
    MonSnap snap;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        snap = g_snap;
    }
    for (int i = 0; i < snap.shown; ++i) {
        const MonRow &m = snap.row[i];
        if (!m.onScreen) continue;
        char a[96], b[96];
        std::snprintf(a, sizeof(a), "%s  cfg %d", row_tag(m), m.cfgId);
        std::snprintf(b, sizeof(b), "ent %d  trans %d", m.entId, m.transId);
        const ImVec2 p(m.sx, m.sy - 36.f);
        const ImU32 fg = IM_COL32(255, 230, 120, 255);
        const ImU32 bg = IM_COL32(0, 0, 0, 220);
        draw->AddText(ImVec2(p.x + 1.f, p.y + 1.f), bg, a);
        draw->AddText(p, fg, a);
        draw->AddText(ImVec2(p.x + 1.f, p.y + 17.f), bg, b);
        draw->AddText(ImVec2(p.x, p.y + 16.f), fg, b);
        if (m.name[0] || m.prefab[0]) {
            char c[96];
            std::snprintf(c, sizeof(c), "%s", m.name[0] ? m.name : m.prefab);
            draw->AddText(ImVec2(p.x + 1.f, p.y + 33.f), bg, c);
            draw->AddText(ImVec2(p.x, p.y + 32.f), fg, c);
        }
    }
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
    bool world = g_world.load();
    if (ImGui::Checkbox("Show ids on mobs", &world)) g_world.store(world);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) copy_rows(snap);
    ImGui::TextDisabled("Ids stay on the mob after you close the menu. Copy grabs this list.");
    if (!snap.ready) return;
    ImGui::BeginChild("monid", ImVec2(0, 0), true);
    for (int i = 0; i < snap.shown; ++i) {
        const MonRow &m = snap.row[i];
        const char *tag = row_tag(m);
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
