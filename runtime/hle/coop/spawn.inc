// Local co-op, the players' Links made (the player creator), placed, walking in through doors, passing through each other. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// The player creator (an event action, main+0x83a470): the game makes its Link (the manager's +0x4b8). With co-op
// it is run once for each other player first - their Links stay from then on, as the mod's - then for Player 1, so
// the manager ends up holding his, as without co-op. (The Links it made: g_made, above.)

// The creator, once for each other player, then for Player 1 - last: the scene's camera is the last Link's (made first,
// the camera wasn't his), and it leaves the game manager's player (+0x4b8) on him. (Each Link's update writes the
// save's restart point into the player entity's object - only Player 1's does: COOP_RESTART_SITE.)
static int coop_create(Cpu *c) {
    if (g_creating >= 0) return 0;  // (our own call: the creator as it is)
    uint64_t args[8];
    memcpy(args, c->x, sizeof args);
    for (int i = g_n - 1; i >= 0; i--) {
        if (i > 0 && g_extra[i] && LD64(g_extra[i]) == GA(PLAYER_VTABLE)) continue;
        if (i > 0 && (g_extra[i] = made_take_back())) {
            LOG("[coop] player %d: Link %#llx (taken back)\n", i + 1, (unsigned long long)g_extra[i]);
            continue;
        }
        g_creating = i;
        memcpy(c->x, args, sizeof args);
        guest_call(c, GA(COOP_CREATE_SITE));
        if (i > 0) {
            g_extra[i] = host_player();
            made_add(g_extra[i]);
            LOG("[coop] player %d: Link %#llx\n", i + 1, (unsigned long long)g_extra[i]);
        }
    }
    g_creating = -1;
    CLOG("[coop] creator: player 1 Link %#llx\n", (unsigned long long)host_player());

    return 2;  // (to the creator's ret)
}

// Placement at a spawn point (main+0x85d5f0, x0 = the spot): each player in turn, Player 1 last (as a single call).
static int coop_place(Cpu *c) {
    if (g_placing >= 0) return 0;
    uint64_t args[8];
    memcpy(args, c->x, sizeof args);
    for (int i = g_n - 1; i >= 0; i--) {
        if (i > 0 && !collision_of(g_extra[i])) continue;
        g_placing = i;
        memcpy(c->x, args, sizeof args);
        guest_call(c, GA(COOP_PLACE_SITE));
    }
    g_placing = -1;
    for (int i = 1; i < g_n; i++) pass_from_here(i);
    lost_forget();  // (all on the spawn: nobody lost, where they stood from now)
    g_placed_frame = renderer_present_count();
    CLOG("[coop] placed at spawn (frame %llu)\n", (unsigned long long)g_placed_frame);
    for (int i = 1; i < g_n; i++)  // (a new area: the knocked-out are back)
        if (g_down[i]) revive(i, "new area");
    return 2;
}

// The door walk-in (main+0xdd4950: x0 its state, x1 Link).
static int coop_enter(Cpu *c) {
    if (g_entering) return 0;
    static uint64_t last_state, last_frame;
    if (c->x[0] != last_state || renderer_present_count() > last_frame + 60)
        CLOG("[coop] walk-in state %#llx Link %#llx (player %d) +0x160=%u\n", (unsigned long long)c->x[0],
             (unsigned long long)c->x[1], coop_index(c->x[1]) + 1, LD8(c->x[0] + 0x160));
    last_state = c->x[0], last_frame = renderer_present_count();
    if (coop_index(c->x[1])) return 0;  // (another player's own walk-in, if the game gives him one)
    g_entering = 1;
    guest_call(c, GA(COOP_ENTER_SITE));
    g_entering = 0;
    uint64_t r0 = c->x[0];
    // (A talk walks Player 1 up to the one he talks to with this walk-in too: then nobody is moved - g_talking)
    for (int i = 1; i < g_n && !g_talking; i++) {
        if (!collision_of(g_extra[i])) continue;
        ST8(g_extra[i] + 0x41, 1);  // (as the mod)
        ride_on_host(i);
    }
    c->x[0] = r0;
    return 2;
}

