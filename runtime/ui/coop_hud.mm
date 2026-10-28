// Local co-op HUD (runtime/hle/coop.c, own hearts): rows of hearts for the other players - "P2", "P3"... in their
// colours, in the bottom corners or the top right (setting coop_hud) - and a "P1" tag beside the game's own hearts. A
// knocked-out player's hearts are grey and a breathing "Zzz" bubble is over him, where a bar fills while another
// player revives him. Drawn over the game (not on the pause menu's pages): the game's own HUD is untouched.
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "imgui.h"

#include "../settings.h"

extern "C" {
typedef struct { int hp, max, down; float revive; uint32_t rgb; } CoopHudRow;
int coop_hud_rows(CoopHudRow *rows, int cap);
bool coop_player_screen(int player, float w, float h, float *sx, float *sy);  // (his head on screen)
bool inventory_items_page_shown(void);  // inventory_overlay.mm
bool tracker_map_page_shown(void);      // tracker_overlay.mm
}

// A knocked-out player's "Zzz" in a speech bubble over his head, breathing (a slow swell, the letters rising one after
// another), the revive filling along its bottom.
static void zzz_bubble(ImDrawList *dl, ImFont *font, float scale, float hx, float hy, const CoopHudRow &r) {
    float t = (float)ImGui::GetTime(), breath = 0.5f + 0.5f * sinf(t * 6.2832f / 2.6f);  // (one breath: 2.6 s)
    float k = 0.94f + 0.1f * breath, ts = 30 * scale * k;
    ImVec2 tz = font->CalcTextSizeA(ts, FLT_MAX, 0, "Zzz");
    float bw = tz.x + 22 * scale * k, bh = ts * 0.95f + 10 * scale * k, tail = 12 * scale;
    ImVec2 a(hx - bw * 0.5f, hy - tail - bh), z(hx + bw * 0.5f, hy - tail);
    ImU32 ink = IM_COL32(40, 30, 20, 255), paper = IM_COL32(250, 248, 240, (int)(215 + 30 * breath));
    dl->AddTriangleFilled(ImVec2(hx - tail * 0.7f, z.y - 1), ImVec2(hx + tail * 0.7f, z.y - 1), ImVec2(hx, hy), ink);
    dl->AddRectFilled(ImVec2(a.x - 2 * scale, a.y - 2 * scale), ImVec2(z.x + 2 * scale, z.y + 2 * scale), ink, 12 * scale);
    dl->AddRectFilled(a, z, paper, 10 * scale);
    dl->AddTriangleFilled(ImVec2(hx - tail * 0.5f, z.y - 1), ImVec2(hx + tail * 0.5f, z.y - 1),
                          ImVec2(hx, hy - 3 * scale), paper);
    float x = hx - tz.x * 0.5f, y = a.y + (bh - tz.y) * 0.5f + ts * 0.08f;
    static const char *const zs[3] = {"Z", "z", "z"};
    for (int i = 0; i < 3; i++) {  // (each letter bobs up in turn)
        float bob = 2.5f * scale * sinf(t * 6.2832f / 2.6f - i * 0.9f);
        dl->AddText(font, ts, ImVec2(x, y - bob), ink, zs[i]);
        x += font->CalcTextSizeA(ts, FLT_MAX, 0, zs[i]).x;
    }
    if (r.revive > 0) {
        float y = z.y - 6 * scale, x0 = a.x + 8 * scale, x1 = z.x - 8 * scale;
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 4 * scale), IM_COL32(0, 0, 0, 60), 2 * scale);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * fminf(r.revive, 1), y + 4 * scale),
                          IM_COL32((r.rgb >> 16) & 255, (r.rgb >> 8) & 255, r.rgb & 255, 255), 2 * scale);
    }
}

static CoopHudRow g_rows[8];
static int g_nrows;

