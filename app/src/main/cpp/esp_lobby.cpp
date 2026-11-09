#include "esp_lobby.hpp"
#include "esp_runtime_off.hpp"
#include "il2_layout.hpp"
#include "mem.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

static std::atomic<bool> g_on{false};

struct LobbyPlayer {
    int pos = 0;
    char name[40] = {};
    char team[32] = {};
    char a[120] = {};
    char b[140] = {};
    char c[140] = {};
};

struct LobbySnap {
    int count = 0;
    char header[96] = {};
    LobbyPlayer p[10]{};
};

static std::mutex g_mu;
static LobbySnap g_snap;

void esp_lobby_set_enabled(bool on) { g_on.store(on); }

static bool read_str(pid_t pid, uintptr_t ent, uintptr_t off, char *out, size_t n) {
    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!ent || !off) return false;
    uintptr_t str = 0;
    if (!mem_read(pid, ent + off, str) || !mem_ptr_ok(str)) return false;
    alignas(4) uint8_t blob[4 + 48 * 2];
    if (!mem_read(pid, str + 0x10, blob, sizeof(blob))) return false;
    int32_t len = 0;
    std::memcpy(&len, blob, sizeof(len));
    if (len <= 0 || len > 48) return false;
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 4 < n; ++i) {
        uint16_t cu = 0;
        std::memcpy(&cu, blob + 4 + (size_t)i * 2, 2);
        if (cu < 0x80) out[o++] = (char)cu;
        else if (cu < 0x800) {
            out[o++] = (char)(0xC0 | (cu >> 6));
            out[o++] = (char)(0x80 | (cu & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (cu >> 12));
            out[o++] = (char)(0x80 | ((cu >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cu & 0x3F));
        }
    }
    out[o < n ? o : n - 1] = '\0';
    return out[0] != '\0';
}

static uint32_t read_u32(pid_t pid, uintptr_t ent, uintptr_t off) {
    uint32_t v = 0;
    if (ent && off) mem_read(pid, ent + off, v);
    return v;
}

static uint8_t read_u8(pid_t pid, uintptr_t ent, uintptr_t off) {
    uint8_t v = 0;
    if (ent && off) mem_read(pid, ent + off, v);
    return v;
}

static const char *spell_name(int id) {
    switch (id) {
    case 20150: return "Execute";
    case 20020: return "Retri";
    case 20030: return "Inspire";
    case 20040: return "Sprint";
    case 20050: return "Revitalize";
    case 20060: return "Aegis";
    case 20070: return "Petrify";
    case 20080: return "Purify";
    case 20140: return "Flameshot";
    case 20100: return "Flicker";
    case 20160: return "Arrival";
    case 20190: return "Vengeance";
    default: return nullptr;
    }
}

static const char *lane_name(uint32_t road) {
    switch (road) {
    case 1: return "Exp";
    case 2: return "Mid";
    case 3: return "Roam";
    case 4: return "Jungle";
    case 5: return "Gold";
    default: return "Lane ?";
    }
}

static int read_wants(pid_t pid, uintptr_t ent, uintptr_t off, uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!ent || !off) return 0;
    uintptr_t list = 0;
    if (!mem_read(pid, ent + off, list) || !mem_ptr_ok(list)) return 0;
    uintptr_t arr = 0;
    int size = 0;
    if (!mem_read(pid, list + Il2::ListItems, arr) || !mem_ptr_ok(arr)) return 0;
    if (!mem_read(pid, list + Il2::ListSize, size) || size <= 0) return 0;
    if (size > 3) size = 3;
    if (!mem_read(pid, arr + Il2::ArrData, out, (size_t)size * sizeof(uint32_t))) return 0;
    return size;
}

