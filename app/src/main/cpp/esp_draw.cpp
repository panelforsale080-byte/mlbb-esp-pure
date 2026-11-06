#include "esp_draw.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <vector>

namespace {

/** Readable HUD: slightly larger text/boxes vs baseline #147. */
constexpr float kTextScale = 1.22f;
constexpr float kBoxScalePlayer = 5.95f;   /* larger on-screen box */
constexpr float kBoxScaleMonster = 6.25f;
constexpr int kCdSideMaxLines = 10;

static float overlay_font_size() {
    ImFont *f = ImGui::GetFont();
    if (f && f->FontSize > 1.f) return f->FontSize * kTextScale;
    return 16.f;
}

/** Dark outline behind text for readability on busy backgrounds. */
static void draw_text_shadow(ImDrawList *draw, ImFont *font, float size, const ImVec2 &pos, ImU32 col,
                            const char *txt) {
    if (!txt) return;
    const ImU32 sh = IM_COL32(0, 0, 0, 210);
    const float o = 1.2f;
    if (font) {
        for (int i = -1; i <= 1; i++)
            for (int j = -1; j <= 1; j++) {
                if (i == 0 && j == 0) continue;
                draw->AddText(font, size, ImVec2(pos.x + i * o, pos.y + j * o), sh, txt);
            }
        draw->AddText(font, size, pos, col, txt);
    } else {
        draw->AddText(ImVec2(pos.x + o, pos.y + o), sh, txt);
        draw->AddText(pos, col, txt);
    }
}

static ImU32 health_color(int hp, int hpMax) {
    if (hpMax <= 0) return IM_COL32(180, 45, 45, 255);
    if (hp < hpMax * 0.3f) return IM_COL32(180, 45, 45, 255);
    if (hp <= hpMax * 0.6f) return IM_COL32(180, 180, 45, 255);
    return IM_COL32(0, 255, 0, 255);
}

static void entity_box(ImDrawList *draw, const ImVec2 &a, const ImVec2 &b, ImU32 col, float r) {
    const float thick = 2.8f;
    draw->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f), IM_COL32(0, 0, 0, 140), r, 0, thick + 1.2f);
    draw->AddRect(a, b, col, r, 0, thick);
}

static void draw_one(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                     const EspTarget &t, const EspFeatures &feat, bool monster, float fontSize) {
    ImFont *font = ImGui::GetFont();

    const bool anyBox = monster ? feat.monsterBox : feat.box;
    const bool anyName = monster ? feat.monsterName : feat.name;
    const bool anyHp = monster ? feat.monsterHealth : feat.health;
    const bool anyCd = !monster && feat.enemyCooldown;
    const bool cdUnderEntity = anyCd && !feat.enemyCdSidePanel;
    /* Side-panel-only CD still needs per-entity pass if Box/Name/HP/Line/Dist are on; if all off, skip entity. */
    if (!anyBox && !anyName && !anyHp && !feat.line && !feat.distance && !cdUnderEntity
        && !(anyCd && feat.enemyCdSidePanel && !monster)) {
        return;
    }
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
        draw->AddLine(anchor, root, IM_COL32(255, 220, 80, 200), 4.f);
        draw->AddLine(anchor, root, IM_COL32(255, 255, 255, 255), 2.2f);
    }
    if (!onScreen) return;

    const float boxH = std::fabs(screenH / (monster ? kBoxScaleMonster : kBoxScalePlayer));
    const float boxW = boxH * (monster ? 0.58f : 0.52f);
    const ImVec2 boxStart{root.x - boxW * 0.5f, root.y - boxH};
    const ImVec2 boxEnd{boxStart.x + boxW, boxStart.y + boxH};
    const float centerX = root.x;

    if (anyBox) {
        const ImU32 col = monster ? IM_COL32(255, 230, 120, 245) : IM_COL32(120, 220, 255, 255);
        entity_box(draw, boxStart, boxEnd, col, monster ? 6.f : 9.f);
    }
    float textYBelowBar = boxEnd.y + 8.f;
    if (anyHp && t.hpMax > 0) {
        const float pct = std::clamp((float)t.hp / (float)t.hpMax, 0.f, 1.f);
        const float barH = monster ? 7.f : 6.f;
        const ImVec2 barStart{boxStart.x, boxEnd.y + 4.f};
        const ImVec2 barEnd{boxEnd.x, barStart.y + barH};
        draw->AddRectFilled(barStart, barEnd, IM_COL32(15, 15, 20, 230), 3.f);
        draw->AddRectFilled(barStart, ImVec2(barStart.x + boxW * pct, barEnd.y),
                            health_color(t.hp, t.hpMax), 3.f);
        if (!monster) {
            char hpTxt[16];
            std::snprintf(hpTxt, sizeof(hpTxt), "%d", t.hp);
            draw_text_shadow(draw, font, fontSize * 1.05f, ImVec2(barEnd.x + 6.f, barStart.y - 4.f),
                             IM_COL32(255, 255, 255, 255), hpTxt);
        }
        textYBelowBar = barEnd.y + 8.f;
    }
    if (cdUnderEntity && t.cdValid && !monster) {
        char line[56];
        std::snprintf(line, sizeof(line), "CD %d/%d/%d  SP%d", t.cdSkill1, t.cdSkill2, t.cdSkill3,
                      t.cdSpell);
        ImVec2 ts = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, line)
                         : ImGui::CalcTextSize(line);
        draw_text_shadow(draw, font, fontSize, ImVec2(centerX - ts.x * 0.5f, textYBelowBar),
                         IM_COL32(160, 240, 255, 255), line);
        textYBelowBar += ts.y + 3.f;
    }
    if (anyName && t.label[0]) {
        ImVec2 ts =
                font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, t.label) : ImGui::CalcTextSize(t.label);
        const ImU32 ncol = monster ? IM_COL32(255, 215, 90, 255) : IM_COL32(255, 255, 255, 255);
        draw_text_shadow(draw, font, fontSize * 1.05f, ImVec2(centerX - ts.x * 0.5f, boxStart.y - ts.y - 6.f), ncol,
                         t.label);
    }
    if (feat.distance && !monster) {
        char dist[32];
        std::snprintf(dist, sizeof(dist), "%dm", t.meters);
        const float dsz = fontSize * 1.1f;
        ImVec2 ts = font ? font->CalcTextSizeA(dsz, FLT_MAX, 0.f, dist) : ImGui::CalcTextSize(dist);
        draw_text_shadow(draw, font, dsz, ImVec2(centerX - ts.x * 0.5f, textYBelowBar), IM_COL32(255, 255, 140, 255),
                         dist);
    }
}

