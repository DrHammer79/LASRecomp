// The settings' Controls tab (in game: menu.mm; start screen: launcher.mm, both through menu_tabs_draw), in option rows
// (runtime/ui/option_rows.h), and its Configure inputs page for the bindings (runtime/input_bind.h): DEVICE and the
// categories on the left, the chosen category's bindings on the right. A on a category moves the cursor to its
// bindings; A on a binding waits for the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over); B goes back a step (bindings -> categories -> the tab). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible)
#include "i18n.h"
#include "option_rows.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"

extern "C" int coop_colours_state(void);    // (runtime/hle/coop.c: 0 the tunic colours are in, 1 after a restart, 2 no file)
extern "C" const char *coop_colours_path(void);  // (... where that file goes)
extern "C" bool coop_supported(void);       // (... false once the game is a version without co-op)
extern "C" const char *input_player_pad_name(int player);  // (runtime/hle/input.c: a player's controller, or NULL)
extern "C" void input_assign_begin(int player);  // (runtime/hle/input.c: the next button pressed gives its controller)
extern "C" int input_assign_waiting(void);       // (... the player waiting for one, or -1)
int input_pad_family(void);              // runtime/hle/input.c
SDL_JoystickID input_active_pad(void);   // runtime/hle/input.c: the controller the game reads (0: none yet)
void input_apply_background(void);
}

// ------------------------------------------------------------------ Configure inputs: what's on it
enum { CAT_BUTTONS, CAT_TRIGGERS, CAT_STICKS, CAT_DPAD, N_CAT };
static const char *const k_cat_name[N_CAT] = {LN("Buttons"), LN("Triggers"), LN("Sticks"), LN("D-pad")};
static const char *const k_cat_hook[N_CAT] = {"buttons", "triggers", "sticks", "dpad"};  // (LARECOMP_CONTROLS_SUB)

struct Bind { int slot; const char *label; const char *section; };  // section: a header before this row
static int cat_rows(int cat, bool pad, Bind *out) {
    int n = 0;
    auto add = [&](int slot, const char *label, const char *section = NULL) { out[n++] = {slot, label, section}; };
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
    }
    return n;
}
static int slot_cat(int slot) {
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

// A finished capture keeps larecomp.ini current; a swap is said under the list (another row changed too)
static void watch_results(int fam, char *msg, size_t cap) {
    static int seen = -1;
    int rdev, rslot, rother, serial, kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen < 0) seen = serial;
    if (serial == seen) return;
    seen = serial;
    msg[0] = 0;
    if (kind == 2)
        snprintf(msg, cap, L("Swapped. %s is on %s now."), L(bind_input_name(rother)),
                 L(rdev == BIND_DEV_PAD ? bind_pad_label(bind_pad_host(rother), fam) : bind_key_label(g_bind_key[rother])));
    if (kind == 1 || kind == 2 || kind == 5) settings_save();
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

    opt_begin("##configure", scale);
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
        if (open) { if (g_cat != c) msg[0] = 0; g_cat = c, g_focus = FOCUS_RIGHT; bind_capture_cancel(); }
    }
    if (bind_is_default(dev)) opt_disable_next();
    if (opt_action("Restore default controls", "", g_dev_pad ? "Puts every controller button back to its default."
                                                            : "Puts every key back to its default.")) {
        bind_capture_cancel();
        bind_reset(dev);
    }
    // (the cursor or the mouse on a row with a description: it shows on the right instead of the bindings)
    bool desc_right = (ImGui::IsItemHovered() || nav_on_item()) && g_focus != FOCUS_RIGHT;
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
            bool waiting = cd == dev && cs == b.slot;
            char val[64];
            if (waiting) snprintf(val, sizeof val, g_dev_pad ? L("Press a button...  %d") : L("Press a key...  %d"), (ms + 999) / 1000);
            else snprintf(val, sizeof val, "%s", g_dev_pad ? bind_pad_label(bind_pad_host(b.slot), fam) : bind_key_label(g_bind_key[b.slot]));
            // the same button / key on two inputs (a hand-edited larecomp.ini): red
            const int16_t *t = g_dev_pad ? g_bind_pad : g_bind_key;
            bool dup = false;
            for (int j = 0, m = g_dev_pad ? BIND_PAD_COUNT : BIND_KEY_COUNT; j < m; j++)
                dup |= j != b.slot && t[j] == t[b.slot] && t[b.slot] > (g_dev_pad ? -1 : 0);
            if (dup && !waiting) opt_warn_next();
            ImGui::PushID(b.slot);
            bool hit = opt_action(b.label, val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            ImGui::PopID();
            if (i == 0) take_focus(FOCUS_RIGHT);
            if (ImGui::IsItemFocused()) in_right = true;
            if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
            if (hit) {
                if (waiting) bind_capture_cancel();
                else { msg[0] = 0; bind_capture_begin(dev, b.slot); }
            }
        }
        g_in_right = in_right;
        if (cd == dev) ImGui::TextDisabled("%s", L("Esc cancels."));
        else if (msg[0]) ImGui::TextDisabled("%s", msg);
    }
    opt_split_end();
}

