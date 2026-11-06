#include "esp_draw.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <vector>

namespace {

/** Slightly larger HUD text and boxes vs baseline #147. */
constexpr float kTextScale = 1.14f;
constexpr float kBoxScalePlayer = 6.15f;  /* was 6.5f — bigger box */
constexpr float kBoxScaleMonster = 6.45f;  /* was 6.8f */
constexpr int kCdSideMaxLines = 10;

static float overlay_font_size() {
    ImFont *f = ImGui::GetFont();
    if (f && f->FontSize > 1.f) return f->FontSize * kTextScale;
    return 15.f;
}

static ImU32 health_color(int hp, int hpMax) {
    if (hpMax <= 0) return IM_COL32(180, 45, 45, 255);
    if (hp < hpMax * 0.3f) return IM_COL32(180, 45, 45, 255);
    if (hp <= hpMax * 0.6f) return IM_COL32(180, 180, 45, 255);
    return IM_COL32(0, 255, 0, 255);
}

static void entity_box(ImDrawList *draw, const ImVec2 &a, const ImVec2 &b, ImU32 col, float r) {
    draw->AddRect(a, b, col, r, 0, 2.f);
}

static void draw_one(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                     const EspTarget &t, const EspFeatures &feat, bool monster, float fontSize) {
    ImFont *font = ImGui::GetFont();

    const bool anyBox = monster ? feat.monsterBox : feat.box;
    const bool anyName = monster ? feat.monsterName : feat.name;
    const bool anyHp = monster ? feat.monsterHealth : feat.health;
    const bool anyCd = !monster && feat.enemyCooldown;
    const bool cdUnderEntity = anyCd && !feat.enemyCdSidePanel;
    if (!anyBox && !anyName && !anyHp && !feat.line && !feat.distance && !cdUnderEntity) return;
    if (!t.screenValid) return;

    const ImVec2 root{t.screenX, t.screenY};
    const bool onScreen =
        root.x >= -64.f && root.y >= -64.f && root.x <= screenW + 64.f && root.y <= screenH + 64.f;

    if (feat.line && !monster) {
        ImVec2 anchor;
        if (frame.selfScreenValid)
            anchor = {frame.selfScreenX, frame.selfScreenY};
        else
            anchor = {screenW * 0.5f, screenH * 0.92f};
        draw->AddLine(anchor, root, IM_COL32(255, 255, 255, 180), 2.f);
    }
    if (!onScreen) return;

    const float boxH = std::fabs(screenH / (monster ? kBoxScaleMonster : kBoxScalePlayer));
    const float boxW = boxH * (monster ? 0.58f : 0.52f);
    const ImVec2 boxStart{root.x - boxW * 0.5f, root.y - boxH};
    const ImVec2 boxEnd{boxStart.x + boxW, boxStart.y + boxH};
    const float centerX = root.x;

    if (anyBox) {
        const ImU32 col = monster ? IM_COL32(255, 255, 255, 210) : IM_COL32(255, 255, 255, 235);
        entity_box(draw, boxStart, boxEnd, col, monster ? 6.f : 9.f);
    }
    float textYBelowBar = boxEnd.y + 8.f;
    if (anyHp && t.hpMax > 0) {
        const float pct = std::clamp((float)t.hp / (float)t.hpMax, 0.f, 1.f);
        const float barH = monster ? 6.f : 5.f;
        const ImVec2 barStart{boxStart.x, boxEnd.y + 4.f};
        const ImVec2 barEnd{boxEnd.x, barStart.y + barH};
        draw->AddRectFilled(barStart, barEnd, IM_COL32(30, 30, 30, 200), 3.f);
        draw->AddRectFilled(barStart, ImVec2(barStart.x + boxW * pct, barEnd.y),
                            health_color(t.hp, t.hpMax), 3.f);
        if (!monster) {
            char hpTxt[16];
            std::snprintf(hpTxt, sizeof(hpTxt), "%d", t.hp);
            if (font)
                draw->AddText(font, fontSize, ImVec2(barEnd.x + 6.f, barStart.y - 3.f), IM_COL32(255, 255, 255, 235),
                              hpTxt);
            else
                draw->AddText(ImVec2(barEnd.x + 6.f, barStart.y - 3.f), IM_COL32(255, 255, 255, 235), hpTxt);
        }
        textYBelowBar = barEnd.y + 8.f;
    }
    if (cdUnderEntity && t.cdValid && !monster) {
        char line[56];
        std::snprintf(line, sizeof(line), "CD %d/%d/%d  SP%d", t.cdSkill1, t.cdSkill2, t.cdSkill3,
                      t.cdSpell);
        ImVec2 ts = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, line)
                         : ImGui::CalcTextSize(line);
        if (font)
            draw->AddText(font, fontSize, ImVec2(centerX - ts.x * 0.5f, textYBelowBar), IM_COL32(190, 225, 255, 245),
                          line);
        else
            draw->AddText(ImVec2(centerX - ts.x * 0.5f, textYBelowBar), IM_COL32(190, 225, 255, 245), line);
        textYBelowBar += ts.y + 3.f;
    }
    if (anyName && t.label[0]) {
        ImVec2 ts =
                font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, t.label) : ImGui::CalcTextSize(t.label);
        const ImU32 ncol = monster ? IM_COL32(255, 200, 100, 255) : IM_COL32(255, 255, 255, 255);
        if (font)
            draw->AddText(font, fontSize, ImVec2(centerX - ts.x * 0.5f, boxStart.y - ts.y - 5.f), ncol, t.label);
        else
            draw->AddText(ImVec2(centerX - ts.x * 0.5f, boxStart.y - ts.y - 5.f), ncol, t.label);
    }
    if (feat.distance && !monster) {
        char dist[24];
        std::snprintf(dist, sizeof(dist), "%dm", t.meters);
        ImVec2 ts = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, dist) : ImGui::CalcTextSize(dist);
        if (font)
            draw->AddText(font, fontSize, ImVec2(centerX - ts.x * 0.5f, textYBelowBar), IM_COL32(210, 210, 210, 235),
                          dist);
        else
            draw->AddText(ImVec2(centerX - ts.x * 0.5f, textYBelowBar), IM_COL32(210, 210, 210, 235), dist);
    }
}

