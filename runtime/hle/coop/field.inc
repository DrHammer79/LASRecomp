// Local co-op, cut grass and thrown rocks kept gone, the screens kept and active around every player, the shadow map's box. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// ---- Cut grass and thrown rocks. On the Field the game makes a level object - grass, a rock - again when Player 1
// steps into its screen and its actor is gone: the screens come back fresh, as on the Game Boy. Alone, what Link cut
// or threw is in the screen he just left, scrolled out of view. With co-op the others cut and throw in screens
// Player 1 isn't in, and the view shows the screens around: grass and rocks came back beside the players - a thrown
// rock back on the stairs in Player 1's way (user's log 2026-10-28: the stairs' rock, ObjRock-4800A5005B88BEDC, back
// as he stepped onto them). Each one destroyed is remembered by its actor's name (the level object's id), and one made
// again while a player is near it or it's in view is killed at once (ACTOR_KILL, as its own break does), before its
// first update: it stays gone until it comes back where nobody sees it. Not indoors: a room comes back whole as Player
// 1 walks in, with everyone, as without co-op.
// What counts as destroyed: dying in its own update (cut, or thrown and broken), or dying elsewhere - in another
// actor's update: lifted, cut by another player's sword - with a player within GONE_TAKER of it, or in a screen well
// inside those the game keeps (gone_kept_screen: a boomerang's, an arrow's, a bomb's from afar). The screens the game
// unloads as Player 1 moves on kill theirs between updates, far from everyone, and those come back whole: counted as
// destroyed, at the zoom's wide views the tall grass around was gone, bare earth in its place (2026-10-28).
// Only those are kept (GONE_SLOTS; the one destroyed longest ago makes way when they're all taken): a slot for every
// one seen filled up in play - thousands walking the Field - and from then on nothing was kept, so cut grass and a
// thrown boulder came back again (user 2026-10-28, File 3, four players: 2 kept in a session).
#define GONE_SLOTS 4096
#define GONE_PROBES 8
#define GONE_NEAR 10.0f
#define GONE_TAKER 4.0f
#define ACTOR_DEAD (1u << 30)  // (actor +0x40: killed - runtime/hle/apple_trees.c)
static struct { uint64_t key, at_frame; float at[3]; } g_gone[GONE_SLOTS];
static bool g_gone_dead_before;  // (the actor updating was dead as its update began)
static bool g_gone_used;         // (one kept since it was cleared)
static uint64_t g_level_changed; // (the frame of the last level change: gone_note)
static void gone_forget(void) {
    if (g_gone_used) memset(g_gone, 0, sizeof g_gone);
    g_gone_used = false;
    actor_class_forget();
}
RT_ON_RESET(coop_gone_reset) { gone_forget(), g_level_changed = 0; }

// A grass's or rock's key: its level object's id, the hex after its kind in its name ("ObjRock-61025C005BA1C308") -
// the actor factory's spawn info has it at +0 (coop_spawn_skip) - or 0: not one of them.
static uint64_t gone_key(const char *name) {
    static const char *const kinds[] = {"ObjRock-", "ObjGrass-", "ObjGrass2x2-", "ObjLawn-", "ObjSkullStone-"};
    size_t at = 0;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds && !at && !strncmp(name, "Obj", 3); i++)
        if (!strncmp(name, kinds[i], strlen(kinds[i]))) at = strlen(kinds[i]);
    if (!at) return 0;
    uint64_t key = 0;
    for (int k = 0; k < 16; k++) {
        char ch = name[at + k];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        if (d < 0) break;
        key = key << 4 | (uint64_t)d;
    }
    return key ? key : 1;
}

// (a key's first slot: a multiplicative hash's top bits - the ids of one room file's objects share their low 32 bits)
static int gone_slot(uint64_t key, int p) { return (int)(((key * 0x9E3779B97F4A7C15ull) >> 52) + p) % GONE_SLOTS; }

static int gone_find(uint64_t key) {  // its slot, or -1: not destroyed
    for (int p = 0; p < GONE_PROBES; p++)
        if (g_gone[gone_slot(key, p)].key == key) return gone_slot(key, p);
    return -1;
}

static void gone_keep(uint64_t key, const float at[3]) {
    if (gone_find(key) >= 0) return;
    int k = gone_slot(key, 0);
    for (int p = 0; p < GONE_PROBES; p++) {
        int s = gone_slot(key, p);
        if (!g_gone[s].key) { k = s; break; }
        if (g_gone[s].at_frame < g_gone[k].at_frame) k = s;
    }
    g_gone[k].key = key, g_gone[k].at_frame = renderer_present_count();
    memcpy(g_gone[k].at, at, sizeof g_gone[k].at);
    g_gone_used = true;
}

