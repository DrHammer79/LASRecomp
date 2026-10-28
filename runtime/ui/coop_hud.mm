// Local co-op HUD (runtime/hle/coop.c, own hearts): rows of hearts for the other players - "P2", "P3"... in their
// colours, in the bottom corners or the top right (setting coop_hud) - and a "P1" tag beside the game's own hearts. A
// knocked-out player's hearts are grey and a breathing "Zzz" bubble is over him (at the window's edge when he is off
// screen), where a bar fills while another player revives him - "(A) Revive" shows under a player close enough. Drawn
// over the game (not on the pause menu's pages): the game's own HUD is untouched.
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "imgui.h"

#include "../settings.h"

extern "C" {
typedef struct { int hp, max, down; float revive; uint32_t rgb; } CoopHudRow;
int coop_hud_rows(CoopHudRow *rows, int cap);
bool coop_player_screen(int player, float w, float h, float *sx, float *sy, bool *edge);  // (his head; off screen: the edge)
bool coop_revive_prompt(int player, float w, float h, float *sx, float *sy);  // (below him, beside a knocked-out one)
bool inventory_items_page_shown(void);  // inventory_overlay.mm
bool tracker_map_page_shown(void);      // tracker_overlay.mm
}

// A knocked-out player's "Zzz" in a speech bubble over his head, breathing (a slow swell, the letters rising one after
// another), the revive filling along its bottom. Off screen (edge) it waits at the window's edge in his direction,
// without its tail, in a rim of his colour: where he was left.
static void zzz_bubble(ImDrawList *dl, ImFont *font, float scale, float hx, float hy, bool edge, const CoopHudRow &r) {
    float t = (float)ImGui::GetTime(), breath = 0.5f + 0.5f * sinf(t * 6.2832f / 2.6f);  // (one breath: 2.6 s)
    float k = 0.94f + 0.1f * breath, ts = 30 * scale * k;
    ImVec2 tz = font->CalcTextSizeA(ts, FLT_MAX, 0, "Zzz");
    float bw = tz.x + 22 * scale * k, bh = ts * 0.95f + 10 * scale * k, tail = edge ? 0 : 12 * scale;
    if (edge) hy += bh * 0.5f;  // (centred on the edge point)
    ImVec2 a(hx - bw * 0.5f, hy - tail - bh), z(hx + bw * 0.5f, hy - tail);
    ImU32 ink = IM_COL32(40, 30, 20, 255), paper = IM_COL32(250, 248, 240, (int)(215 + 30 * breath));
    ImU32 rim = edge ? IM_COL32((r.rgb >> 16) & 255, (r.rgb >> 8) & 255, r.rgb & 255, 255) : ink;
    if (!edge)
        dl->AddTriangleFilled(ImVec2(hx - tail * 0.7f, z.y - 1), ImVec2(hx + tail * 0.7f, z.y - 1), ImVec2(hx, hy), ink);
    dl->AddRectFilled(ImVec2(a.x - 3 * scale, a.y - 3 * scale), ImVec2(z.x + 3 * scale, z.y + 3 * scale), ink, 13 * scale);
    dl->AddRectFilled(ImVec2(a.x - 2 * scale, a.y - 2 * scale), ImVec2(z.x + 2 * scale, z.y + 2 * scale), rim, 12 * scale);
    dl->AddRectFilled(a, z, paper, 10 * scale);
    if (!edge)
        dl->AddTriangleFilled(ImVec2(hx - tail * 0.5f, z.y - 1), ImVec2(hx + tail * 0.5f, z.y - 1),
                              ImVec2(hx, hy - 3 * scale), paper);
    float x = hx - tz.x * 0.5f, y = a.y + (bh - tz.y) * 0.5f + ts * 0.08f;
    static const char *const zs[3] = {"Z", "z", "z"};
    for (int i = 0; i < 3; i++) {  // (each letter bobs up in turn)
        float bob = 2.5f * scale * sinf(t * 6.2832f / 2.6f - i * 0.9f);
        dl->AddText(font, ts, ImVec2(x, y - bob), ink, zs[i]);
        x += font->CalcTextSizeA(ts, FLT_MAX, 0, zs[i]).x;
    }
    if (r.revive > 0) {  // (the game's green on a dark track, whatever his colour: a white player's reads too)
        float y = z.y - 7 * scale, x0 = a.x + 8 * scale, x1 = z.x - 8 * scale, bh2 = 5 * scale;
        dl->AddRectFilled(ImVec2(x0 - scale, y - scale), ImVec2(x1 + scale, y + bh2 + scale), ink, 3 * scale);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bh2), IM_COL32(90, 80, 70, 255), 2 * scale);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * fminf(r.revive, 1), y + bh2), IM_COL32(60, 200, 70, 255),
                          2 * scale);
    }
}

