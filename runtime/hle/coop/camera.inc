// Local co-op, the cameras, the zoom (moved back on the Field, the field of view indoors), the Field's boxes of screens as the zoom grows them. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// ---- Cameras (COOP_CAMERA_SITE: x23 a camera whose view matrix was just set - columns at +0x580 / +0x590 / +0x5a0
// and the translation at +0x5b0; vertical field of view +0x564, radians). The view is Player 1's own camera (every
// Link has one, +0x318: COOP_CAMERA_SCROLL_SITE) when it's among them, else the one Player 1 is in front of and near
// the middle of; the HUD projects a point with it (player_screen).
#define CAMS 12
static uint64_t g_cam[CAMS], g_main_cam;
// The HUD's (runtime/ui/coop_hud.mm: the UI thread): Player 1's Link and the view camera as the frame starts
// (coop_tick, on the game thread) - not as that thread has them mid-update: an enemy's update points the player slot at
// the Link it targets (actor_exec_begin), and the camera list changes as cameras are seen.
static _Atomic uint64_t g_hud_host, g_hud_cam;
static uint64_t g_back_cam;  // (the camera the zoom moved back: camera_back)

static void cameras_forget(void) {
    memset(g_cam, 0, sizeof g_cam);
    g_main_cam = 0, g_hud_host = g_hud_cam = 0, g_back_cam = 0;
}
RT_ON_RESET(coop_cameras_reset) { cameras_forget(); }

static void camera_seen(uint64_t cam) {
    for (int i = 0; i < CAMS; i++)
        if (g_cam[i] == cam) return;
    memmove(g_cam + 1, g_cam, sizeof g_cam - sizeof g_cam[0]);
    g_cam[0] = cam;
}