extern "C" bool coop_hud_wanted(void) {
    bool paused = inventory_items_page_shown() || tracker_map_page_shown();
    g_nrows = g_settings.coop_health == COOP_HEALTH_OWN && !paused ? coop_hud_rows(g_rows, 8) : 0;
    return g_nrows > 1;
}

static ImU32 rgb_col(uint32_t rgb, float k, int a) {
    int r = (int)fminf(255, (rgb >> 16 & 255) * k), g = (int)fminf(255, (rgb >> 8 & 255) * k), b = (int)fminf(255, (rgb & 255) * k);
    return IM_COL32(r, g, b, a);
}

// One heart (s wide) at p: two lobes and a point; `fill` (0..1, quarters) of it from the left in `full`.
static void heart(ImDrawList *dl, ImVec2 p, float s, float fill, ImU32 full, ImU32 empty, ImU32 edge) {
    float r = s * 0.27f;
    ImVec2 l(p.x + r, p.y + r), rr(p.x + s - r, p.y + r);
    ImVec2 t0(p.x + s * 0.02f, p.y + r * 1.25f), t1(p.x + s * 0.98f, p.y + r * 1.25f), t2(p.x + s * 0.5f, p.y + s * 0.92f);
    auto shape = [&](ImU32 c, float grow) {
        dl->AddCircleFilled(l, r + grow, c, 16);
        dl->AddCircleFilled(rr, r + grow, c, 16);
        dl->AddTriangleFilled(ImVec2(t0.x - grow, t0.y), ImVec2(t1.x + grow, t1.y), ImVec2(t2.x, t2.y + grow * 1.6f), c);
    };
    shape(edge, s * 0.07f);
    shape(empty, 0);
    if (fill > 0) {
        dl->PushClipRect(p, ImVec2(p.x + s * fill, p.y + s), true);
        shape(full, 0);
        dl->PopClipRect();
    }
}

// The game's HUD box: 16:9, centred (runtime/ui/race_overlay.mm); its layouts are 1280x720 in it.
static void hud_box(ImVec2 display, float *bx, float *by, float *bw, float *bh) {
    *bw = fminf(display.x, display.y * 16 / 9), *bh = *bw * 9 / 16;
    *bx = (display.x - *bw) * 0.5f, *by = (display.y - *bh) * 0.5f;
}