// "(A) Revive" under a player who is up beside a knocked-out one, as the game's own "(A) Lift" by a rock: the button
// in a light disc and the word, in a dark rounded box.
static void revive_prompt(ImDrawList *dl, ImFont *font, float scale, float x, float y) {
    float ts = 26 * scale, r = ts * 0.5f, pad = 7 * scale;
    ImVec2 wz = font->CalcTextSizeA(ts, FLT_MAX, 0, "Revive"), az = font->CalcTextSizeA(ts * 0.85f, FLT_MAX, 0, "A");
    float w = pad + 2 * r + 6 * scale + wz.x + pad, h = 2 * r + 1.2f * pad;
    ImVec2 a(x - w * 0.5f, y), z(x + w * 0.5f, y + h), c(x - w * 0.5f + pad + r, y + h * 0.5f);
    dl->AddRectFilled(a, z, IM_COL32(20, 20, 20, 190), h * 0.5f);
    dl->AddCircleFilled(c, r, IM_COL32(245, 240, 225, 255), 20);
    dl->AddText(font, ts * 0.85f, ImVec2(c.x - az.x * 0.5f, c.y - az.y * 0.5f), IM_COL32(40, 30, 20, 255), "A");
    dl->AddText(font, ts, ImVec2(c.x + r + 6 * scale, c.y - wz.y * 0.5f), IM_COL32(250, 250, 250, 255), "Revive");
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

// A player's tag ("P2") at p: in his colour, lifted to at least a bright shade (a dark red or black one would sink
// into the ground behind it), outlined in black all round so it reads on grass, sand and dark floors alike.
static void tag_text(ImDrawList *dl, ImFont *font, float size, float scale, ImVec2 p, uint32_t rgb, bool down,
                     const char *t) {
    float o = 1.6f * scale;
    for (int k = 0; k < 8; k++) {
        static const float d[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        dl->AddText(font, size, ImVec2(p.x + d[k][0] * o, p.y + d[k][1] * o), IM_COL32(0, 0, 0, 230), t);
    }
    int c[3] = {(int)(rgb >> 16 & 255), (int)(rgb >> 8 & 255), (int)(rgb & 255)};
    int top = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    for (int k = 0; k < 3; k++)  // (the brightest channel at 230 at least; a grey's all of them)
        c[k] = top < 230 ? (top < 8 ? 230 : c[k] * 230 / top) : c[k];
    dl->AddText(font, size, p, down ? IM_COL32(150, 150, 150, 255) : IM_COL32(c[0], c[1], c[2], 255), t);
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
    for (int i = a; i < b; i++) {  // (no panel behind them: as the game's own hearts; outlined tags and hearts)
        const CoopHudRow &r = g_rows[i];
        float hx = x0 + pad + lw + 10 * scale;
        char t[4];
        snprintf(t, sizeof t, "P%d", i + 1);
        tag_text(dl, font, tag, scale, ImVec2(x0 + pad, y), r.rgb, r.down, t);
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
        tag_text(dl, font, tag, scale, at, g_rows[0].rgb, false, t);
    }
    for (int i = 1; i < g_nrows; i++) {  // the knocked-out players' bubbles, over their heads (or at the edge)
        float hx, hy;
        bool edge;
        if (g_rows[i].down && coop_player_screen(i, display.x, display.y, &hx, &hy, &edge))
            zzz_bubble(dl, font, scale, hx, hy, edge, g_rows[i]);
    }
    float placed[8][2];  // "(A) Revive" under each player who can revive one (two players on one spot: one under the other)
    int np = 0;
    for (int i = 0; i < g_nrows; i++) {
        float px, py, w = 130 * scale, h = 38 * scale;
        if (!coop_revive_prompt(i, display.x, display.y, &px, &py)) continue;
        for (int k = 0; k < np; k++)
            if (fabsf(px - placed[k][0]) < w && fabsf(py - placed[k][1]) < h) py = placed[k][1] + h, k = -1;
        revive_prompt(dl, font, scale, px, py);
        if (np < 8) placed[np][0] = px, placed[np][1] = py, np++;
    }
    // the other players' rows: in the chosen corner, up to 3 in a bottom one - the rest in the other bottom corner
    int n = g_nrows - 1, corner = g_settings.coop_hud;
    int first = corner == COOP_HUD_TOP_RIGHT ? n : n <= 3 ? n : (n + 1) / 2 > 3 ? (n + 1) / 2 : 3;
    rows_panel(dl, font, scale, display, 1, 1 + first, corner);
    if (first < n)
        rows_panel(dl, font, scale, display, 1 + first, g_nrows,
                   corner == COOP_HUD_BOTTOM_LEFT ? COOP_HUD_BOTTOM_RIGHT : COOP_HUD_BOTTOM_LEFT);
}