// The Link (player index) whose collision capsule is centred at (x, y, z) - a capsule's centre is 0.72 above his feet
// (measured 2026-10-27) - or -1. Positions within 0.2 across: a capsule there is his (another character's centre
// can't be that close to his without the two overlapping).
static bool capsule_of(int i, double x, double y, double z) {
    float p[3];
    if (!player_pos(coop_player(i), p)) return false;
    double dx = x - p[0], dz = z - p[2], dy = y - (p[1] + 0.72);
    return dx * dx + dz * dz < 0.04 && dy > -0.5 && dy < 0.5;
}

static int link_at(double x, double y, double z) {
    for (int i = 0; i < g_n; i++)
        if (capsule_of(i, x, y, z)) return i;
    return -1;
}

static bool passing_at(double x, double y, double z) {  // (any Link there passing through: g_pass)
    for (int i = 1; i < g_n; i++)
        if (g_pass[i] && capsule_of(i, x, y, z)) return true;
    return false;
}

// A player put on Player 1 stops passing through the others once he has walked off the spot (see g_pass).
static void pass_tick(void) {
    for (int i = 1; i < g_n; i++) {
        float p[3], q[3];
        if (!g_pass[i] || !player_pos(g_extra[i], p)) continue;
        float dx = p[0] - g_pass_at[i][0], dy = p[1] - g_pass_at[i][1], dz = p[2] - g_pass_at[i][2];
        if (dx * dx + dy * dy + dz * dz < PASS_OFF * PASS_OFF) continue;
        bool clear = true;
        for (int j = 0; j < g_n && clear; j++)
            if (j != i && player_pos(coop_player(j), q))
                clear = (p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2]) >= PASS_CLEAR * PASS_CLEAR ||
                        fabsf(p[1] - q[1]) > 1.5f;
        if (!clear) continue;
        g_pass[i] = false;
        if (g_settings.coop_collision) CLOG("[coop] player %d walked off: the others block him again\n", i + 1);
    }
}

// PhysX character-controller sweep (COOP_CCT_SWEEP_SITE_*): x28 a touched geometry, x19 the swept volume (its
// centre: doubles at +8). The game gives each character controller the others as user capsule obstacles
// (TouchedGeomType eUSER_CAPSULE = 1: no owner, just the capsule - endpoints as doubles at +0x2c / +0x44, radius
// +0x5c). With player collision off, a Link's sweep skips the capsule of another Link: they walk through each
// other; everything else still blocks them. (Any Link's: stacked Links share one spot, so which one a capsule is
// can't be told - and a Link's own capsule isn't among his obstacles.) With it on, it skips one where a player is
// passing through (g_pass), and every capsule in the sweep of a player passing through - and every one in a
// side-view room: there a Link jumping onto another stood on his head and stuck a second or two (user 2026-10-28).
static bool cct_sweep(Cpu *c) {
    uint64_t g = c->x[28];
    if (LD32(g) != 1) return false;
    double vol[3], p0[3], p1[3], cap[3];
    memcpy(vol, (const void *)(uintptr_t)(c->x[19] + 8), 24);
    memcpy(p0, (const void *)(uintptr_t)(g + 0x2c), 24);
    memcpy(p1, (const void *)(uintptr_t)(g + 0x44), 24);
    for (int k = 0; k < 3; k++) cap[k] = (p0[k] + p1[k]) / 2;
    if (link_at(vol[0], vol[1], vol[2]) < 0 || link_at(cap[0], cap[1], cap[2]) < 0) return false;
    extern bool game_side_view(void);  // runtime/hle/widescreen.c
    return !g_settings.coop_collision || game_side_view() || passing_at(vol[0], vol[1], vol[2]) ||
           passing_at(cap[0], cap[1], cap[2]);
}
