#include "esp_draw.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <unordered_map>
#include <vector>

namespace {

struct SmoothTarget {
    float lastX = 0.f;
    float lastY = 0.f;
    bool init = false;
};
static std::unordered_map<uintptr_t, SmoothTarget> g_smoothCache;

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
    const ImU32 sh = IM_COL32(0, 0, 0, 200);
    const float o = 1.f;
    if (font) {
        draw->AddText(font, size, ImVec2(pos.x + o, pos.y), sh, txt);
        draw->AddText(font, size, ImVec2(pos.x - o, pos.y), sh, txt);
        draw->AddText(font, size, ImVec2(pos.x, pos.y + o), sh, txt);
        draw->AddText(font, size, ImVec2(pos.x, pos.y - o), sh, txt);
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
        && !(anyCd && feat.enemyCdSidePanel && !monster)
        && !(feat.facing && !monster)
        && !(feat.playerInfo && !monster)) {
        return;
    }
    if (!t.screenValid) return;

    float targetX = t.screenX;
    float targetY = t.screenY;
    auto &smooth = g_smoothCache[t.ent];
    if (!smooth.init) {
        smooth.lastX = targetX;
        smooth.lastY = targetY;
        smooth.init = true;
    } else {
        constexpr float kSmoothAlpha = 0.45f;
        smooth.lastX += (targetX - smooth.lastX) * kSmoothAlpha;
        smooth.lastY += (targetY - smooth.lastY) * kSmoothAlpha;
    }
    const ImVec2 root{smooth.lastX, smooth.lastY};
    const bool onScreen =
        root.x >= -64.f && root.y >= -64.f && root.x <= screenW + 64.f && root.y <= screenH + 64.f;

    if (feat.line && !monster) {
        ImVec2 anchor;
        if (frame.selfScreenValid)
            anchor = {frame.selfScreenX, frame.selfScreenY};
        else
            anchor = {screenW * 0.5f, screenH * 0.92f};
        draw->AddLine(anchor, root, IM_COL32(255, 255, 255, 180), 1.5f);
    }
    if (!onScreen) return;
    if (feat.facing && !monster && t.faceScreenValid) {
        const ImVec2 tip{t.faceScreenX, t.faceScreenY};
        const ImVec2 from{root.x, root.y - 18.f};
        draw->AddLine(from, tip, IM_COL32(0, 0, 0, 180), 3.4f);
        draw->AddLine(from, tip, IM_COL32(255, 214, 64, 235), 2.f);
        draw->AddCircleFilled(tip, 3.2f, IM_COL32(255, 214, 64, 235), 8);
    }

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
        textYBelowBar += ts.y + 3.f;
    }
    if (feat.playerInfo && !monster && t.infoValid) {
        const char *lines[] = {t.info0, t.info1, t.info2, t.info3};
        const float isz = fontSize * 0.92f;
        for (const char *line : lines) {
            if (!line || !line[0]) continue;
            ImVec2 ts = font ? font->CalcTextSizeA(isz, FLT_MAX, 0.f, line) : ImGui::CalcTextSize(line);
            draw_text_shadow(draw, font, isz, ImVec2(centerX - ts.x * 0.5f, textYBelowBar),
                             IM_COL32(190, 255, 190, 255), line);
            textYBelowBar += ts.y + 2.f;
        }
    }
}

static bool enemy_cd_cmp(const EspTarget *a, const EspTarget *b) {
    if (a->meters != b->meters) return a->meters < b->meters;
    int c = std::strcmp(a->label, b->label);
    if (c != 0) return c < 0;
    return a->ent < b->ent;
}

