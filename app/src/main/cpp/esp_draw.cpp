#include "esp_draw.hpp"
#include "esp_camera.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

static ImU32 health_color(int hp, int hpMax) {
    if (hpMax <= 0) return IM_COL32(180, 45, 45, 255);
    if (hp < hpMax * 0.3f) return IM_COL32(180, 45, 45, 255);
    if (hp <= hpMax * 0.6f) return IM_COL32(180, 180, 45, 255);
    return IM_COL32(0, 255, 0, 255);
}

static void entity_box(ImDrawList *draw, const ImVec2 &a, const ImVec2 &b, ImU32 col, float r) {
    draw->AddRect(a, b, col, r, 0, 1.6f);
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

static ImVec2 target_screen(const EspFrame &frame, const EspTarget &t, float screenW, float screenH) {
    float sx = 0.f, sy = 0.f;
    if (esp_camera_world_to_screen(t.worldX, t.worldY, t.worldZ, sx, sy))
        return {sx, sy};
    return world_to_screen_pseudo(t.relX, t.relZ, screenW, screenH);
}

static ImVec2 self_screen(const EspFrame &frame, float screenW, float screenH) {
    float sx = 0.f, sy = 0.f;
    if (esp_camera_world_to_screen(frame.selfX, frame.selfY, frame.selfZ, sx, sy))
        return {sx, sy};
    return {screenW * 0.5f, screenH * 0.82f};
}

static void draw_one(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                     const EspTarget &t, const EspFeatures &feat, bool monster) {
    const bool anyBox = monster ? feat.monsterBox : feat.box;
    const bool anyName = monster ? feat.monsterName : feat.name;
    const bool anyHp = monster ? feat.monsterHealth : feat.health;
    if (!anyBox && !anyName && !anyHp && !feat.line && !feat.distance) return;

    const ImVec2 root = target_screen(frame, t, screenW, screenH);
    const float boxH = std::fabs(screenH / (monster ? 6.8f : 6.5f));
    const float boxW = boxH * (monster ? 0.55f : 0.48f);
    const ImVec2 boxStart{root.x - boxW * 0.5f, root.y - boxH};
    const ImVec2 boxEnd{boxStart.x + boxW, boxStart.y + boxH};
    const float centerX = root.x;

    const ImVec2 selfAnchor = self_screen(frame, screenW, screenH);

    if (feat.line && !monster) {
        draw->AddLine(selfAnchor, root, IM_COL32(255, 255, 255, 180), 1.5f);
    }
    if (anyBox) {
        const ImU32 col = monster ? IM_COL32(255, 255, 255, 200) : IM_COL32(255, 255, 255, 230);
        entity_box(draw, boxStart, boxEnd, col, monster ? 6.f : 8.f);
    }
    if (anyHp && t.hpMax > 0) {
        const float pct = std::clamp((float)t.hp / (float)t.hpMax, 0.f, 1.f);
        const ImVec2 barStart{boxStart.x, boxEnd.y + 3.f};
        const ImVec2 barEnd{boxEnd.x, barStart.y + (monster ? 5.f : 4.f)};
        draw->AddRectFilled(barStart, barEnd, IM_COL32(30, 30, 30, 220), 2.5f);
        draw->AddRectFilled(barStart, ImVec2(barStart.x + boxW * pct, barEnd.y),
                            health_color(t.hp, t.hpMax), 2.5f);
        if (!monster) {
            char hpTxt[16];
            std::snprintf(hpTxt, sizeof(hpTxt), "%d", t.hp);
            draw->AddText(ImVec2(barEnd.x + 4.f, barStart.y - 2.f), IM_COL32(255, 255, 255, 220),
                          hpTxt);
        }
    }
    if (anyName && t.label[0]) {
        const ImVec2 ts = ImGui::CalcTextSize(t.label);
        const ImU32 ncol = monster ? IM_COL32(255, 200, 100, 255) : IM_COL32(255, 255, 255, 255);
        draw->AddText(ImVec2(centerX - ts.x * 0.5f, boxStart.y - ts.y - 4.f), ncol, t.label);
    }
    if (feat.distance && !monster) {
        char dist[24];
        std::snprintf(dist, sizeof(dist), "%dm", t.meters);
        const ImVec2 ts = ImGui::CalcTextSize(dist);
        draw->AddText(ImVec2(centerX - ts.x * 0.5f, boxEnd.y + 34.f), IM_COL32(200, 200, 200, 220),
                      dist);
    }
}

} // namespace

void esp_draw_overlay(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                      const EspFeatures &feat) {
    if (!draw || screenW < 1.f || screenH < 1.f) return;
    if (frame.status != EspStatus::Ready) return;

    const bool anyPlayer = feat.box || feat.line || feat.health || feat.name || feat.distance;
    const bool anyMon =
        feat.monsterBox || feat.monsterName || feat.monsterHealth;
    if (!anyPlayer && !anyMon) return;

    const bool haveSelf = std::isfinite(frame.selfX) && std::isfinite(frame.selfZ) &&
                        (std::fabs(frame.selfX) > 0.01f || std::fabs(frame.selfZ) > 0.01f);
    esp_camera_frame_begin(frame.pid, screenW, screenH, frame.selfX, frame.selfY, frame.selfZ,
                           haveSelf);

    if (esp_camera_ready()) {
        draw->AddText(ImVec2(16.f, 56.f), IM_COL32(120, 220, 120, 200),
                      "ESP W2S (memory, no ptrace)");
    } else {
        draw->AddText(ImVec2(16.f, 56.f), IM_COL32(220, 160, 80, 200),
                      "ESP radar fallback — Resolve in match");
    }

    for (const EspTarget &t : frame.targets) {
        if (t.kind == 0 && anyPlayer) draw_one(draw, screenW, screenH, frame, t, feat, false);
        if (t.kind == 1 && anyMon) draw_one(draw, screenW, screenH, frame, t, feat, true);
    }

    esp_camera_frame_end();
}