static bool cam_view(uint64_t cam, const float p[3], float v[3]) {
    const float *m = (const float *)(uintptr_t)(cam + 0x580);
    for (int k = 0; k < 3; k++) v[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
    return v[2] < -0.5f;  // (in front)
}

static uint64_t view_camera(void) {
    uint64_t me = host_player(), mine = me ? LD64(me + 0x318) : 0;
    float host[3], v[3];
    for (int i = 0; mine && i < CAMS; i++)
        if (g_cam[i] == mine) return g_main_cam = mine;
    if (player_pos(me, host))  // (else the camera Player 1 is nearest the middle of)
        for (int i = 0; i < CAMS; i++)
            if (g_cam[i] && cam_view(g_cam[i], host, v) && fabsf(v[0]) < -v[2] && fabsf(v[1]) < -v[2])
                return g_main_cam = g_cam[i];
    return g_main_cam;
}

// World point -> where it is in camera cam's view (view_camera()): x, y from -1 to 1 across it (out of that range: off
// screen), *front whether it is in front of the camera. False without a camera yet.
static bool view_point(uint64_t cam, const float p[3], float aspect, float *nx, float *ny, bool *front) {
    float v[3];
    if (!cam) return false;
    *front = cam_view(cam, p, v);
    extern float rt_fovy(float);  // (runtime/hle/widescreen.c: the view as drawn - tall windows, the zoom)
    float fovy = rt_fovy(*(const float *)(uintptr_t)(cam + 0x564)), t = tanf(fovy * 0.5f);
    if (!(t > 0.01f && t < 10)) return false;
    float d = fabsf(v[2]) > 0.5f ? fabsf(v[2]) : 0.5f;
    *ny = v[1] / (d * t), *nx = v[0] / (d * t * aspect);
    return true;
}

// World point -> window position (pixels, for a w x h window); false when off screen or no camera yet. (Game thread:
// the LARECOMP_COOP_LOG line.)
static bool coop_project(const float p[3], float w, float h, float *sx, float *sy) {
    float nx, ny;
    bool front;
    if (!view_point(view_camera(), p, w / h, &nx, &ny, &front) || !front) return false;
    *sx = (nx * 0.5f + 0.5f) * w, *sy = (0.5f - ny * 0.5f) * h;
    return nx > -1.2f && nx < 1.2f && ny > -1.2f && ny < 1.2f;
}

// A player on screen: his head (just above him: lying down) for the "Zzz" bubble, or his feet (below him) for the
// "A Revive" prompt. Off screen, or behind the camera, it's kept to the window's edge in his direction (*edge): the
// bubble marks where a knocked-out player was left. (UI thread: the frame's g_hud_host / g_hud_cam. Guest memory stays
// mapped, so a camera gone a moment ago reads as garbage - off screen - not a fault.)
static bool player_screen(int i, float lift, float w, float h, float *sx, float *sy, bool *edge) {
    float p[3], nx, ny;
    bool front;
    if (i < 0 || i >= COOP_MAX || g_n < 2 || !player_pos(i ? g_extra[i] : g_hud_host, p)) return false;
    p[1] += lift;
    if (!view_point(g_hud_cam, p, w / h, &nx, &ny, &front)) return false;
    if (!front) nx = -nx, ny = -ny;  // (behind: mirrored through the camera)
    const float mx = 0.9f, my = 0.8f;  // (the bubble's room at the edges)
    float k = fmaxf(fabsf(nx) / mx, fabsf(ny) / my);
    *edge = !front || k > 1;
    if (*edge && k > 0) nx /= fmaxf(k, 1), ny /= fmaxf(k, 1);
    *sx = (nx * 0.5f + 0.5f) * w, *sy = (0.5f - ny * 0.5f) * h;
    return true;
}

// ---- The camera zoomed out (settings coop_zoom, coop_camera_fit, coop_zoom_fit): g_zoom, coop_zoom 10% steps out
// always (up to 60%); with coop_camera_fit, further out as far as it takes to keep every player who is up in view, up
// to coop_zoom_fit steps (by default 100%: twice as far; up to 200%, three times, set so), never in of coop_zoom, eased in and out. 1 out of play and
// while zoom_on() isn't. On the Field the camera is moved back by it (camera_back); indoors its field of view is widened
// instead (runtime/hle/widescreen.c rt_fovy, also the culling pass's).
// Widening the view on the Field first (to 2.3): its top edge ran toward the horizon, so each step out needed far more
// of the world made and drawn - at 2.5 the game's own thread took 13 ms a frame, and its frame pacing fell to 30 fps.
// Moved back, the view grows evenly: at 3 the game's thread took 6.6 ms, with the screens it makes anyway.
static float g_zoom = 1.0f;
static bool g_back_on;          // (the zoom moves the camera back: on the Field)
float coop_camera_zoom(void) { return g_back_on ? 1.0f : g_zoom; }  // (the field of view's: rt_fovy, effect stretch)
float coop_camera_reach(void) { return g_zoom; }                    // (how much further the view reaches)

// The zoom by setting coop_zoom_mode: with co-op (2 players or more), playing alone too, or never. Alone, only its
// own hooks run (rt_coop_hook: the camera, the Field's boxes of screens); with no other player its fit is the least.
static bool zoom_on(void) {
    int m = g_settings.coop_zoom_mode;
    return GA(COOP_CAMERA_SITE) && m != COOP_ZOOM_OFF && (m == COOP_ZOOM_ALWAYS || rt_coop_on());
}

// The camera moved back (COOP_CAMERA_SITE: its view matrix just set). On the Field each frame: the view camera's eye -
// its node's position (+0x510, +0xb0), which the culling pass builds its own view from - goes back along its view
// axis by g_back, Player 1's distance from it times g_zoom - 1, and so do the view matrix (+0x580) and the renderer's
// copy of it (x24 +0x20). The camera rebuilds its eye from what it follows every frame, so the move doesn't add up.
// With it, what the game keeps by that distance (each the game's own value plus g_back, put back with g_back 0):
//   the far plane (+0x56c: at 3x the far ground was cut off, black) and the shadows' reach (+0x560, +0x350, +0x734:
//   24 each; past it a wedge of the ground was all in shadow); the tilt-shift's focus (qol.c rt_tilt_focus_add: Player
//   1 was blurred, the sharp band nearer the screen's bottom).
// The eye's move is taken off again as the next frame starts (camera_back_undo, from coop_tick, as the game reads the
// controllers - before the camera's update): a camera that goes on from where its eye is - the walk in through a
// dungeon's door glides it - went further back each frame, to 120 away (user 2026-10-28, Angler's Tunnel). So are the
// values kept by the distance: the camera eases some of them itself, and from ours each frame it stopped short of
// where it eases them to - an event waiting for it to get there waited for ever (the ghost's grave: the owl flew off,
// the camera came back to Player 1, and he stayed in the event; user 2026-10-30, three times).
static float g_back, g_back_d0 = 24.8f;  // (the distance moved back; Player 1's distance from the eye before it)
static uint64_t g_eye_node;              // (the node whose eye was moved, 0: none)
static float g_eye_wrote[3], g_eye_moved[3];
// The values kept by the distance (camera_back): the camera they're in (g_back_cam), its own, ours.
static const int k_back_keep[] = {0x560, 0x56c, 0x350, 0x734};
static float g_back_orig[4], g_back_wrote[4];
static void camera_back_put(void) {  // (the camera's own values back in it, where they're still ours)
    if (!heap_ptr(g_back_cam)) return;
    for (int k = 0; k < 4; k++) {
        float *f = (float *)(uintptr_t)(g_back_cam + k_back_keep[k]);
        if (*f == g_back_wrote[k]) *f = g_back_orig[k];
    }
}
static void camera_back_undo(void) {
    float *eye = g_eye_node && heap_ptr(g_eye_node) ? (float *)(uintptr_t)(g_eye_node + 0xb0) : NULL;
    if (eye && !memcmp(eye, g_eye_wrote, sizeof g_eye_wrote))  // (as we left it: the camera hasn't been updated since)
        for (int k = 0; k < 3; k++) eye[k] -= g_eye_moved[k];
    g_eye_node = 0;
    camera_back_put();
}
// The view matrix as moved (its +14 and the renderer's copy's): one the game hasn't set again since isn't moved again.
// While the pause menu is up the game leaves it as it was, and moved again each frame it went 12 units further back a
// frame - thousands away by the time a file was loaded from the menu, and the game hung (user 2026-10-30, File 3; as old
// as camera_back): then only what the frame's start took off is put back.
static float g_m14_wrote, g_copy14_wrote;
static void camera_back(Cpu *c) {
    uint64_t cam = c->x[23], node = LD64(cam + 0x510);
    float p[3], v[3];
    if (cam != view_camera() || !heap_ptr(node)) return;
    float *m = (float *)(uintptr_t)(cam + 0x580), *copy = (float *)(uintptr_t)(c->x[24] + 0x20);
    float *eye = (float *)(uintptr_t)(node + 0xb0);
    if (cam == g_back_cam && g_back != 0 && m[14] == g_m14_wrote && copy[14] == g_copy14_wrote) {
        bool undone = !g_eye_node;  // (the frame's start took the eye's move off, and the kept values)
        for (int k = 0; undone && k < 3; k++) undone = eye[k] == g_eye_wrote[k] - g_eye_moved[k];
        if (undone) {
            for (int k = 0; k < 3; k++) eye[k] = g_eye_wrote[k];
            g_eye_node = node;
        }
        for (int k = 0; k < 4; k++) {
            float *f = (float *)(uintptr_t)(cam + k_back_keep[k]);
            if (*f == g_back_orig[k]) *f = g_back_wrote[k];
        }
        return;
    }
    float back = 0;
    if (g_back_on && g_zoom > 1.0f && player_pos(host_player(), p) && cam_view(cam, p, v))  // (a camera not on him: 24.8)
        g_back_d0 = -v[2] > 5.0f && -v[2] < 60.0f ? -v[2] : 24.8f, back = (g_zoom - 1.0f) * g_back_d0;
    // (another view camera: the last one's values put back first - left moved back, its own were read from them when
    // it was the view again, and each time the move added up: the far plane and the shadows' reach grew for good)
    if (cam != g_back_cam) camera_back_put();
    for (int k = 0; k < 4; k++) {
        float *f = (float *)(uintptr_t)(cam + k_back_keep[k]);
        if (cam != g_back_cam || *f != g_back_wrote[k]) g_back_orig[k] = *f;  // (the game's own: a new camera, or it changed it)
        *f = g_back_wrote[k] = g_back_orig[k] + back;
    }
    extern void rt_tilt_focus_add(float);  // runtime/hle/qol.c
    rt_tilt_focus_add(back);
    g_back = back;
    if (g_eye_node == node) camera_back_undo();  // (not taken off yet - two camera updates in a frame: now)
    for (int k = 0; k < 3; k++)  // (row 2: its back axis)
        g_eye_moved[k] = back * m[2 + 4 * k], eye[k] += g_eye_moved[k];
    memcpy(g_eye_wrote, eye, sizeof g_eye_wrote);
    g_eye_node = back != 0 ? node : 0;
    g_back_cam = cam;
    m[14] -= back, copy[14] -= back;
    g_m14_wrote = m[14], g_copy14_wrote = copy[14];
}

// The zoom at which a point is inside frac of the view, the camera moved back (1: in it now, or nearer). From the
// view before the move (the matrix holds it moved by g_back): x across, y up, depth d. Moved back by B = (Z - 1) d0,
// it is in when |x| <= frac (d + B) tan(fovy / 2) aspect, and as much for y.
static float back_need(uint64_t cam, const float p[3], float aspect, float frac) {
    float v[3];
    if (!cam) return 0;
    cam_view(cam, p, v);
    extern float rt_fovy(float);
    float t = tanf(rt_fovy(*(const float *)(uintptr_t)(cam + 0x564)) * 0.5f), d = -(v[2] + g_back);
    if (!(t > 0.01f && t < 10)) return 0;
    float zx = 1.0f + (fabsf(v[0]) / (frac * t * aspect) - d) / g_back_d0, zy = 1.0f + (fabsf(v[1]) / (frac * t) - d) / g_back_d0;
    return fmaxf(zx, zy);
}

// The level's screens around Player 1 (main+0x85c280, each frame; COOP_LOAD_BOX_SITE...). On the Field each screen
// (15 x 12) has its objects made - tall grass, grass, rocks, trees, enemies, and its ground (MapStatic) - once it meets
// a 75 x 60 box centred on Player 1 (7 x 7 screens), and loses them outside 90 x 72; only those meeting a much smaller
// box (about 4 x 3 screens) are active: ground solid, objects running. Zoomed out the view reached past the screens
// made, where the ground shows bare earth for tall grass (dark ovals - the "weird shadows" zoomed far out, user report
// 2026-10-28), and another player in view one screen past the active ones fell through the ground. With co-op the
// active box takes in every player (active_box), and the boxes that make and keep the screens grow by g_box with the
// zoom, up to BOX_MAX.
#define SCREEN_W 15.0f  // a Field screen (Field_01A .. Field_16P), across
#define SCREEN_D 12.0f  // ... and deep
#define LOAD_W 75.0f    // the box whose screens' objects are made (main+0x1701e40, the Field's)
#define LOAD_D 60.0f
#define BOX_STEP 0.25f
#define BOX_MAX 2.0f
#define BOX_LOAD_FRAMES 90     // (the screens a grown box takes in: their objects made about 40 frames on)
#define BOX_SHRINK_FRAMES 300  // (5 s needing less before it shrinks back)
static float g_box = 1.0f;        // the boxes' scale
static float g_box_ready = 1.0f;  // ... the one whose screens are made by now
static uint64_t g_box_at, g_box_less;
static bool on_field(void) {  // (the overworld: its screens made and unloaded around Player 1)
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    const char *lv = chaos_current_level();
    return lv && !strcmp(lv, "Field");
}
static bool zoom_field(void) { return zoom_on() && on_field(); }

// The furthest the zoom can go before the view reaches past the screens a box of that scale makes (their edge, on the
// ground at Player 1's height, less ROOM_MARGIN: a tall grass patch's reach, ground lower than him), or 0 (no camera).
#define ROOM_MARGIN 2.0f
#define ROOM_STEPS 32  // (points along each side)
static float zoom_room(float aspect, float box) {
    float p[3], most = 1e9f;
    uint64_t cam = view_camera();
    if (!player_pos(coop_player(0), p) || !cam) return 0;
    float x0 = SCREEN_W * floorf((p[0] - LOAD_W * box / 2) / SCREEN_W) + ROOM_MARGIN;
    float x1 = SCREEN_W * ceilf((p[0] + LOAD_W * box / 2) / SCREEN_W) - ROOM_MARGIN;
    float z0 = SCREEN_D * floorf((p[2] - LOAD_D * box / 2) / SCREEN_D) + ROOM_MARGIN;
    float z1 = SCREEN_D * ceilf((p[2] + LOAD_D * box / 2) / SCREEN_D) - ROOM_MARGIN;
    for (int k = 0; k < 4 * ROOM_STEPS; k++) {
        float t = (float)(k % ROOM_STEPS) / ROOM_STEPS, side = k / ROOM_STEPS;
        float c[3] = {side == 0 ? x0 : side == 1 ? x1 : x0 + t * (x1 - x0), p[1],
                      side >= 2 ? (side == 2 ? z0 : z1) : z0 + t * (z1 - z0)};
        float z = back_need(cam, c, aspect, 1.0f);  // (the zoom it comes into view at)
        if (z > 0) most = fminf(most, z);
    }
    return most < 1e9f ? most : 0;
}

// The box for a zoom wanted: grown at once, shrunk after BOX_SHRINK_FRAMES wanting less; g_box_ready catches up once
// the screens it takes in are made.
static void box_tick(float need, float aspect) {
    uint64_t now = renderer_present_count();
    float box = 1.0f, room = 0;
    while (box < BOX_MAX && (room = zoom_room(aspect, box)) > 0 && room < need) box += BOX_STEP;
    if (!(room > 0)) return;  // (no camera or no Player 1 yet - a file loading: the boxes as they are, not the largest)
    if (box >= g_box) g_box_less = 0;
    if (box > g_box) g_box = box, g_box_at = now;
    else if (box < g_box && !g_box_less) g_box_less = now;
    else if (box < g_box && now - g_box_less > BOX_SHRINK_FRAMES) g_box = g_box_ready = box, g_box_less = 0;
    if (now - g_box_at >= BOX_LOAD_FRAMES) g_box_ready = g_box;
}
RT_ON_RESET(coop_box_reset) { g_box = g_box_ready = 1.0f, g_box_at = g_box_less = 0; }

// Indoors - any level but the Field: houses, caves, dungeons - only the room the camera frames is drawn, one screen:
// zoomed out it sat small in black (at 2x, every dungeon and house in build/zoom_audit.sh), most houses with their
// lighting's glow a hard circle. There the zoom is coop_zoom_indoor (Normal by default), without fit.
static void zoom_tick(bool play) {
    extern float widescreen_factor(void);  // (runtime/hle/widescreen.c: the window's aspect over 16:9)
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    const char *lv = chaos_current_level();
    bool indoor = lv && *lv && strcmp(lv, "Field");
    float base = 1.0f + 0.1f * (indoor ? g_settings.coop_zoom_indoor : g_settings.coop_zoom);
    float top = indoor ? base : fmaxf(base, 1.0f + 0.1f * g_settings.coop_zoom_fit), want = base;
    float aspect = 16.0f / 9.0f * widescreen_factor();
    bool fit = g_settings.coop_camera_fit && top > base;
    g_back_on = play && zoom_field();
    if (play && g_held_at) return;  // (the pause menu's hold: everyone on Player 1's spot - the zoom stays as it was)
    if (!play || !zoom_on()) want = 1.0f;
    else if (fit) {  // (each player who is up inside 0.7 of the view - not one lost below the world; never in of base)
        float p[3];
        uint64_t cam = view_camera();
        for (int i = 1; i < g_n; i++)
            if (!g_down[i] && !g_lost_until[i] && player_pos(g_extra[i], p) && p[1] > -500.0f)
                want = fmaxf(want, back_need(cam, p, aspect, 0.7f));
        want = fminf(want, top);
    }
    // (out of play, without fit, and once the zoom is off - switched off in play, the view widened as it eased in)
    if (!fit || !play || !zoom_on()) g_zoom = want;
    else g_zoom += (want - g_zoom) * (want > g_zoom ? 0.06f : 0.02f);  // (out quicker than back in)
    if (fabsf(g_zoom - want) < 0.002f) g_zoom = want;
    const char *forced = play && zoom_on() ? ENV("LARECOMP_COOP_ZOOM") : NULL;  // (test)
    if (g_back_on) {  // (inside the screens made around Player 1: at once, not eased)
        box_tick(forced ? (float)atof(forced) : want, aspect);
        float room = g_zoom > 1.0f ? zoom_room(aspect, g_box_ready) : 0;
        if (room > 0) g_zoom = fmaxf(fminf(g_zoom, room), 1.0f);
    } else
        g_box = g_box_ready = 1.0f;
    if (forced) g_zoom = (float)atof(forced);
}

