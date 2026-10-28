// The settings' Controls tab (in game: menu.mm; start screen: launcher.mm, both through menu_tabs_draw), in option rows
// (runtime/ui/option_rows.h), and its Configure inputs page for the bindings (runtime/input_bind.h): DEVICE and the
// categories on the left, the chosen category's bindings on the right. A on a category moves the cursor to its
// bindings; A on a binding waits for the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over); B goes back a step (bindings -> categories -> the tab). Open menu holds the controller's shortcuts for
// this menu (one button or two together; X / Delete clears one, never the last) and the keyboard's F1 / Esc, which
// can't be changed. Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible)
#include "i18n.h"
#include "option_rows.h"

extern "C" {
#include "../platform.h"
#include "../input_bind.h"
#include "../settings.h"

extern "C" int coop_players_setting(void);  // (runtime/hle/coop.c: 1 once the game is a version without co-op)
extern "C" int coop_colours_state(void);    // (runtime/hle/coop.c: 0 the tunic colours are in, 1 after a restart, 2 no file)
extern "C" const char *coop_colours_path(void);  // (... where that file goes)
extern "C" bool coop_supported(void);       // (... false once the game is a version without co-op)
extern "C" const char *input_player_pad_name(int player);  // (runtime/hle/input.c: a player's controller, or NULL)
int input_pad_family(void);              // runtime/hle/input.c
SDL_JoystickID input_active_pad(void);   // runtime/hle/input.c: the controller the game reads (0: none yet)
#if PLAT_TOUCH
bool touch_edit_available(void);  // mobile/touch_controls.mm (the on-screen controls)
void touch_edit_begin(void);
void touch_layout_reset(void);
int game_in_play(void);  // hle/warp.c
void menu_toggle(void);  // menu.mm
#endif
}

// ------------------------------------------------------------------ Configure inputs: what's on it
enum { CAT_BUTTONS, CAT_TRIGGERS, CAT_STICKS, CAT_DPAD, CAT_MENU, N_CAT };
static const char *const k_cat_name[N_CAT] = {LN("Buttons"), LN("Triggers"), LN("Sticks"), LN("D-pad"), LN("Open menu")};
static const char *const k_cat_hook[N_CAT] = {"buttons", "triggers", "sticks", "dpad", "menu"};  // (LARECOMP_CONTROLS_SUB)

// section: a header before this row; fixed: a key that can't be changed (slot -1)
struct Bind { int slot; const char *label; const char *section; const char *fixed; };
static int cat_rows(int cat, bool pad, Bind *out) {
    int n = 0;
    auto add = [&](int slot, const char *label, const char *section = NULL) { out[n++] = {slot, label, section, NULL}; };
    switch (cat) {
    case CAT_BUTTONS:
        add(BIND_A, "A"), add(BIND_B, "B"), add(BIND_X, "X"), add(BIND_Y, "Y"), add(BIND_PLUS, LN("Plus")),
            add(BIND_MINUS, LN("Minus"));
        break;
    case CAT_TRIGGERS: add(BIND_L, "L"), add(BIND_R, "R"), add(BIND_ZL, "ZL"), add(BIND_ZR, "ZR"); break;
    case CAT_STICKS:  // (a controller's sticks stay its sticks: only their presses are buttons)
        add(BIND_LSTICK, LN("Press"), LN("Left stick"));
        if (!pad)
            add(BIND_LS_UP, LN("Up")), add(BIND_LS_DOWN, LN("Down")), add(BIND_LS_LEFT, LN("Left")),
                add(BIND_LS_RIGHT, LN("Right"));
        add(BIND_RSTICK, LN("Press"), LN("Right stick"));
        if (!pad)
            add(BIND_RS_UP, LN("Up")), add(BIND_RS_DOWN, LN("Down")), add(BIND_RS_LEFT, LN("Left")),
                add(BIND_RS_RIGHT, LN("Right"));
        break;
    case CAT_DPAD:
        add(BIND_UP, LN("Up")), add(BIND_DOWN, LN("Down")), add(BIND_LEFT, LN("Left")), add(BIND_RIGHT, LN("Right"));
        break;
    case CAT_MENU:  // (F1 and Esc stay: whatever the controller's shortcuts are, the menu can always be reached)
        if (pad) add(BIND_MENU_1, LN("Shortcut 1")), add(BIND_MENU_2, LN("Shortcut 2"));
        else out[n++] = {-1, LN("Shortcut 1"), NULL, "F1"}, out[n++] = {-1, LN("Shortcut 2"), NULL, "Esc"};
        break;
    }
    return n;
}
static int slot_cat(int slot) {
    if (slot >= BIND_MENU_1) return CAT_MENU;
    Bind b[16];
    for (int c = 0; c < N_CAT; c++)
        for (int i = 0, n = cat_rows(c, false, b); i < n; i++)
            if (b[i].slot == slot) return c;
    return CAT_BUTTONS;
}