static bool dict_values(pid_t pid, uintptr_t dict, std::vector<uintptr_t> &out) {
    out.clear();
    if (!dict) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!mem_read(pid, dict + Il2::DictEntries, entries) || !mem_ptr_ok(entries)) return false;
    if (!mem_read(pid, dict + Il2::DictCount, count) || count <= 0 || count > 20) return false;
    int maxLength = 0;
    if (!mem_read(pid, entries + Il2::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 64)
        return false;
    const uintptr_t data = entries + Il2::ArrData;
    for (int i = 0; i < maxLength && (int)out.size() < count; ++i) {
        int hash = 0;
        uintptr_t value = 0;
        if (!mem_read(pid, data + (uintptr_t)i * Il2::EntryStride + Il2::EntryHash, hash)) break;
        if (hash < 0) continue;
        if (!mem_read(pid, data + (uintptr_t)i * Il2::EntryStride + Il2::EntryValue, value)) break;
        if (mem_ptr_ok(value)) out.push_back(value);
    }
    return !out.empty();
}

void esp_lobby_tick(pid_t pid) {
    LobbySnap next{};
    if (!g_on.load() || pid <= 0) {
        std::snprintf(next.header, sizeof(next.header), "Lobby off");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = next;
        return;
    }
    const EspRuntimeOff &R = esp_runtime_off();
    if (!R.systemDataKlass || !R.roomDict || !R.rdName) {
        std::snprintf(next.header, sizeof(next.header), "Lobby: SystemData not resolved");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = next;
        return;
    }
    uintptr_t dict = 0;
    for (uintptr_t soff : Il2::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!mem_read(pid, R.systemDataKlass + soff, st) || !mem_ptr_ok(st)) continue;
        uintptr_t d = 0;
        if (!mem_read(pid, st + R.roomDict, d) || !mem_ptr_ok(d)) continue;
        std::vector<uintptr_t> probe;
        if (dict_values(pid, d, probe)) {
            dict = d;
            break;
        }
    }
    std::vector<uintptr_t> players;
    if (!dict || !dict_values(pid, dict, players)) {
        std::snprintf(next.header, sizeof(next.header), "Lobby: not in a room");
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = next;
        return;
    }
    uint64_t room = 0;
    if (R.rdRoom) mem_read(pid, players[0] + R.rdRoom, room);
    std::snprintf(next.header, sizeof(next.header), "Lobby  %d  room %llu", (int)players.size(),
                  (unsigned long long)room);
    for (uintptr_t ent : players) {
        if (next.count >= 10) break;
        LobbyPlayer &p = next.p[next.count];
        p.pos = (int)read_u32(pid, ent, R.rdPos);
        if (!read_str(pid, ent, R.rdName, p.name, sizeof(p.name)))
            std::snprintf(p.name, sizeof(p.name), "player");
        read_str(pid, ent, R.rdTeam, p.team, sizeof(p.team));
        const uint64_t uid = [&] {
            uint64_t v = 0;
            if (R.rdUid) mem_read(pid, ent + R.rdUid, v);
            return v;
        }();
        const uint32_t zone = read_u32(pid, ent, R.rdZone);
        const uint32_t lv = read_u32(pid, ent, R.rdLevel);
        const uint32_t rank = read_u32(pid, ent, R.rdRank);
        const uint32_t sub = read_u32(pid, ent, R.rdSubRank);
        const uint32_t myth = read_u32(pid, ent, R.rdMyth);
        const uint32_t hist = read_u32(pid, ent, R.rdHistRank);
        const uint32_t country = read_u32(pid, ent, R.rdCountry);
        std::snprintf(p.a, sizeof(p.a), "id %llu  zone %u  Lv %u  Rank %u  Sub %u  Myth %u  Hist %u  Nat %u",
                      (unsigned long long)uid, zone, lv, rank, sub, myth, hist, country);
        const int spell = (int)read_u32(pid, ent, R.rdSpell);
        char spellBuf[24];
        const char *sn = spell_name(spell);
        if (sn) std::snprintf(spellBuf, sizeof(spellBuf), "%s", sn);
        else std::snprintf(spellBuf, sizeof(spellBuf), "Spell %d", spell);
        uint32_t wants[3] = {};
        const int nw = read_wants(pid, ent, R.rdWant, wants);
        std::snprintf(p.b, sizeof(p.b),
                      "Hero %u  Skin %u  %s  Rune %u/%u  Ban %u  %s  Ping %u  Uses %u  Want %u/%u/%u",
                      read_u32(pid, ent, R.rdHero), read_u32(pid, ent, R.rdSkin), spellBuf,
                      read_u32(pid, ent, R.rdRune), read_u32(pid, ent, R.rdRuneLv),
                      read_u32(pid, ent, R.rdBan), lane_name(read_u32(pid, ent, R.rdRoad)),
                      read_u32(pid, ent, R.rdPing), read_u32(pid, ent, R.rdHeroUse),
                      nw > 0 ? wants[0] : 0, nw > 1 ? wants[1] : 0, nw > 2 ? wants[2] : 0);
        char flags[64] = {};
        size_t fo = 0;
        auto add = [&](const char *s) {
            if (fo && fo + 1 < sizeof(flags)) flags[fo++] = ' ';
            for (size_t i = 0; s[i] && fo + 1 < sizeof(flags); ++i) flags[fo++] = s[i];
            flags[fo] = '\0';
        };
        if (read_u8(pid, ent, R.rdRobot)) add("BOT");
        if (read_u8(pid, ent, R.rdVp)) add("VP");
        if (read_u8(pid, ent, R.rdFake)) add("Fake");
        if (read_u8(pid, ent, R.rdLeader)) add("Leader");
        if (read_u8(pid, ent, R.rdStar)) add("Star");
        std::snprintf(p.c, sizeof(p.c), "Camp %u  Pos %u  %s%s%s", read_u32(pid, ent, R.rdCamp),
                      read_u32(pid, ent, R.rdPos), p.team[0] ? p.team : "no team",
                      flags[0] ? "  " : "", flags);
        ++next.count;
    }
    for (int i = 1; i < next.count; ++i) {
        LobbyPlayer key = next.p[i];
        int j = i - 1;
        while (j >= 0 && next.p[j].pos > key.pos) {
            next.p[j + 1] = next.p[j];
            --j;
        }
        next.p[j + 1] = key;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_snap = next;
}