static bool gone_near(const float at[3], float r) {  // a player within r of it
    float p[3];
    for (int i = 0; i < g_n; i++)
        if (player_pos(coop_player(i), p) && (p[0] - at[0]) * (p[0] - at[0]) + (p[2] - at[2]) * (p[2] - at[2]) < r * r)
            return true;
    return false;
}

static bool gone_watched(const float at[3]) {  // a player near it, or in view
    float nx, ny;
    bool front;
    if (gone_near(at, GONE_NEAR)) return true;
    extern float widescreen_factor(void);
    return view_point(view_camera(), at, 16.0f / 9.0f * widescreen_factor(), &nx, &ny, &front) && front && fabsf(nx) < 1.15f &&
           fabsf(ny) < 1.15f;
}

// The actor factory's spawn (runtime/hle/souls.c rt_actor_create_fn: its spawn info's key +0, position +0x180): one
// of these kept gone, made again where it's seen, isn't made at all - the factory's own "no such actor" path. Killed
// on its first update instead (gone_check, still there for one made another way), it had been made, loaded and drawn
// a frame: a screen's broken rocks flashed back with a hitch each time Player 1 stepped into it again, standing on its
// edge - three times in a few seconds (user 2026-10-30).
bool coop_spawn_skip(uint64_t info) {
    if (!info || !g_gone_used || !LD64(info) || !(rt_coop_on() || g_back_on) || !on_field()) return false;
    int k = gone_find(LD64(info));  // (key 0: made by the game as it goes, not a level object - and an empty slot's)
    if (k < 0) return false;
    const float *at = (const float *)(uintptr_t)(info + 0x180);
    if (!gone_watched(at)) {
        CLOG("[coop] %016llX back where nobody sees it (%.2f %.2f %.2f)\n", (unsigned long long)LD64(info), at[0], at[1],
             at[2]);
        g_gone[k].key = 0;
        return false;
    }
    static uint64_t logged_frame;
    uint64_t frame = renderer_present_count();
    if (frame != logged_frame)  // (one line a batch)
        CLOG("[coop] %016llX not made again, in view at (%.2f %.2f %.2f): kept gone (and the rest of its batch)\n",
             (unsigned long long)LD64(info), at[0], at[1], at[2]);
    logged_frame = frame;
    return true;
}

// Before an actor's update: true when it was one of these made again where it's seen, now killed. (Looked at once,
// on its first update.)
static bool gone_check(Cpu *c, uint64_t actor) {
    g_gone_dead_before = (LD32(actor + 0x40) & ACTOR_DEAD) != 0;
    if ((LD32(actor + 0x18) & 0xffffff) != ('O' | 'b' << 8 | 'j' << 16)) return false;
    ActorClass *e = actor_class(actor);
    if (e->gone == GONE_NOTED && !g_gone_dead_before) e->gone = GONE_NEW;  // (alive: made again at the same address)
    if (!e->key || e->gone != GONE_NEW) return false;
    e->gone = GONE_LIVE;
    int k = gone_find(e->key);
    if (k < 0 || g_gone_dead_before) return false;
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    const char *lv = chaos_current_level();
    float at[3];
    if (!actor_pos(actor, at)) memcpy(at, g_gone[k].at, sizeof at);
    if (!lv || strcmp(lv, "Field") || !gone_watched(at)) {
        float p1[3] = {0};
        player_pos(host_player(), p1);
        CLOG("[coop] %.40s back where nobody sees it (%.2f %.2f %.2f; Player 1 at %.2f %.2f %.2f)\n",
             (const char *)(uintptr_t)(actor + 0x18), at[0], at[1], at[2], p1[0], p1[1], p1[2]);
        g_gone[k].key = 0;
        return false;
    }
    uint64_t saved[31], sp = c->sp, pc = c->pc;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = actor;
    guest_call(c, GA(ACTOR_KILL));
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
    e->gone = GONE_NOTED;
    CLOG("[coop] %.40s made again in view at (%.2f %.2f %.2f): kept gone\n", (const char *)(uintptr_t)(actor + 0x18),
         at[0], at[1], at[2]);
    return true;  // (still gone: its next one is checked the same way)
}