// Devices: each connected controller by name (they share one set of bindings; the name picks the button names), or
// "Controller" when none is connected (bindings for one connected later), then the keyboard.
struct Device { SDL_JoystickID id; char name[64]; int fam; bool pad; };
static int pad_fam(SDL_JoystickID id) {
    if (!id || getenv("LARECOMP_PAD_TYPE")) return bind_label_family();  // (none / a test pretending one)
    return input_family_of_type(SDL_GetGamepadTypeForID(id));
}
static int list_devices(Device *d, int cap) {
    int n = 0, np = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&np);
    for (int i = 0; i < np && n < cap - 1; i++) {
        const char *nm = SDL_GetGamepadNameForID(ids[i]);
        d[n].id = ids[i], d[n].pad = true, d[n].fam = pad_fam(ids[i]);
        snprintf(d[n].name, sizeof d[n].name, "%s", nm && *nm ? nm : LN("Controller"));
        if (strlen(d[n].name) > 30) strcpy(d[n].name + 27, "...");  // (fits beside DEVICE)
        n++;
    }
    SDL_free(ids);
    if (!n) d[n].id = 0, d[n].pad = true, d[n].fam = pad_fam(0), snprintf(d[n].name, sizeof d[n].name, LN("Controller")), n++;
    d[n].id = 0, d[n].pad = false, d[n].fam = 0, snprintf(d[n].name, sizeof d[n].name, LN("Keyboard")), n++;
    return n;
}

// ------------------------------------------------------------------ state
static bool g_sub;                 // on the Configure inputs page
static int g_cat = CAT_BUTTONS;    // the category shown on the right
static bool g_dev_pad = true;      // the device chosen: a controller (g_dev_id) or the keyboard
static SDL_JoystickID g_dev_id;
static bool g_in_right;            // the cursor is on a binding (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the page starts over)
static int g_back_frame = -10;     // the frame B / Esc went back a step here (the menu mustn't close on it)
enum { FOCUS_NONE, FOCUS_CONFIGURE, FOCUS_DEVICE, FOCUS_CAT, FOCUS_RIGHT };
static int g_focus;                // put the cursor on this row when it's drawn

// B / Esc this frame went back a step on the Configure inputs page (menu.mm / launcher.mm: don't close too)
extern "C" bool controls_ui_took_back(void) { return g_back_frame == ImGui::GetFrameCount(); }

static void take_focus(int want) {  // (right after the row: it's the last item)
    if (g_focus != want) return;
    g_focus = FOCUS_NONE;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::SetScrollHereY(0.3f);
}

// The device the page opens on: the keyboard when it was opened with one (Enter / Space), else the controller the game
// reads (or the first connected; "Controller" when there's none)
static void pick_device(bool keyboard) {
    SDL_JoystickID act = input_active_pad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!act && n > 0) act = ids[0];
    SDL_free(ids);
    g_dev_pad = !keyboard, g_dev_id = act;
}
static bool keyboard_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
}