void esp_lobby_draw(ImDrawList *draw, float screenW, float screenH) {
    if (!draw || !g_on.load()) return;
    LobbySnap snap;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        snap = g_snap;
    }
    const float x = 16.f;
    float y = 72.f;
    const float fs = 15.f;
    ImFont *font = ImGui::GetFont();
    auto line = [&](const char *text, ImU32 col) {
        if (!text || !text[0]) return;
        ImVec2 ts = font ? font->CalcTextSizeA(fs, FLT_MAX, 0.f, text) : ImGui::CalcTextSize(text);
        const float w = ts.x + 16.f;
        const float h = ts.y + 6.f;
        if (x + w > screenW - 8.f) {
            /* keep the card on screen; long lines still start at the left */
        }
        draw->AddRectFilled(ImVec2(x - 4.f, y - 2.f), ImVec2(x + w, y + h), IM_COL32(20, 20, 24, 190),
                            4.f);
        if (font) draw->AddText(font, fs, ImVec2(x, y), col, text);
        else draw->AddText(ImVec2(x, y), col, text);
        y += h + 2.f;
        if (y > screenH - 24.f) y = 72.f;
    };
    line(snap.header[0] ? snap.header : "Lobby", IM_COL32(255, 220, 120, 255));
    for (int i = 0; i < snap.count; ++i) {
        char title[96];
        std::snprintf(title, sizeof(title), "%s", snap.p[i].name);
        line(title, IM_COL32(255, 255, 255, 255));
        line(snap.p[i].a, IM_COL32(190, 230, 255, 255));
        line(snap.p[i].b, IM_COL32(190, 255, 190, 255));
        line(snap.p[i].c, IM_COL32(255, 210, 160, 255));
    }
}