// ------------------------------------------------------------------ local co-op (runtime/hle/coop.c)
// Right under Inputs: the player count and who has which controller and color first, then hearts, the camera and
// the rules, each under its own heading. The player count applies from the next file loaded (the extra Links are made
// with Link's own as a save loads); the tunic colors' file is part of the game's data from when it starts.
// The camera's zoom is there without co-op too (its "Always"); fitting everyone in view only with it.
static void zoom_options(bool coop) {
    opt_section("Camera");
    static const char *const when[] = {"With co-op", "Always", "Off"};
    opt_choice("Zoom out", &g_settings.coop_zoom_mode, when, 3, COOP_ZOOM_WITH_COOP,
               "With co-op: the camera zooms out as set below with 2 players or more. Always: playing alone too. "
               "Off: the game's own camera.", false);
    if (g_settings.coop_zoom_mode == COOP_ZOOM_OFF || (!coop && g_settings.coop_zoom_mode == COOP_ZOOM_WITH_COOP))
        return;
    opt_indent(1);
    static const char *const outs[] = {"Normal", "Out 10%", "Out 20%", "Out 30%", "Out 40%", "Out 50%", "Out 60%",
                                       "Out 70%", "Out 80%", "Out 90%", "Out 100%", "Out 110%", "Out 120%",
                                       "Out 130%", "Out 140%", "Out 150%", "Out 160%", "Out 170%", "Out 180%",
                                       "Out 190%", "Out 200%"};
    opt_choice("Camera zoom", &g_settings.coop_zoom, outs, COOP_ZOOM_MAX + 1, 5,
               "How far out the camera always is outdoors.", false);
    if (coop) {
        opt_indent(1);
        opt_toggle("Zoom out to fit everyone", &g_settings.coop_camera_fit, true,
                   "Zooms out further as players spread out, and back in as you regroup - never closer than Camera "
                   "zoom.");
        if (g_settings.coop_camera_fit) {
            opt_indent(1);
            int f = g_settings.coop_zoom_fit - 1;
            if (opt_choice("Fit up to", &f, outs + 1, COOP_ZOOM_FIT_MAX, COOP_ZOOM_FIT_MAX - 1,
                           "The furthest out it zooms to fit everyone. Below Camera zoom, Camera zoom is the furthest. "
                           "Far out, the overworld loads more around Player 1.",
                           false))
                g_settings.coop_zoom_fit = f + 1;
            opt_indent(-1);
        }
        opt_indent(-1);
    }
    opt_choice("Indoor zoom", &g_settings.coop_zoom_indoor, outs, COOP_ZOOM_MAX + 1, 0,
               "Houses, caves and dungeons: the game frames each room to fit the screen, so zoomed out it sits small "
               "in black. Normal keeps the game's own view.", false);
    opt_indent(-1);
}

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
    if (g_settings.coop_players <= 1 || !supported) {
        if (supported) zoom_options(false);
        return;
    }
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
    static char descs[8][360];
#ifdef _WIN32
    // (Windows: Xbox-type controllers come through XInput, 4 at most - SDL's RawInput / WGI drivers are off by
    // default. An 8BitDo SN30 Pro in X-input mode, the fifth with four Xbox 360 pads in, didn't even start in Windows
    // (code 10, user 2026-10-30); in Switch or D-input mode it isn't one)
    const char *xinput = " On Windows the game takes 4 Xbox-type controllers at most: start a fifth in another mode "
                         "(an 8BitDo's Switch mode, say).";
#else
    const char *xinput = "";
