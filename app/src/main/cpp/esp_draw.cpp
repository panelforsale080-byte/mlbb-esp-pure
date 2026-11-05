#include "esp_draw.hpp"
#include "esp_camera.hpp"
#include "esp_engine.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct SmoothSlot {
    int id = -1;
    float x = 0.f;
    float y = 0.f;
};

static SmoothSlot g_smooth[32];

static ImU32 health_color(int hp, int hpMax) {
    if (hpMax <= 0) return IM_COL32(220, 70, 90, 255);
    const float r = (float)hp / (float)hpMax;
    if (r < 0.3f) return IM_COL32(255, 70, 90, 255);
    if (r <= 0.6f) return IM_COL32(255, 210, 80, 255);
    return IM_COL32(80, 240, 140, 255);
}

static void corner_box(ImDrawList *draw, const ImVec2 &a, const ImVec2 &b, ImU32 col, float t) {
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    const float len = std::min(w, h) * 0.22f;
    auto seg = [&](float x1, float y1, float x2, float y2) {
        draw->AddLine({x1, y1}, {x2, y2}, col, t);
    };
    seg(a.x, a.y, a.x + len, a.y);
    seg(a.x, a.y, a.x, a.y + len);
    seg(b.x, a.y, b.x - len, a.y);
    seg(b.x, a.y, b.x, a.y + len);
    seg(a.x, b.y, a.x + len, b.y);
    seg(a.x, b.y, a.x, b.y - len);
    seg(b.x, b.y, b.x - len, b.y);
    seg(b.x, b.y, b.x, b.y - len);
}

static void text_outlined(ImDrawList *draw, const ImVec2 &p, ImU32 col, const char *txt) {
    const ImU32 shadow = IM_COL32(0, 0, 0, 180);
    draw->AddText({p.x + 1.f, p.y + 1.f}, shadow, txt);
    draw->AddText(p, col, txt);
}

static ImVec2 world_to_screen_pseudo(float relX, float relZ, float screenW, float screenH) {
    constexpr float kRange = 45.f;
    const float cx = screenW * 0.5f;
    const float cy = screenH * 0.48f;
    const float sx = screenW * 0.40f / kRange;
    const float sy = screenH * 0.28f / kRange;
    const float dist = std::sqrt(relX * relX + relZ * relZ);
    const float clamp = dist > kRange ? (kRange / dist) : 1.f;
    const float x = std::clamp(cx + relX * clamp * sx, 16.f, screenW - 16.f);
    const float y = std::clamp(cy - relZ * clamp * sy, 90.f, screenH - 28.f);
    return {x, y};
}

static ImVec2 smooth_screen(int entityId, ImVec2 raw) {
    if (entityId <= 0) return raw;
    const float dt = ImGui::GetIO().DeltaTime;
    const float k = 1.f - std::exp(-dt * 38.f);
    SmoothSlot *slot = nullptr;
    for (SmoothSlot &s : g_smooth) {
        if (s.id == entityId) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        for (SmoothSlot &s : g_smooth) {
            if (s.id < 0) {
                slot = &s;
                break;
            }
        }
    }
    if (!slot) slot = &g_smooth[entityId & 31];
    if (slot->id != entityId) {
        slot->id = entityId;
        slot->x = raw.x;
        slot->y = raw.y;
        return raw;
    }
    slot->x += (raw.x - slot->x) * k;
    slot->y += (raw.y - slot->y) * k;
    return {slot->x, slot->y};
}

static bool live_world(const EspFrame &frame, const EspTarget &t, float &wx, float &wy, float &wz) {
    wx = t.worldX;
    wy = t.worldY;
    wz = t.worldZ;
    if (t.entityPtr && frame.pid > 0)
        esp_read_entity_world(frame.pid, t.entityPtr, wx, wy, wz);
    return true;
}

static ImVec2 target_screen(const EspFrame &frame, const EspTarget &t, float screenW, float screenH) {
    float wx = 0.f, wy = 0.f, wz = 0.f;
    live_world(frame, t, wx, wy, wz);
    float sx = 0.f, sy = 0.f;
    if (esp_camera_world_to_screen(wx, wy, wz, sx, sy))
        return smooth_screen(t.entityId, {sx, sy});
    return smooth_screen(t.entityId,
                         world_to_screen_pseudo(t.relX, t.relZ, screenW, screenH));
}

static ImVec2 self_screen(const EspFrame &frame, float screenW, float screenH) {
    float sx = 0.f, sy = 0.f, wx = frame.selfX, wy = frame.selfY, wz = frame.selfZ;
    if (frame.bmInstance && frame.pid > 0)
        esp_read_self_world(frame.pid, frame.bmInstance, wx, wy, wz);
    if (esp_camera_world_to_screen(wx, wy, wz, sx, sy)) return {sx, sy};
    return {screenW * 0.5f, screenH * 0.82f};
}