static bool enemy_cd_cmp(const EspTarget *a, const EspTarget *b) {
    if (a->meters != b->meters) return a->meters < b->meters;
    return std::strcmp(a->label, b->label) < 0;
}

static void draw_enemy_cd_side_panels(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                                      const EspFeatures &feat, float fontSize) {
    if (!feat.enemyCooldown || !feat.enemyCdSidePanel) return;

    ImFont *font = ImGui::GetFont();
    std::vector<const EspTarget *> rows;
    for (const EspTarget &t : frame.targets) {
        if (t.kind != 0 || !t.cdValid) continue;
        rows.push_back(&t);
    }
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(), enemy_cd_cmp);

    const float margin = 10.f;
    const float panelPad = 10.f;
    const float titleH = fontSize + 6.f;
    const float lineStep = fontSize + 5.f;

    char sample[192];
    float maxW = 0.f;
    const size_t nShow = std::min(rows.size(), (size_t)kCdSideMaxLines);
    for (size_t i = 0; i < nShow; ++i) {
        const EspTarget &t = *rows[i];
        const char *nm = t.label[0] ? t.label : "?";
        std::snprintf(sample, sizeof(sample), "%s  %dm  %d/%d/%d SP%d", nm, t.meters, t.cdSkill1, t.cdSkill2,
                      t.cdSkill3, t.cdSpell);
        ImVec2 tw = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, sample) : ImGui::CalcTextSize(sample);
        maxW = std::max(maxW, tw.x);
    }
    float panelInnerW = maxW + 8.f;
    float panelW = panelInnerW + panelPad * 2.f;
    float panelH = titleH + lineStep * (float)nShow + panelPad * 2.f;

    /* Right side (does not cover minimap area too much — narrow strip) */
    const ImVec2 r0(screenW - margin - panelW, screenH * 0.20f);
    const ImVec2 r1(screenW - margin, r0.y + panelH);

    draw->AddRectFilled(r0, r1, IM_COL32(12, 18, 28, 115), 8.f);
    draw->AddRect(r0, r1, IM_COL32(255, 255, 255, 95), 8.f, 0, 1.25f);

    const char *title = "Enemy cooldown";
    ImVec2 titleSz = font ? font->CalcTextSizeA(fontSize * 1.06f, FLT_MAX, 0.f, title)
                          : ImGui::CalcTextSize(title);
    if (font)
        draw->AddText(font, fontSize * 1.06f,
                      ImVec2(r0.x + (panelW - titleSz.x) * 0.5f, r0.y + panelPad),
                      IM_COL32(220, 240, 255, 250), title);
    else
        draw->AddText(ImVec2(r0.x + (panelW - titleSz.x) * 0.5f, r0.y + panelPad),
                      IM_COL32(220, 240, 255, 250), title);

    float y = r0.y + panelPad + titleH;
    for (size_t i = 0; i < nShow; ++i) {
        const EspTarget &t = *rows[i];
        const char *nm = t.label[0] ? t.label : "?";
        std::snprintf(sample, sizeof(sample), "%s  %dm  %d/%d/%d SP%d", nm, t.meters, t.cdSkill1, t.cdSkill2,
                      t.cdSkill3, t.cdSpell);
        if (font)
            draw->AddText(font, fontSize, ImVec2(r0.x + panelPad, y), IM_COL32(200, 230, 255, 245), sample);
        else
            draw->AddText(ImVec2(r0.x + panelPad, y), IM_COL32(200, 230, 255, 245), sample);
        y += lineStep;
    }
}

} // namespace

void esp_draw_overlay(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                      const EspFeatures &feat) {
    if (!draw || screenW < 1.f || screenH < 1.f) return;
    if (frame.status != EspStatus::Ready) return;

    const bool anyPlayer =
        feat.box || feat.line || feat.health || feat.name || feat.distance || feat.enemyCooldown;
    const bool anyMon =
        feat.monsterBox || feat.monsterName || feat.monsterHealth;
    if (!anyPlayer && !anyMon) return;

    const float fontSize = overlay_font_size();

    for (const EspTarget &t : frame.targets) {
        if (t.kind == 0 && anyPlayer) draw_one(draw, screenW, screenH, frame, t, feat, false, fontSize);
        if (t.kind == 1 && anyMon) draw_one(draw, screenW, screenH, frame, t, feat, true, fontSize);
    }

    if (anyPlayer) draw_enemy_cd_side_panels(draw, screenW, screenH, frame, feat, fontSize);
}