static bool enemy_cd_cmp(const EspTarget *a, const EspTarget *b) {
    if (a->meters != b->meters) return a->meters < b->meters;
    int c = std::strcmp(a->label, b->label);
    if (c != 0) return c < 0;
    return a->ent < b->ent;
}

static void draw_enemy_cd_side_panels(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                                      const EspFeatures &feat, float fontSize) {
    if (!feat.enemyCooldown || !feat.enemyCdSidePanel) return;

    ImFont *font = ImGui::GetFont();
    std::vector<const EspTarget *> rows;
    for (const EspTarget &t : frame.targets) {
        if (t.kind != 0) continue;
        rows.push_back(&t);
    }
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(), enemy_cd_cmp);

    const float margin = 10.f;
    const float panelPad = 10.f;
    const float titleH = fontSize + 6.f;
    const float lineStep = fontSize + 5.f;

    char sample[192];
    float maxW = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, "Enemy cooldown").x : 120.f;
    const size_t nShow = std::min(rows.size(), (size_t)kCdSideMaxLines);
    for (size_t i = 0; i < nShow; ++i) {
        const EspTarget &t = *rows[i];
        const char *nm = t.label[0] ? t.label : "?";
        if (t.cdValid)
            std::snprintf(sample, sizeof(sample), "%s  %dm  %d/%d/%d SP%d", nm, t.meters, t.cdSkill1, t.cdSkill2,
                          t.cdSkill3, t.cdSpell);
        else
            std::snprintf(sample, sizeof(sample), "%s  %dm  CD …", nm, t.meters);
        ImVec2 tw = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, sample) : ImGui::CalcTextSize(sample);
        maxW = std::max(maxW, tw.x);
    }
    float panelInnerW = maxW + 8.f;
    float panelW = panelInnerW + panelPad * 2.f;
    float panelH = titleH + lineStep * (float)nShow + panelPad * 2.f;

    /* Left side — avoids ML right-side skill/minimap/FAB overlap */
    const ImVec2 r0(margin, screenH * 0.16f);
    const ImVec2 r1(margin + panelW, r0.y + panelH);

    draw->AddRectFilled(r0, r1, IM_COL32(10, 14, 22, 130), 8.f);
    draw->AddRect(r0, r1, IM_COL32(180, 220, 255, 120), 8.f, 0, 1.5f);

    const char *title = "Enemy cooldown";
    ImVec2 titleSz = font ? font->CalcTextSizeA(fontSize * 1.06f, FLT_MAX, 0.f, title)
                          : ImGui::CalcTextSize(title);
    draw_text_shadow(draw, font, fontSize * 1.08f, ImVec2(r0.x + (panelW - titleSz.x) * 0.5f, r0.y + panelPad),
                     IM_COL32(230, 245, 255, 255), title);

    float y = r0.y + panelPad + titleH;
    for (size_t i = 0; i < nShow; ++i) {
        const EspTarget &t = *rows[i];
        const char *nm = t.label[0] ? t.label : "?";
        if (t.cdValid)
            std::snprintf(sample, sizeof(sample), "%s  %dm  %d/%d/%d SP%d", nm, t.meters, t.cdSkill1, t.cdSkill2,
                          t.cdSkill3, t.cdSpell);
        else
            std::snprintf(sample, sizeof(sample), "%s  %dm  CD …", nm, t.meters);
        draw_text_shadow(draw, font, fontSize * 1.02f, ImVec2(r0.x + panelPad, y), IM_COL32(210, 235, 255, 255),
                         sample);
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