/** High-contrast red alert when an enemy hero is within 12 m. */
static void draw_proximity_alert_12m(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                                     float fontSize) {
    ImFont *font = ImGui::GetFont();
    std::vector<const EspTarget *> near;
    near.reserve(16);
    for (const EspTarget &t : frame.targets) {
        if (t.kind != 0) continue;
        if (t.meters > 12) continue;
        if (frame.localPlayerEnt && t.ent == frame.localPlayerEnt) continue;
        near.push_back(&t);
    }
    if (near.empty()) return;

    std::sort(near.begin(), near.end(), [](const EspTarget *a, const EspTarget *b) {
        if (a->meters != b->meters) return a->meters < b->meters;
        return a->ent < b->ent;
    });

    constexpr int kMaxLines = 8;
    const size_t nShow = std::min(near.size(), (size_t)kMaxLines);
    const size_t nExtra = near.size() > nShow ? (near.size() - nShow) : 0;

    char lineBuf[220];
    float maxLineW = 0.f;
    const char *title = "ENEMY — WITHIN 12 METERS";
    const float titleScale = 1.28f;
    ImVec2 titleSz = font ? font->CalcTextSizeA(fontSize * titleScale, FLT_MAX, 0.f, title)
                          : ImGui::CalcTextSize(title);
    maxLineW = titleSz.x;

    for (size_t i = 0; i < nShow; ++i) {
        const EspTarget &t = *near[i];
        const char *nm = (t.label[0] ? t.label : "?");
        std::snprintf(lineBuf, sizeof(lineBuf), "%s   %dm", nm, t.meters);
        ImVec2 tw = font ? font->CalcTextSizeA(fontSize * 1.08f, FLT_MAX, 0.f, lineBuf) : ImGui::CalcTextSize(lineBuf);
        maxLineW = std::max(maxLineW, tw.x);
    }
    char extraBuf[48] = {};
    if (nExtra > 0) {
        std::snprintf(extraBuf, sizeof(extraBuf), "+%zu more enemy nearby", nExtra);
        ImVec2 tw = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, extraBuf) : ImGui::CalcTextSize(extraBuf);
        maxLineW = std::max(maxLineW, tw.x);
    }

    const float padX = 22.f;
    const float padY = 14.f;
    const float lineH = fontSize + 7.f;
    const float titleBlockH = titleSz.y + 10.f;
    float boxH = padY * 2.f + titleBlockH + lineH * (float)nShow + (nExtra ? lineH : 0.f);
    const float boxW = std::min(maxLineW + padX * 2.f, screenW - 24.f);

    const float cx = screenW * 0.5f;
    const float top = screenH * 0.04f;
    const ImVec2 r0(cx - boxW * 0.5f, top);
    const ImVec2 r1(cx + boxW * 0.5f, top + boxH);

    /* Outer dark outline + bright red panel */
    draw->AddRectFilled(ImVec2(r0.x - 4.f, r0.y - 4.f), ImVec2(r1.x + 4.f, r1.y + 4.f), IM_COL32(0, 0, 0, 230), 14.f);
    draw->AddRectFilled(r0, r1, IM_COL32(215, 25, 25, 250), 12.f);
    draw->AddRect(r0, r1, IM_COL32(255, 220, 220, 255), 12.f, 0, 3.5f);
    draw->AddRect(ImVec2(r0.x + 2.f, r0.y + 2.f), ImVec2(r1.x - 2.f, r1.y - 2.f), IM_COL32(120, 0, 0, 200), 10.f, 0, 1.5f);

    float y = r0.y + padY;
    draw_text_shadow(draw, font, fontSize * titleScale, ImVec2(cx - titleSz.x * 0.5f, y), IM_COL32(255, 255, 255, 255),
                     title);
    y += titleBlockH;
    for (size_t i = 0; i < nShow; ++i) {
        const EspTarget &t = *near[i];
        const char *nm = (t.label[0] ? t.label : "?");
        std::snprintf(lineBuf, sizeof(lineBuf), "%s   %dm", nm, t.meters);
        ImVec2 tw =
                font ? font->CalcTextSizeA(fontSize * 1.08f, FLT_MAX, 0.f, lineBuf) : ImGui::CalcTextSize(lineBuf);
        draw_text_shadow(draw, font, fontSize * 1.08f, ImVec2(cx - tw.x * 0.5f, y), IM_COL32(255, 255, 255, 255),
                         lineBuf);
        y += lineH;
    }
    if (nExtra > 0) {
        ImVec2 tw = font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, extraBuf) : ImGui::CalcTextSize(extraBuf);
        draw_text_shadow(draw, font, fontSize, ImVec2(cx - tw.x * 0.5f, y), IM_COL32(255, 230, 230, 240), extraBuf);
    }
}

static void draw_enemy_cd_side_panels(ImDrawList *draw, float screenW, float screenH, const EspFrame &frame,
                                      const EspFeatures &feat, float fontSize, bool menuOpen) {
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

    const int fillA = menuOpen ? 130 : 215;
    const int borderA = menuOpen ? 120 : 200;
    draw->AddRectFilled(r0, r1, IM_COL32(10, 14, 22, fillA), 8.f);
    draw->AddRect(r0, r1, IM_COL32(180, 220, 255, borderA), 8.f, 0, 1.5f);

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
                      const EspFeatures &feat, bool menuOpen) {
    if (!draw || screenW < 1.f || screenH < 1.f) return;
    if (frame.status != EspStatus::Ready) return;

    const bool anyPlayer = feat.box || feat.line || feat.health || feat.name || feat.distance || feat.enemyCooldown
                           || feat.proximityAlert12m || feat.facing || feat.playerInfo;
    const bool anyMon =
        feat.monsterBox || feat.monsterName || feat.monsterHealth;
    if (!anyPlayer && !anyMon) return;

    const float fontSize = overlay_font_size();

    for (const EspTarget &t : frame.targets) {
        if (t.kind == 0 && anyPlayer) draw_one(draw, screenW, screenH, frame, t, feat, false, fontSize);
        if (t.kind == 1 && anyMon) draw_one(draw, screenW, screenH, frame, t, feat, true, fontSize);
    }

    if (feat.proximityAlert12m) draw_proximity_alert_12m(draw, screenW, screenH, frame, fontSize);
    if (anyPlayer) draw_enemy_cd_side_panels(draw, screenW, screenH, frame, feat, fontSize, menuOpen);

    if (frame.targets.empty()) g_smoothCache.clear();
}