// The screens kept around Player 1 (main+0x85c280's box, 90 x 72, grown by g_box: COOP_KEEP_BOX_SITE) lose their
// objects once outside it, and come back fresh - their cut grass and broken rocks whole - when the box takes them in
// again. Alone that is a screen well behind Link. With co-op it was centred on Player 1 only: what another player cut
// came back by him once Player 1 had walked off and back. So with co-op on the Field it reaches every other player and
// the screen around him (keep_reach), each way, up to the largest box the zoom makes (BOX_MAX): the screens around any
// player keep what was done in them, as Link's do alone.
#define KEEP_W 90.0f
#define KEEP_D 72.0f
static void keep_reach(float *rx, float *rz) {  // (how far the kept box must reach from Player 1, across and deep; 0: no further)
    float p1[3], p[3];
    *rx = *rz = 0;
    if (!rt_coop_on() || !on_field() || !player_pos(host_player(), p1) || (fabsf(p1[0]) < 1 && fabsf(p1[2]) < 1)) return;
    for (int i = 1; i < g_n; i++) {
        if (!player_pos(coop_player(i), p) || p[1] < -500.0f) continue;  // (taken off the map)
        float dx = fabsf(p[0] - p1[0]) + SCREEN_W, dz = fabsf(p[2] - p1[2]) + SCREEN_D;
        if (dx > KEEP_W * BOX_MAX / 2 || dz > KEEP_D * BOX_MAX / 2) continue;  // (far off: bring-back takes him back)
        *rx = fmaxf(*rx, dx), *rz = fmaxf(*rz, dz);
    }
}
static bool gone_kept_screen(const float at[3]) {
    float p1[3], rx, rz;
    if (!player_pos(host_player(), p1)) return false;
    keep_reach(&rx, &rz);
    float sx = SCREEN_W * floorf(at[0] / SCREEN_W), sz = SCREEN_D * floorf(at[2] / SCREEN_D);  // (its screen's corner)
    float hx = fmaxf(KEEP_W * g_box / 2, rx) - SCREEN_W, hz = fmaxf(KEEP_D * g_box / 2, rz) - SCREEN_D;
    return sx + SCREEN_W > p1[0] - hx && sx < p1[0] + hx && sz + SCREEN_D > p1[2] - hz && sz < p1[2] + hz;
}

// After an actor's update: whether it was destroyed. (Not as a level goes - the level change, coop_level_change, then
// LEVEL_GOING frames: everything in it dies, and a warp to another spot of the Field would leave it kept gone there.)
#define LEVEL_GOING 120
static void gone_note(uint64_t actor) {
    if (!(LD32(actor + 0x40) & ACTOR_DEAD) || (LD32(actor + 0x18) & 0xffffff) != ('O' | 'b' << 8 | 'j' << 16)) return;
    if (g_level_changed && renderer_present_count() < g_level_changed + LEVEL_GOING) return;
    ActorClass *e = actor_class(actor);
    if (!e->key || e->gone == GONE_NOTED) return;
    e->gone = GONE_NOTED;
    float at[3];
    if (!actor_pos(actor, at)) return;
    // (dead before its own update, nobody by it, its screen at the kept ones' edge: unloaded - it comes back whole)
    if (g_gone_dead_before && !gone_near(at, GONE_TAKER) && !(on_field() && gone_kept_screen(at))) return;
    gone_keep(e->key, at);
    CLOG("[coop] %.40s destroyed%s\n", (const char *)(uintptr_t)(actor + 0x18), g_gone_dead_before ? " (taken)" : "");
}

static void scale_s(Cpu *c, int r, float k) {  // (a float register's scalar, sN, times k)
    float f;
    memcpy(&f, &c->v[r], 4);
    f *= k;
    memcpy(&c->v[r], &f, 4);
}
static void set_s(Cpu *c, int r, float f) { memcpy(&c->v[r], &f, 4); }

// The active screens (COOP_ACTIVE_BOX_SITE): the game's box around Player 1 (s3 across and s4 deep its half sizes, s5
// its offset deep) grown to take in every other player and the screen around him - his ground solid, what's near him
// running - wherever the view lets him be (the zoom keeps it inside the screens made). Every screen made active
// instead held 30 fps at 2.5 (13 x 13 screens' objects running).
static void active_box(Cpu *c) {
    float hx, hz, oz, p1[3], p[3];
    memcpy(&hx, &c->v[3], 4), memcpy(&hz, &c->v[4], 4), memcpy(&oz, &c->v[5], 4);
    // Only in play, and only for a player inside the screens made: as a file loads the Links stand at the map's origin
    // or where they were in the level before, and taken in, the box woke a large part of the Field at once - the
    // game ran out of memory (user 2026-10-28: File 3 loaded from Angler's Tunnel, four players).
    if (!g_back_on || !player_pos(coop_player(0), p1) || (fabsf(p1[0]) < 1 && fabsf(p1[2]) < 1)) return;
    float z0 = oz - hz, z1 = oz + hz, far_x = LOAD_W * g_box / 2, far_z = LOAD_D * g_box / 2;  // (from Player 1)
    for (int i = 1; i < g_n; i++)
        if (player_pos(coop_player(i), p) && p[1] > -500.0f &&  // (not taken off the map)
            fabsf(p[0] - p1[0]) < far_x && fabsf(p[2] - p1[2]) < far_z) {
            hx = fmaxf(hx, fabsf(p[0] - p1[0]) + SCREEN_W);
            z0 = fminf(z0, p[2] - p1[2] - SCREEN_D), z1 = fmaxf(z1, p[2] - p1[2] + SCREEN_D);
        }
    set_s(c, 3, hx), set_s(c, 4, (z1 - z0) / 2), set_s(c, 5, (z0 + z1) / 2);
}