// a menu shortcut's value: "Home", "Minus + Plus", "(none)" (in the menus' language)
static const char *menu_label(int i, int fam, char *buf, size_t cap) {
    int a = g_bind_menu[i][0], b = g_bind_menu[i][1];
    if (a < 0) snprintf(buf, cap, "%s", L(LN("(none)")));
    else if (b < 0) snprintf(buf, cap, "%s", L(bind_pad_label(a, fam)));
    else snprintf(buf, cap, "%s + %s", L(bind_pad_label(a, fam)), L(bind_pad_label(b, fam)));
    return buf;
}

// A finished capture keeps larecomp.ini current; a swap is said under the list (another row changed too)
static void watch_results(int fam, char *msg, size_t cap) {
    static int seen = -1;
    int rdev, rslot, rother, serial, kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen < 0) seen = serial;
    if (serial == seen) return;
    seen = serial;
    msg[0] = 0;
    char lab[64];
    if (kind == 2 && rother >= BIND_MENU_1)
        snprintf(msg, cap, L("Swapped. Shortcut %d is %s now."), rother - BIND_MENU_1 + 1,
                 menu_label(rother - BIND_MENU_1, fam, lab, sizeof lab));
    else if (kind == 2)
        snprintf(msg, cap, L("Swapped. %s is on %s now."), L(bind_input_name(rother)),
                 L(rdev == BIND_DEV_PAD ? bind_pad_label(bind_pad_host(rother), fam) : bind_key_label(g_bind_key[rother])));
    if (kind == 6) snprintf(msg, cap, "%s", L("The game uses that button. Hold it with a second button."));
    if (kind == 7) snprintf(msg, cap, "%s", L("Keep at least one shortcut, so a controller can open this menu."));
    if (kind == 1 || kind == 2 || kind == 5 || kind == 8) settings_save();
}