#endif
    for (int i = 0; i < g_settings.coop_players && i < 8; i++) {
        // (whose controller is whose: a Link that doesn't move is the likeliest first problem; a color two players
        // share, or no controller, shows in red - with a note saying which: the red alone was taken for a color clash)
        const char *pad = input_player_pad_name(i);
        int same = 0;  // (the first other player with this color, from 1)
        for (int j = 0; j < g_settings.coop_players && j < 8 && !same; j++)
            if (j != i && g_settings.coop_colour[j] == g_settings.coop_colour[i]) same = j + 1;
        char label[32], note[48] = "", also[48] = "";
        snprintf(label, sizeof label, "Player %d color", i + 1);
        if (same) snprintf(also, sizeof also, " Player %d has this color too.", same);
        if (i == 0)
            snprintf(descs[i], sizeof descs[i], "Player 1's tunic and hearts. Controller: keyboard%s%s.%s",
                     pad ? " and " : "", pad ? pad : "", also);
        else
            snprintf(descs[i], sizeof descs[i], "Player %d's tunic and hearts. Controller: %s.%s%s", i + 1,
                     pad ? pad : "none connected - this Link stands still", !pad && i >= 4 ? xinput : "", also);
        if (i > 0 && !pad) snprintf(note, sizeof note, "no controller");
        if (same) snprintf(note + strlen(note), sizeof note - strlen(note), "%ssame as Player %d", *note ? ", " : "", same);
        if (*note) opt_warn_next(), opt_note_next(note);
        opt_choice(label, &g_settings.coop_colour[i], colours, COOP_COLOURS, i, descs[i], true);
    }
    // whose controller is whose (runtime/hle/input.c input_assign_*): the row, then a button on the controller wanted -
    // swapped with whoever had it, and remembered (user 2026-10-30: "we need a way to assign controllers to a player")
    int waiting = input_assign_waiting();
    static int was_waiting = -1;  // (it stopped waiting: assigned - each player's controller kept - or given up)
    if (was_waiting >= 0 && waiting < 0) settings_save();
    was_waiting = waiting;
    static char pdesc[8][300];
    for (int i = 0; i < g_settings.coop_players && i < 8; i++) {
        const char *pad = input_player_pad_name(i);
        char label[32];
        snprintf(label, sizeof label, "Player %d controller", i + 1);
        snprintf(pdesc[i], sizeof pdesc[i],
                 "Choose this, then press any button on the controller Player %d should use. If another player had it, "
                 "they get Player %d's in its place. Remembered for next time. Escape cancels.%s", i + 1, i + 1,
                 i ? "" : " Player 1 keeps the keyboard too.");
        if (waiting == i) opt_mark_next();
        if (opt_action(label, waiting == i ? "Press a button..." : pad ? pad : i ? "None" : "Keyboard only", pdesc[i],
                       true, false))
            input_assign_begin(i);
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

    zoom_options(true);

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
    if (opt_link("Configure inputs", "Set the buttons and keys for the game. F1, Esc, Home or Back + Start always "
                                     "open this menu.")) {
        pick_device(keyboard_pressed());
        g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
    }
    take_focus(FOCUS_CONFIGURE);
    if (opt_toggle("Allow background inputs", &g_settings.bg_input, false,
                   "Controllers keep working while another window is in front. macOS may ask for Input Monitoring "
                   "permission."))
        input_apply_background();

    coop_options();

    opt_section("Movement");
    {
        static const char *const modes[] = {LN("Nothing"), LN("Items"), LN("Movement")};
        opt_choice("D-pad", &g_settings.dpad_mode, modes, 3, DPAD_NOTHING, "What the D-pad does in play.");
    }
    opt_toggle("360-degree movement", &g_settings.move_360, false,
               "Link walks in the exact direction of the stick instead of snapping to 8 directions.");

    opt_section("Controller");
    opt_toggle("Rumble", &g_settings.rumble, true, "Controller vibration.");
    if (g_settings.rumble) {
        static const char *const pct[] = {LN("25%"), LN("50%"), LN("75%"), LN("100%")};
        int i = g_settings.rumble_strength / 25 - 1;
        if (i < 0 || i > 3) i = 3;
        opt_indent(1);
        if (opt_choice("Intensity", &i, pct, 4, 3, "How strong the vibration is.", false))
            g_settings.rumble_strength = (i + 1) * 25;
        opt_indent(-1);
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
