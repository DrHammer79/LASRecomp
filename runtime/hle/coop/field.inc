// Local co-op, cut grass and thrown rocks kept gone, the screens kept and active around every player, the shadow map's box. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// ---- Cut grass and thrown rocks. On the Field the game makes a level object - grass, a rock - again when its screen
// comes back into the box of screens it keeps around Player 1 and its actor is gone: the screens come back fresh, as on
// the Game Boy. Alone, that is a screen well behind Link, out of view. With co-op the others cut and throw in screens
// Player 1 isn't in, and the zoomed-out view shows the screens around: grass and rocks came back beside the players
// (user's log 2026-10-28: a thrown rock back on the stairs in Player 1's way). Until 2026-11-13 each one destroyed was
// remembered by name and one made again in view refused at the actor factory or killed on its first update (the
// "spawn skip"); upstream's review asked for the cause. It is the kept box, centred on Player 1 only: below, with co-op
// it reaches every player (keep_reach), and with the zoom it grows with the view (g_box, COOP_KEEP_BOX_SITE), so no
// screen anyone can see is let go and made fresh. A screen is also made fresh when a camera of a room-changing kind is
// told of a new camera area (the screens outside it): on the Field only the other Links' cameras were, without the
// level's camera set-up - a cut bush grew back beside Player 1 as he stepped into another area (camera_setup_tick,
// runtime/hle/coop/events.inc). (gone_key, gone_near, gone_watched, ACTOR_DEAD: for the respawn log.)
#define GONE_PROBES 8
#define GONE_NEAR 10.0f
#define ACTOR_DEAD (1u << 30)  // (actor +0x40: killed - runtime/hle/apple_trees.c)
static uint64_t g_level_changed; // (the frame of the last level change: respawn_track)
RT_ON_RESET(coop_gone_reset) { g_level_changed = 0; }

// A grass's or rock's key: its level object's id, the hex after its kind in its name ("ObjRock-61025C005BA1C308"), or
// 0: not one of them.
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

// The shadow map's box (COOP_SHADOW_BOX_SITE): the game fits its one 2048 x 2048 shadow map to a world box around
// Player 1 (main+0x10540, each frame: his position plus a box kept relative to him, about 34 x 16 x 33 on the Field,
// stored at the view's +0x200 min, +0x210 max). Zoomed out the view reached past it: the shadows at its edge ran on as
// dark diagonal stripes across the far ground (user 2026-10-29, past 2.3 in a wide window), and past it nothing cast
// one. Grown across and deep by the zoom, as much as the view grows, the map covers what the view shows. It is grown
// as it is stored, before anything reads it. Grown later, only where the light's projection is fitted
// (main+0x100a460), the map no longer matched it: the casters sat off-centre in it, cut off on the light's side, and
// from 2x most of the ground lost its shadows (user 2026-11-14: "they start drawing wrongly then settle weird"; the
// map read back at 3x). It costs nothing measurable: the game draws every caster into the map anyway.
static void shadow_box(Cpu *c) {
    if (g_zoom <= 1.0f) return;
    float lo[4], hi[4];  // (q1: min x y z, q0: max)
    memcpy(lo, &c->v[1], sizeof lo), memcpy(hi, &c->v[0], sizeof hi);
    for (int i = 0; i < 3; i += 2) {  // (across and deep: its height as it is)
        float mid = (lo[i] + hi[i]) * 0.5f, half = (hi[i] - lo[i]) * 0.5f * g_zoom;
        lo[i] = mid - half, hi[i] = mid + half;
    }
    memcpy(&c->v[1], lo, sizeof lo), memcpy(&c->v[0], hi, sizeof hi);
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