static bool nav_on_item(void) { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

static void configure_inputs(float scale) {
    Device devs[9];
    int nd = list_devices(devs, 9), di = -1;
    for (int i = 0; i < nd; i++)
        if (devs[i].pad == g_dev_pad && (!g_dev_pad || devs[i].id == g_dev_id)) di = i;
    if (di < 0) di = g_dev_pad ? 0 : nd - 1;  // (that controller is gone: the first one)
    g_dev_pad = devs[di].pad, g_dev_id = devs[di].id;
    const int dev = g_dev_pad ? BIND_DEV_PAD : BIND_DEV_KEY, fam = devs[di].fam;

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    static char msg[160];
    watch_results(fam, msg, sizeof msg);

    // B / Esc: from the bindings back to their category, from the categories back to the tab (a capture takes Esc and
    // every controller button itself: runtime/hle/bindings.c)
    if (cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        g_back_frame = ImGui::GetFrameCount();
        if (g_in_right) g_focus = FOCUS_CAT;
        else { g_sub = false, g_focus = FOCUS_CONFIGURE, msg[0] = 0; return; }  // (the tab is drawn instead)
    }
    // Left on a binding: back to its category too
    if (g_in_right && cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)))
        g_focus = FOCUS_CAT;

    opt_begin("##configure", scale, true);
    opt_section("Configure inputs");
    const char *names[9];
    for (int i = 0; i < nd; i++) names[i] = devs[i].name;
    int ndi = di;
    if (opt_choice("Device", &ndi, names, nd, di, NULL)) {
        bind_capture_cancel();
        g_dev_pad = devs[ndi].pad, g_dev_id = devs[ndi].id, msg[0] = 0;
    }
    take_focus(FOCUS_DEVICE);
    for (int c = 0; c < N_CAT; c++) {
        if (g_in_right && c == g_cat) opt_mark_next();
        bool open = opt_link(k_cat_name[c], NULL);
        if (c == g_cat) take_focus(FOCUS_CAT);
        if (nav_on_item() && g_cat != c) g_cat = c, msg[0] = 0;  // (moving down the list shows each)
        if (open) {
            if (g_cat != c) msg[0] = 0;
            g_cat = c;
            if (!(ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))) g_focus = FOCUS_RIGHT;  // (a tap / click on it: no cursor)
            bind_capture_cancel();
        }
    }
    if (bind_is_default(dev)) opt_disable_next();
    if (opt_action("Restore default controls", "", g_dev_pad ? "Puts every controller button back to its default."
                                                            : "Puts every key back to its default.")) {
        bind_capture_cancel();
        bind_reset(dev);
    }
    // (the cursor or the mouse on a row with a description: it shows on the right instead of the bindings)
    bool desc_right = !PLAT_TOUCH && (ImGui::IsItemHovered() || nav_on_item()) && g_focus != FOCUS_RIGHT;  // (phones: in its card)
    opt_split();
    if (desc_right) {
        opt_desc_text();
        g_in_right = false;
    } else {
        Bind rows[16];
        int n = cat_rows(g_cat, g_dev_pad, rows);
        opt_section(k_cat_name[g_cat]);
        bool in_right = false;
        for (int i = 0; i < n; i++) {
            const Bind &b = rows[i];
            if (b.section) opt_section(b.section);
            if (b.fixed) {
                opt_disable_next();
                ImGui::PushID(i);
                opt_action(b.label, b.fixed, NULL);
                ImGui::PopID();
                continue;
            }
            bool waiting = cd == dev && cs == b.slot, menu = b.slot >= BIND_MENU_1;
            char val[64];
            if (waiting) snprintf(val, sizeof val, g_dev_pad ? L("Press a button...  %d") : L("Press a key...  %d"), (ms + 999) / 1000);
            else if (menu) menu_label(b.slot - BIND_MENU_1, fam, val, sizeof val);
            else snprintf(val, sizeof val, "%s", g_dev_pad ? bind_pad_label(bind_pad_host(b.slot), fam) : bind_key_label(g_bind_key[b.slot]));
            // the same button / key on two inputs (a hand-edited larecomp.ini), or a game button that opens the menu: red
            bool dup = false;
            if (menu) {
                int a = g_bind_menu[b.slot - BIND_MENU_1][0];
                for (int j = 0; j < BIND_PAD_COUNT; j++) dup |= g_bind_menu[b.slot - BIND_MENU_1][1] < 0 && bind_pad_host(j) == a && a >= 0;
            } else {
                const int16_t *t = g_dev_pad ? g_bind_pad : g_bind_key;
                for (int j = 0, m = g_dev_pad ? BIND_PAD_COUNT : BIND_KEY_COUNT; j < m; j++)
                    dup |= j != b.slot && t[j] == t[b.slot] && t[b.slot] > (g_dev_pad ? -1 : 0);
                dup |= g_dev_pad && bind_menu_single(bind_pad_host(b.slot));
            }
            if (dup && !waiting) opt_warn_next();
            ImGui::PushID(b.slot);
            bool hit;
            if (menu) {  // (its value is put together here, so already in the menus' language)
                opt_shown_next();
                hit = opt_action(L(b.label), val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            } else hit = opt_action(b.label, val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            ImGui::PopID();
            if (i == 0) take_focus(FOCUS_RIGHT);
            if (ImGui::IsItemFocused()) in_right = true;
            if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
            // X (the top button) / Delete / Backspace / right click: clear a menu shortcut (#591: Home free for overlays)
            if (menu && !waiting && cd < 0 &&
                ((ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) ||
                 ImGui::IsItemClicked(ImGuiMouseButton_Right))) {
                msg[0] = 0;
                bind_menu_clear(b.slot - BIND_MENU_1);
            }
            if (hit && menu && ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp)) hit = false;  // (ImGui activates on it too)
            if (hit) {
                if (waiting) bind_capture_cancel();
                else { msg[0] = 0; bind_capture_begin(dev, b.slot); }
            }
        }
        g_in_right = in_right;
        if (cd == dev && cs >= BIND_MENU_1) ImGui::TextDisabled("%s", L("Press one button, or two together. Esc cancels."));
        else if (cd == dev) ImGui::TextDisabled("%s", L("Esc cancels."));
        else if (msg[0]) ImGui::TextDisabled("%s", msg);
        else if (g_cat == CAT_MENU && g_dev_pad)
            ImGui::TextDisabled(L("%s or Delete clears a shortcut."), L(bind_pad_label(SDL_GAMEPAD_BUTTON_NORTH, fam)));
        else if (g_cat == CAT_MENU) ImGui::TextDisabled("%s", L("F1 and Esc always open this menu."));
    }
    opt_split_end();
}