static void draw_one(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                     const EspTarget &t, const EspFeatures &feat, bool monster) {
    const bool anyBox = monster ? feat.monsterBox : feat.box;
    const bool anyName = monster ? feat.monsterName : feat.name;
    const bool anyHp = monster ? feat.monsterHealth : feat.health;
    if (!anyBox && !anyName && !anyHp && !feat.line && !feat.distance) return;

    const ImVec2 root = target_screen(frame, t, screenW, screenH);
    const float boxH = std::fabs(screenH / (monster ? 7.2f : 6.8f));
    const float boxW = boxH * (monster ? 0.52f : 0.46f);
    const ImVec2 boxStart{root.x - boxW * 0.5f, root.y - boxH};
    const ImVec2 boxEnd{boxStart.x + boxW, boxStart.y + boxH};
    const float centerX = root.x;

    const ImU32 accent =
        monster ? IM_COL32(255, 190, 110, 235) : IM_COL32(110, 220, 255, 245);
    const ImU32 accentDim =
        monster ? IM_COL32(255, 190, 110, 100) : IM_COL32(110, 220, 255, 110);

    const ImVec2 selfAnchor = self_screen(frame, screenW, screenH);

    if (feat.line && !monster) {
        draw->AddLine(selfAnchor, root, accentDim, 1.2f);
        draw->AddLine(selfAnchor, root, IM_COL32(255, 255, 255, 90), 0.6f);
    }
    if (anyBox) {
        corner_box(draw, boxStart, boxEnd, accent, 1.8f);
    }
    if (anyHp && t.hpMax > 0) {
        const float pct = std::clamp((float)t.hp / (float)t.hpMax, 0.f, 1.f);
        const ImVec2 barStart{boxStart.x, boxEnd.y + 2.f};
        const ImVec2 barEnd{boxEnd.x, barStart.y + (monster ? 4.f : 3.f)};
        draw->AddRectFilled(barStart, barEnd, IM_COL32(12, 14, 18, 200), 2.f);
        draw->AddRectFilled(barStart, ImVec2(barStart.x + boxW * pct, barEnd.y),
                            health_color(t.hp, t.hpMax), 2.f);
        if (!monster) {
            char hpTxt[16];
            std::snprintf(hpTxt, sizeof(hpTxt), "%d", t.hp);
            text_outlined(draw, ImVec2(barEnd.x + 3.f, barStart.y - 3.f),
                          IM_COL32(240, 240, 240, 230), hpTxt);
        }
    }
    if (anyName && t.label[0]) {
        const ImVec2 ts = ImGui::CalcTextSize(t.label);
        text_outlined(draw, ImVec2(centerX - ts.x * 0.5f, boxStart.y - ts.y - 3.f), accent,
                      t.label);
    }
    if (feat.distance && !monster) {
        float wx = t.worldX, wy = t.worldY, wz = t.worldZ;
        live_world(frame, t, wx, wy, wz);
        float sx = frame.selfX, sy = frame.selfY, sz = frame.selfZ;
        if (frame.bmInstance && frame.pid > 0)
            esp_read_self_world(frame.pid, frame.bmInstance, sx, sy, sz);
        const float dx = wx - sx;
        const float dz = wz - sz;
        const int meters = (int)std::sqrt(dx * dx + dz * dz);
        char dist[24];
        std::snprintf(dist, sizeof(dist), "%dm", meters);
        const ImVec2 ts = ImGui::CalcTextSize(dist);
        text_outlined(draw, ImVec2(centerX - ts.x * 0.5f, boxEnd.y + 10.f),
                      IM_COL32(200, 210, 220, 220), dist);
    }
}

} // namespace

void esp_draw_overlay(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                      const EspFeatures &feat) {
    if (!draw || screenW < 1.f || screenH < 1.f) return;
    if (frame.status != EspStatus::Ready) return;

    const bool anyPlayer = feat.box || feat.line || feat.health || feat.name || feat.distance;
    const bool anyMon = feat.monsterBox || feat.monsterName || feat.monsterHealth;
    if (!anyPlayer && !anyMon) return;

    esp_camera_frame_begin(frame.pid, screenW, screenH, false);

    {
        char hud[72];
        std::snprintf(hud, sizeof(hud), "ESP ON  %zu enemies  (in classic/training match)",
                      frame.targets.size());
        text_outlined(draw, ImVec2(14.f, 14.f), IM_COL32(120, 240, 160, 240), hud);
        if (frame.targets.empty()) {
            text_outlined(draw, ImVec2(14.f, 34.f), IM_COL32(255, 200, 100, 230),
                          "No targets — wait for heroes / tap Resolve in match");
        }
    }

    for (const EspTarget &t : frame.targets) {
        if (t.kind == 0 && anyPlayer) draw_one(draw, screenW, screenH, frame, t, feat, false);
        if (t.kind == 1 && anyMon) draw_one(draw, screenW, screenH, frame, t, feat, true);
    }

    esp_camera_frame_end();
}