// Players [a, b)'s heart rows in a panel in a corner. A knocked-out player's hearts are grey (his "Zzz" bubble is over
// him). Top right: below the game's X/Y items, its rupee count and its compass / seashell sensor (to 0.32 of the HUD
// box's height). Bottom right: above its "Now saving." note (about 0.79-1.0 of the box's width, 0.86-0.94 of its
// height) and the run timer's panel when it's in that corner (runtime/ui/race_overlay.mm), as the timer does.
static void rows_panel(ImDrawList *dl, ImFont *font, float scale, ImVec2 display, int a, int b, int corner) {
    float tag = 30 * scale, s = 30 * scale, gap = 4 * scale, pad = 10 * scale, rowh = s + 14 * scale;
    int nh = 3, lines = 0;  // (hearts per line: the most any of these players can have, 10 a line as the game's)
    for (int i = a; i < b; i++) {
        int hearts = g_rows[i].max / 4;
        nh = hearts > nh ? hearts : nh;
        lines += hearts > 10 ? (hearts + 9) / 10 : 1;
    }
    if (nh > 10) nh = 10;
    float lw = font->CalcTextSizeA(tag, FLT_MAX, 0, "P8").x;
    float pw = pad * 2 + lw + 10 * scale + nh * (s + gap) - gap, ph = pad * 2 + lines * rowh - 14 * scale;
    bool top = corner == COOP_HUD_TOP_RIGHT, right = corner != COOP_HUD_BOTTOM_LEFT;
    float bx, by, bw, bh;
    hud_box(display, &bx, &by, &bw, &bh);
    float x0 = right ? display.x - 30 * scale - pw : 30 * scale;
    float y0 = top ? by + 0.34f * bh : display.y - 30 * scale - ph;
    if (!top && g_settings.timer && right == (g_settings.timer_pos == TIMER_POS_RIGHT)) y0 -= 100 * scale;
    if (!top && right) {
        float sx0 = bx + 0.79f * bw, sy0 = by + 0.86f * bh;
        if (x0 + pw > sx0 && y0 + ph > sy0) y0 = sy0 - 10 * scale - ph;
    }
    float y = y0 + pad;
    for (int i = a; i < b; i++) {  // (no panel behind them: as the game's own hearts; shadowed tags, outlined hearts)
        const CoopHudRow &r = g_rows[i];
        float hx = x0 + pad + lw + 10 * scale;
        char t[4];
        snprintf(t, sizeof t, "P%d", i + 1);
        dl->AddText(font, tag, ImVec2(x0 + pad + 2 * scale, y + 2 * scale), IM_COL32(0, 0, 0, 170), t);
        dl->AddText(font, tag, ImVec2(x0 + pad, y), r.down ? IM_COL32(150, 150, 150, 255) : rgb_col(r.rgb, 1.15f, 255), t);
        bool colour = g_settings.coop_heart_colours;
        ImU32 full = r.down ? IM_COL32(110, 110, 110, 255) : colour ? rgb_col(r.rgb, 1, 255) : IM_COL32(235, 40, 40, 255);
        ImU32 empty = r.down ? IM_COL32(55, 55, 55, 220) : colour ? rgb_col(r.rgb, 0.3f, 230) : IM_COL32(80, 14, 14, 230);
        for (int h = 0; h < r.max / 4; h++) {
            float f = (r.hp - h * 4) / 4.0f;
            heart(dl, ImVec2(hx + (h % 10) * (s + gap), y + (h / 10) * rowh + 2 * scale), s, f < 0 ? 0 : f > 1 ? 1 : f,
                  full, empty, IM_COL32(20, 12, 8, 230));
        }
        y += (r.max / 4 > 10 ? (r.max / 4 + 9) / 10 : 1) * rowh;
    }
}

extern "C" void coop_hud_draw(ImVec2 display, ImFont *font, float scale) {
    if (g_nrows < 2) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float bx, by, bw, bh, tag = 30 * scale;  // (the game's hearts start at its HUD box's top left)
    hud_box(display, &bx, &by, &bw, &bh);
    {   // "P1" beside the game's own hearts
        char t[4] = "P1";
        ImVec2 sz = font->CalcTextSizeA(tag, FLT_MAX, 0, t);
        ImVec2 at(bx + 0.022f * bw - sz.x - 6 * scale, by + 0.026f * bh);
        if (at.x < 4 * scale) at.x = 4 * scale;
        dl->AddText(font, tag, ImVec2(at.x + 2 * scale, at.y + 2 * scale), IM_COL32(0, 0, 0, 170), t);
        dl->AddText(font, tag, at, rgb_col(g_rows[0].rgb, 1.15f, 255), t);
    }
    for (int i = 1; i < g_nrows; i++) {  // the knocked-out players' bubbles, over their heads
        float hx, hy;
        if (g_rows[i].down && coop_player_screen(i, display.x, display.y, &hx, &hy))
            zzz_bubble(dl, font, scale, hx, hy, g_rows[i]);
    }
    // the other players' rows: in the chosen corner, up to 3 in a bottom one - the rest in the other bottom corner
    int n = g_nrows - 1, corner = g_settings.coop_hud;
    int first = corner == COOP_HUD_TOP_RIGHT ? n : n <= 3 ? n : (n + 1) / 2 > 3 ? (n + 1) / 2 : 3;
    rows_panel(dl, font, scale, display, 1, 1 + first, corner);
    if (first < n)
        rows_panel(dl, font, scale, display, 1 + first, g_nrows,
                   corner == COOP_HUD_BOTTOM_LEFT ? COOP_HUD_BOTTOM_RIGHT : COOP_HUD_BOTTOM_LEFT);
}