// ------------------------------------------------------------------ local co-op (runtime/hle/coop.c)
// Right under Inputs: the player count and who has which controller and color first, then hearts, the camera and
// the rules, each under its own heading. The player count applies from the next file loaded (the extra Links are made
// with Link's own as a save loads); the tunic colors' file is part of the game's data from when it starts.
static void coop_options(void) {
    opt_section("Co-op");
    bool supported = coop_supported();
    {
        static const char *const players[] = {"Off", "2 players", "3 players", "4 players", "5 players", "6 players",
                                              "7 players", "8 players"};
        int i = supported ? g_settings.coop_players - 1 : 0;
        if (i < 0 || i > 7) i = 0;
        if (!supported) opt_disable_next();
        if (opt_choice("Players", &i, players, 8, 0,
                       supported ? "Local co-op: one Link per controller; Player 1 also has the keyboard. Starts when "
                                   "you next load a file. Lost? Press both sticks in (L3 + R3) to jump back to Player 1."
                                 : "Local co-op needs the game files of version 1.0.1 or 1.1.0.",
                       false))
            g_settings.coop_players = i + 1;
    }
    if (g_settings.coop_players <= 1 || !supported) return;
    opt_indent(1);
    if (int cs = coop_colours_state()) {
        static char where[1400];
        snprintf(where, sizeof where,
                 "The tunic colors need the co-op mod's Playerlink.bfres at %s. Without it every Link wears Player 1's "
                 "tunic.", coop_colours_path());
        opt_warn_next();
        opt_action("Tunic colors", cs == 1 ? "Quit to Start Screen" : "Not installed",
                   cs == 1 ? "The players' tunic colors load as the game starts. Quit to the Start Screen and play "
                             "again: until then every Link wears Player 1's tunic."
                           : where, false, false);
    }
    static const char *const colours[COOP_COLOURS] = {"Green", "Red", "Blue", "Yellow", "Purple", "Pink", "Black",
                                                      "White"};
    static char descs[8][200];
    for (int i = 0; i < g_settings.coop_players && i < 8; i++) {
        // (whose controller is whose: a Link that doesn't move is the likeliest first problem; a color two players
        // share, or no controller, shows in red)
        const char *pad = input_player_pad_name(i);
        bool same = false;
        for (int j = 0; j < g_settings.coop_players && j < 8; j++)
            same |= j != i && g_settings.coop_colour[j] == g_settings.coop_colour[i];
        char label[32];
        snprintf(label, sizeof label, "Player %d color", i + 1);
        if (i == 0)
            snprintf(descs[i], sizeof descs[i], "Player 1's tunic and hearts. Controller: keyboard%s%s.",
                     pad ? " and " : "", pad ? pad : "");
        else
            snprintf(descs[i], sizeof descs[i], "Player %d's tunic and hearts. Controller: %s.%s", i + 1,
                     pad ? pad : "none connected", same ? " Another player has this color too." : "");
        if ((i > 0 && !pad) || same) opt_warn_next();
        opt_choice(label, &g_settings.coop_colour[i], colours, COOP_COLOURS, i, descs[i], true);
    }
    opt_indent(-1);

    opt_section("Co-op hearts");
    static const char *const health[] = {"Shared", "Each player's own"};
    opt_choice("Hearts", &g_settings.coop_health, health, 2, COOP_HEALTH_OWN,
               "Shared: the game's one set. Own: a hit costs only that player's hearts; hearts picked up go to every "
               "player who is up.", false);
    if (g_settings.coop_health == COOP_HEALTH_OWN) {
        opt_indent(1);
        static const char *const ko[] = {"Knocked out", "Game over for all"};
        opt_choice("Out of hearts", &g_settings.coop_knockout, ko, 2, COOP_KO_REVIVE,
                   "Knocked out: lie down until a teammate holds A beside you, or Player 1 reaches a new area or "
                   "room. You come back with 30% of full hearts. Game over for all: as when Player 1 runs out.", false);
        if (g_settings.coop_knockout == COOP_KO_REVIVE) {
            opt_indent(1);
            opt_int("Revive time", &g_settings.coop_revive_s, 1, 10, 1, 4, "%d s",
                    "How long a teammate holds A beside a knocked-out player to revive them.");
            opt_indent(-1);
        }
        static const char *const hud[] = {"Bottom left", "Bottom right", "Top right"};
        opt_choice("Other players' hearts", &g_settings.coop_hud, hud, 3, COOP_HUD_BOTTOM_LEFT,
                   "Where players 2 and up show their hearts. In a bottom corner, past 3 players the rest go in the "
                   "other one.", false);
        opt_toggle("Hearts in player colors", &g_settings.coop_heart_colours, true,
                   "Each player's hearts in their color, Player 1's too (instead of the Cosmetics tab's Heart color).");
        opt_indent(-1);
    }

    opt_section("Co-op camera");
    static const char *const cams[] = {"Normal", "Out 5%", "Out 10%", "Out 25%", "Out 50%"};
    opt_choice("Camera zoom", &g_settings.coop_camera, cams, 5, COOP_CAM_NORMAL,
               "How far out the camera always is. Far out, the next screen's enemies may wait until you walk in.",
               false);
    opt_indent(1);
    if (g_settings.coop_camera == COOP_CAM_150) opt_disable_next();  // (already as far out as it goes)
    opt_toggle("Zoom out to fit everyone", &g_settings.coop_camera_fit, true,
               "Zooms out further as players spread out (up to 50%), and back in as you regroup.");
    opt_indent(-1);

    opt_section("Co-op rules");
    opt_toggle("Bring lost players back", &g_settings.coop_leash, true,
               "A player off screen for 3 seconds is brought back to Player 1 (knocked out: after 30 seconds, "
               "revived). Off: players can wander; L3 + R3 still brings one back.");
    opt_toggle("Only Player 1 triggers exits", &g_settings.coop_host_exits, true,
               "Doors, stairs and loading zones take everyone along only when Player 1 walks in. Off: anyone's "
               "step does.");
    opt_toggle("Enemies target the nearest player", &g_settings.coop_targets, true,
               "Each enemy chases the nearest player who is up and sticks with them while fighting. Off: they all "
               "chase Player 1.");
    opt_toggle("Players block each other", &g_settings.coop_collision, false,
               "On: the Links bump into each other; a player arriving on Player 1 passes through until they walk "
               "off the spot. Off: they walk through each other.");
}