// The shadow map's light box (COOP_SHADOW_BOX_SITE): the game fits its one 2048 x 2048 shadow map to a world box around
// Player 1 (the view's +0x200 min, +0x210 max: about 34 x 16 x 33 on the Field, set again each frame). Zoomed out the
// view reached past it: the shadows at its edge ran on as dark diagonal stripes across the far ground (user 2026-10-29,
// past 2.3 in a wide window), and past it nothing cast one. Grown across and deep by the zoom, as much as the view
// grows, the map's texels cover as much of the screen as at 1. It costs nothing measurable: the game draws every
// caster into the map anyway (draws the same, GPU time within 0.2 ms either way at 2.0, 2.6 and 3.0).
// (what we left in it: a second fit in the same frame, the game not having set it since, doesn't grow it again)
static uint64_t g_shadow_view;
static float g_shadow_grown[8];
static void shadow_box(uint64_t view) {
    if (g_zoom <= 1.0f || !heap_ptr(view)) return;
    float *b = (float *)(uintptr_t)(view + 0x200);  // (min x y z, max x y z: each 4 floats)
    if (view == g_shadow_view && !memcmp(b, g_shadow_grown, sizeof g_shadow_grown)) return;
    for (int i = 0; i < 3; i += 2) {  // (across and deep: its height as it is)
        float mid = (b[i] + b[4 + i]) * 0.5f, half = (b[4 + i] - b[i]) * 0.5f * g_zoom;
        b[i] = mid - half, b[4 + i] = mid + half;
    }
    g_shadow_view = view;
    memcpy(g_shadow_grown, b, sizeof g_shadow_grown);
}

// LARECOMP_TRACK_RESPAWN=1 (diagnostic): a rock or grass actor made again under the same name - one that came back -
// logged as "[respawn]", with where it is and whether a player is near it or it's in view (gone_watched): one made
// again in view, destroyed or not, pops (cut grass and lifted rocks came back beside Player 1 in play, 2026-10-28).
// (the ones seen this level - gone_key's kinds, by its key - in a few probes; cleared with the gone list's level)
static struct { uint64_t key, actor; } g_respawn_seen[8192];
static void respawn_track(uint64_t actor) {
    if (!ENV("LARECOMP_TRACK_RESPAWN") || !heap_ptr(actor)) return;
    static uint64_t level;
    if (level != g_level_changed) memset(g_respawn_seen, 0, sizeof g_respawn_seen), level = g_level_changed;
    const char *nm = (const char *)(uintptr_t)(actor + 0x18);
    char name[64];
    if (strncmp(nm, "Obj", 3)) return;
    snprintf(name, sizeof name, "%.63s", nm);
    uint64_t key = gone_key(name);
    if (!key) return;
    int k = (int)(key % 8192);
    for (int p = 0; p < GONE_PROBES && g_respawn_seen[k].key && g_respawn_seen[k].key != key; p++) k = (k + 1) % 8192;
    if (g_respawn_seen[k].key == key && g_respawn_seen[k].actor != actor) {
        float at[3] = {0}, p1[3] = {0};
        actor_pos(actor, at);
        player_pos(host_player(), p1);
        LOG("[respawn] %.48s again (actor %#llx, was %#llx), frame %llu, at (%.2f %.2f %.2f)%s, Player 1 %.1f away\n",
            nm, (unsigned long long)actor, (unsigned long long)g_respawn_seen[k].actor,
            (unsigned long long)renderer_present_count(), at[0], at[1], at[2], gone_watched(at) ? " SEEN" : "",
            sqrtf((at[0] - p1[0]) * (at[0] - p1[0]) + (at[2] - p1[2]) * (at[2] - p1[2])));
    }
    g_respawn_seen[k].key = key, g_respawn_seen[k].actor = actor;
}