// ------------------------------------------------------------------ the tab
extern "C" void controls_tab_draw(float scale) {
    int frame = ImGui::GetFrameCount();
    if (frame - g_last_frame > 1) g_sub = false, g_focus = FOCUS_NONE, g_in_right = false;  // (back on the tab)
    g_last_frame = frame;
    // (test) LARECOMP_CONTROLS_SUB=<buttons|triggers|sticks|dpad>[:right][:key]: on that page, the cursor on the category
    // (right: on its first binding; key: the keyboard's) - for a few frames, past the menu putting it on the tab bar
    static bool hook_done;
    static int hook_frames, hook_focus;
    if (!hook_done) {
        hook_done = true;
        if (const char *h = getenv("LARECOMP_CONTROLS_SUB")) {
            pick_device(strstr(h, ":key") != NULL);
            g_sub = true, hook_focus = strstr(h, "right") ? FOCUS_RIGHT : FOCUS_CAT, hook_frames = 10;
            for (int c = 0; c < N_CAT; c++) if (!strncmp(h, k_cat_hook[c], strlen(k_cat_hook[c]))) g_cat = c;
        }
    }
    if (hook_frames > 0) hook_frames--, g_focus = hook_focus;
    {   // a capture started elsewhere (LARECOMP_BIND_TEST): show it waiting
        int cd, cs;
        if (bind_capture_active(&cd, &cs, NULL) && (!g_sub || g_dev_pad != (cd == BIND_DEV_PAD) || g_cat != slot_cat(cs))) {
            if (!g_sub) pick_device(cd == BIND_DEV_KEY);
            g_sub = true, g_dev_pad = cd == BIND_DEV_PAD, g_cat = slot_cat(cs);
        }
    }
    if (g_sub) {
        configure_inputs(scale);
        if (g_sub) return;
    }

    opt_begin("##controls", scale);
    opt_section("Inputs");
    if (opt_link("Configure inputs", PLAT_TOUCH ? "Set the buttons for the game, and the buttons that open this menu. The "
                                                  "on-screen menu button always opens it."
                                                : "Set the buttons and keys for the game, and the buttons that open this menu. "
                                                  "F1 and Esc always open it.")) {
        pick_device(keyboard_pressed());
        g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
    }
    take_focus(FOCUS_CONFIGURE);

    coop_options();

    opt_section("Movement");
    {
        static const char *const modes[] = {LN("Nothing"), LN("Items"), LN("Movement")};
        opt_choice("D-pad", &g_settings.dpad_mode, modes, 3, DPAD_NOTHING, "What the D-pad does in play.");
    }
    opt_toggle("360-degree movement", &g_settings.move_360, false,
               "Link walks in the exact direction of the stick instead of snapping to 8 directions.");
    opt_child_begin(g_settings.move_360);
    if (!g_settings.move_360) opt_disable_next(), opt_needs_next("360-degree movement");
    opt_toggle("Hookshot aims in 8 directions", &g_settings.hookshot_8way, false,
               "The hookshot fires straight along the nearest of 8 directions, even with 360-degree movement.");
    opt_child_end();

    opt_section("Controller");
    opt_toggle("Rumble", &g_settings.rumble, true, "Controller vibration.");
    {
        static const char *const pct[] = {LN("25%"), LN("50%"), LN("75%"), LN("100%")};
        int i = g_settings.rumble_strength / 25 - 1;
        if (i < 0 || i > 3) i = 3;
        opt_child_begin(g_settings.rumble);
        if (!g_settings.rumble) opt_disable_next(), opt_needs_next("Rumble");
        if (opt_choice("Intensity", &i, pct, 4, 3, "How strong the vibration is.", false))
            g_settings.rumble_strength = (i + 1) * 25;
        opt_child_end();
    }
    opt_toggle("Controller light", &g_settings.controller_light, true, "The light bar shows Link's health.");
    {
        static const char *const fam[] = {LN("Nintendo"), LN("Xbox"), LN("PlayStation")};
        char autol[64];  // (in the menus' language already: the row leaves it as it is)
        snprintf(autol, sizeof autol, L("Auto (%s)"), L(fam[input_pad_family() % 3]));
        const char *names[] = {autol, LN("Nintendo"), LN("Xbox"), LN("PlayStation")};
        opt_choice("Button icons", &g_settings.prompts, names, 4, PROMPTS_AUTO,
                   "Which button icons the game shows. Auto matches the controller you have plugged in.");
    }
    {
        static const char *const names[] = {LN("Nintendo layout"), LN("Bottom button")};
        opt_choice("Confirm button", &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                   "Which button confirms in LAS Recomp's menus. Nintendo layout uses the right button like the Switch, "
                   "Bottom button uses the bottom one like Xbox and PlayStation.");
    }
    opt_toggle("Buttons match labels", &g_settings.face_by_label, false,
               "On Xbox and PlayStation controllers, A, B, X and Y work as printed on the controller, so the bottom "
               "button is A. Off keeps the Switch layout. Nintendo controllers aren't affected.");

    opt_end();
}

#if PLAT_TOUCH
// The Touch tab (phones and tablets): the on-screen controls. In play their looks open the live preview, the controls
// drawn over the game (menu.mm); before play there is nothing to show them over.
extern "C" void touch_tab_draw(float scale) {
    const bool live = game_in_play() != 0;
    auto row = [live](const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
        return opt_preview(label, v, names, n, def, desc, wrap, live);
    };
    opt_begin("##touch", scale);
    opt_section("Display");
    {
        static const char *const modes[] = {"Auto (no controller)", "Always", "Off"};
        row("Show", &g_settings.touch_mode, modes, 3, 0,
            "On-screen buttons for the touch screen. The middle button at the top opens this menu.", true);
        static const char *const off_on[] = {"Off", "On"};
        int hide = g_settings.touch_hide_xy_hud ? 1 : 0;
        if (row("Hide X/Y item HUD", &hide, off_on, 2, 1, "Hides the game's own X / Y items while the on-screen buttons are shown.", true))
            g_settings.touch_hide_xy_hud = hide != 0;
    }
    opt_section("Appearance");
    {
        // (steps of 5%: the preview's rows are lists of values)
        static char text[21][8];
        static const char *size_names[21], *op_names[18];
        static bool init;
        if (!init) {
            init = true;
            for (int i = 0; i < 21; i++) snprintf(text[i], 8, "%d%%", 60 + i * 5), size_names[i] = text[i];
        }
        static char op_text[18][8];
        for (int i = 0; i < 18; i++) snprintf(op_text[i], 8, "%d%%", 15 + i * 5), op_names[i] = op_text[i];
        int sz = (int)lroundf((g_settings.touch_size * 100 - 60) / 5), op = (int)lroundf((g_settings.touch_opacity * 100 - 15) / 5);
        sz = sz < 0 ? 0 : sz > 20 ? 20 : sz, op = op < 0 ? 0 : op > 17 ? 17 : op;
        if (row("Size", &sz, size_names, 21, 8, "The on-screen buttons' size.", false)) g_settings.touch_size = (60 + sz * 5) / 100.0f;
        if (row("Opacity", &op, op_names, 18, 9, "How see-through the on-screen buttons are.", false))
            g_settings.touch_opacity = (15 + op * 5) / 100.0f;
    }
    opt_section("Layout");
    {
        static const char *const move[] = {"Stick", "D-pad"};
        int v = g_settings.touch_dpad ? 1 : 0;
        if (row("Movement", &v, move, 2, 0, "Stick: any direction. D-pad: eight directions.", true)) g_settings.touch_dpad = v != 0;
        bool f = !g_settings.touch_stick_fixed;
        if (opt_toggle("Follow thumb", &f, true,
                       "On: it comes to where your thumb lands, anywhere on the lower left. Off: it stays where it is "
                       "(move it with the Layout button)."))
            g_settings.touch_stick_fixed = !f;
        // the layout editor's own button on the screen (mobile/touch_controls.mm), beside the menu's: off once the
        // layout is set
        opt_toggle("Layout button", &g_settings.touch_edit_button, true,
                   "A button at the top, next to the menu button, that lets you move and resize the on-screen buttons.");
        if (opt_action("Reset touch layout", "", "Puts every on-screen button back in its place, at its size, shown."))
            touch_layout_reset();
    }
    opt_end();
}
#endif
