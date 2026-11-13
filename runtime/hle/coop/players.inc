// Local co-op, players left behind (bring-back), lost below the world, Links left over, the pause menu's hold, the HUD's questions. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// A player out of the camera's view (zoomed out as far as it goes) for LEASH_FRAMES is put back on Player 1 (setting
// coop_leash): left behind in another room, run off the screen - once he is on his feet (his state kind 0: not mid-
// fall, knocked back..., whose own ending would put him back where it began), or after LEASH_ANYWAY whatever he does.
// A knocked-out one waits LEASH_DOWN - his bubble marks where he is, for a revive - then is revived and put back.
// Not while Player 1 is in an event.
#define LEASH_FRAMES 180   // (3 s)
#define LEASH_ANYWAY 600   // (10 s)
#define LEASH_DOWN 1800    // (30 s)
static void leash_tick(void) {
    static uint64_t away[COOP_MAX];
    extern float widescreen_factor(void);
    uint64_t now = renderer_present_count();
    float aspect = 16.0f / 9.0f * widescreen_factor();
    static uint64_t noted[COOP_MAX];
    for (int i = 1; i < g_n; i++) {
        float p[3], h[3], nx = 0, ny = 0;
        bool front = false, have = player_pos(g_extra[i], p);
        const char *held = !g_settings.coop_leash                      ? "the option is off"
                           : g_p1_event                                ? "Player 1 is in an event"
                           : !have                                     ? "no position"
                           : !view_point(view_camera(), p, aspect, &nx, &ny, &front) ? "no camera"
                           : front && fabsf(nx) < 1.05f && fabsf(ny) < 1.05f ? "in view"
                                                                       : NULL;
        if (held) {
            away[i] = 0;
            // (far from Player 1 yet held - once in play the bring-back stopped, unexplained: the reason, every 10 s)
            if (have && player_pos(host_player(), h) && now >= noted[i] &&
                (p[0] - h[0]) * (p[0] - h[0]) + (p[2] - h[2]) * (p[2] - h[2]) > 30.0f * 30.0f) {
                uint64_t me = host_player();
                LOG("[coop] player %d is %.0f from Player 1, not brought back: %s (view %.2f %.2f%s, his state %d, "
                    "Player 1's %d, camera %#llx%s)\n", i + 1,
                    sqrtf((p[0] - h[0]) * (p[0] - h[0]) + (p[2] - h[2]) * (p[2] - h[2])), held, nx, ny,
                    front ? "" : " behind", link_state_kind(g_extra[i]), link_state_kind(me),
                    (unsigned long long)view_camera(), me && view_camera() == LD64(me + 0x318) ? ", his own" : "");
                noted[i] = now + 600;
            }
            continue;
        }
        if (!away[i]) away[i] = now;
        uint64_t gone = now - away[i];
        if (g_down[i] ? gone > LEASH_DOWN
                      : gone > LEASH_ANYWAY || (gone > LEASH_FRAMES && link_state_kind(g_extra[i]) == 0)) {
            CLOG("[coop] player %d out of view %llu s%s: back on Player 1\n", i + 1, (unsigned long long)(gone / 60),
                 g_down[i] ? ", knocked out" : "");
            if (g_down[i]) revive(i, "left behind");
            ride_on_host(i);
            away[i] = 0;
        }
    }
}

// Players lost below the world. On the Field only the area Player 1 is in has its ground: another player left in the
// next one falls through it, and the game puts him back on the spot he fell from, where he falls again - for good
// (one screen away; at Out 50% still in view, so leash_tick never took him). His position then is the game's to set:
// put on Player 1 while he falls, he's back on that spot a frame later. A player whose height drops LOST_DROP below
// where he last stood (state kind 0; a ledge is a few units) is lost and taken off the map, LOST_AWAY from then put
// on Player 1, his safe spot with him (user 2026-10-28: gone for a moment, then back with Player 1, rather than seen
// falling and popping up). While he falls he's below the ground; as the game stands him up again where he fell from,
// he's parked there out of sight (y -1000, his update not running: standing, as a teleport needs), until it's time.
// Whether or not the camera saw him, and with "bring back" off too. Only on the Field: elsewhere the ground is all
// there, and a player swimming down or climbing a long ladder in a side-view room would be "lost".
#define LOST_DROP 8.0f
#define LOST_AWAY 150  // (2.5 s)
static float g_lost_stood[COOP_MAX];  // (where he last stood: height)
static bool g_lost_known[COOP_MAX];
static void lost_forget(void) {  // (a new file, a spawn: nobody lost, nowhere stood yet)
    memset(g_lost_until, 0, sizeof g_lost_until), memset(g_lost_parked, 0, sizeof g_lost_parked);
    memset(g_lost_known, 0, sizeof g_lost_known);
}
static void lost_tick(void) {
    float *stood = g_lost_stood;
    bool *known = g_lost_known;
    bool field = on_field();  // (a knocked-out player put on Player 1 - down_move_tick - is let go anywhere)
    uint64_t now = renderer_present_count();
    for (int i = 1; i < g_n; i++) {
        float p[3];
        uint64_t q = collision_of(g_extra[i]);
        int kind = link_state_kind(g_extra[i]);
        if (g_lost_until[i]) {
            if (!g_lost_parked[i] && kind == 0) g_lost_parked[i] = true;  // (stood up where he fell from)
            if (g_lost_parked[i] && now < g_lost_until[i]) {
                if (q) ST32(q + 0x84, 0xc47a0000u);  // (y -1000: out of sight)
                continue;
            }
            if (!g_lost_parked[i] && now < g_lost_until[i] + 600) continue;  // (still falling: 10 s at most)
            g_lost_until[i] = 0, g_lost_parked[i] = false;
            ride_on_host(i);
            LOG("[coop] player %d back on Player 1 (%s)\n", i + 1,
                g_down[i] ? "knocked out, away from what hit him" : "fell through the world");
            if (player_pos(g_extra[i], p)) stood[i] = p[1];
            continue;
        }
        if (!q || !player_pos(g_extra[i], p) || kind < 0) { known[i] = false; continue; }
        if (kind == 0) {
            stood[i] = p[1];
            known[i] = true;
        } else if (field && known[i] && p[1] < stood[i] - LOST_DROP) {
            g_lost_until[i] = now + LOST_AWAY, g_lost_parked[i] = false;
            LOG("[coop] player %d fell through the world (from %.1f to %.1f): off the map for %.1f s\n", i + 1,
                stood[i], p[1], LOST_AWAY / 60.0f);
        }
    }
}
static bool lost_away(uint64_t actor) { int i = coop_index(actor); return i && g_lost_until[i] && g_lost_parked[i]; }
RT_ON_RESET(coop_lost_reset) { lost_forget(); }

// The Links made for the other players (coop_create) outlive a game over and a return to the title (the game keeps
// them, as Player 1's): play_ended forgets which player is which, and the next file's players take these back - any
// seen updating in the last MADE_ALIVE frames - rather than getting new ones beside them (left over, they copied Player
// 1). One that isn't taken back (fewer players now - co-op turned off too) is parked: out of sight below the level, its
// update skipped (rt_coop_hook; g_leftover).
#define MADE_MAX 32
#define MADE_ALIVE 120
static struct { uint64_t actor, seen; } g_made[MADE_MAX];
static bool g_leftover;  // (one of them lives that no player holds: the actors' updates are looked at, co-op off too)
RT_ON_RESET(coop_made_reset) { memset(g_made, 0, sizeof g_made), g_leftover = false; }

static int made_index(uint64_t actor) {
    for (int k = 0; k < MADE_MAX; k++)
        if (actor && g_made[k].actor == actor) return k;
    return -1;
}

static void made_add(uint64_t actor) {
    int k = made_index(actor), old = 0;
    for (int j = 0; k < 0 && j < MADE_MAX; j++)
        if (!g_made[j].actor) k = j;
        else if (g_made[j].seen < g_made[old].seen) old = j;
    k = k < 0 ? old : k;
    g_made[k].actor = actor, g_made[k].seen = renderer_present_count();
}

// A live Link made earlier (seen updating in the last MADE_ALIVE frames) that no player holds now, from slot k on;
// -1: none.
static int made_spare(int k) {
    uint64_t now = renderer_present_count();
    for (; k < MADE_MAX; k++) {
        uint64_t a = g_made[k].actor;
        if (!a || coop_index(a) || a == host_player() || now - g_made[k].seen > MADE_ALIVE) continue;
        if (LD64(a) == GA(PLAYER_VTABLE)) return k;
    }
    return -1;
}
static uint64_t made_take_back(void) { int k = made_spare(0); return k < 0 ? 0 : g_made[k].actor; }

// A knocked-out player hit again and again (down_hit) goes onto Player 1's spot as one fallen through the world does
// (lost_tick): standing still (his state kind 0), he's taken off the map, his update not running, then put on Player
// 1. Moved while knocked back or falling - his collision's position, or every copy a standing Link keeps - his state
// put him back in the spikes; and with no knockback (dead_state) he didn't stop falling on them: so his state is
// changed to the first of kind 0 first, through the game's own change (COOP_DEAD_STATE_SITE's function: x0 his state
// machine, w1 the state).
#define DOWN_AWAY 30  // (half a second)
static void down_move_tick(Cpu *c) {
    for (int i = 1; i < g_n; i++) {
        if (!g_down_move[i] || g_lost_until[i]) continue;  // (the spikes go on hitting him a moment while he's away)
        g_down_move[i] = false;
        uint64_t link = g_extra[i];
        float p[3], h[3];  // (on Player 1's spot already: nothing to do - moved, he was gone half a second)
        if (player_pos(link, p) && player_pos(host_player(), h) && fabsf(p[0] - h[0]) < 1.5f &&
            fabsf(p[1] - h[1]) < 1.5f && fabsf(p[2] - h[2]) < 1.5f)
            continue;
        for (unsigned s = 0; s < 0x40 && GA(COOP_DEAD_STATE_SITE) && link_state_kind(link) != 0; s++) {
            uint64_t st = LD64(link + 0x1340 + 8ull * s);
            if (!heap_ptr(st) || LD8(st + 0x28) != 0) continue;
            Cpu saved = *c;
            c->x[0] = link + 0x4e0, c->x[1] = s;
            guest_call(c, GA(COOP_DEAD_STATE_SITE));
            *c = saved;
            break;
        }
        LOG("[coop] player %d hit again while knocked out (his state kind %d): put on Player 1\n", i + 1,
            link_state_kind(link));
        g_lost_until[i] = renderer_present_count() + DOWN_AWAY, g_lost_parked[i] = false;
        g_down_moved_until[i] = g_lost_until[i] + 120;  // (and 2 s once he's there)
    }
}

// (neither while Player 1 is in an event - nobody else can act then: they showed over the warp holes' "Where to?" map,
// 2026-10-28)
bool coop_player_screen(int i, float w, float h, float *sx, float *sy, bool *edge) {
    return i >= 1 && !g_p1_event && player_screen(i, 1.0f, w, h, sx, sy, edge);
}

// Where to show knocked-out player i's "A Revive" prompt (below him), when a player who is up is beside him.
bool coop_revive_prompt(int i, float w, float h, float *sx, float *sy) {
    bool edge;
    return i > 0 && i < COOP_MAX && g_can_revive[i] && g_down[i] && !g_p1_event &&
           player_screen(i, -0.3f, w, h, sx, sy, &edge) && !edge;
}

// Player i (0: Player 1) holds A beside a knocked-out player: his stick and d-pad don't move him.
int coop_reviving(int i) { return own_hearts() && i >= 0 && i < COOP_MAX && g_reviving[i] != 0; }

// The HUD (runtime/ui/coop_hud.mm): each player's hearts, maximum, knocked out / revive progress and colour.
typedef struct { int hp, max, down; float revive; uint32_t rgb; } CoopHudRow;
uint32_t coop_colour_rgb(int player);
uint32_t coop_colour_rgb_of(int colour);

int coop_hud_rows(CoopHudRow *rows, int cap) {
    extern int game_in_play(void);
    if (g_n < 2 || !g_max_hp || !game_in_play()) return 0;  // (UI thread: the count once the game has set it)
    int n = g_n < cap ? g_n : cap;
    for (int i = 0; i < n; i++) {
        int h = i ? g_hp[i] : game_hearts();
        rows[i] = (CoopHudRow){h < 0 ? g_max_hp : h, g_max_hp, i ? g_down[i] : 0, i ? g_revive[i] : 0,
                               coop_colour_rgb(i)};
    }
    return n;
}

// The player who owns a colour set and its colour as drawn (hearts, the HUD): the co-op Playerlink.bfres's sets. Red
// is a deeper crimson than the game's own heart red, so a red player's hearts don't read as "the" hearts.
uint32_t coop_colour_rgb_of(int colour) {  // (a colour set's, by its number)
    static const uint32_t k[COOP_COLOURS] = {0x2fa043, 0x8b1a2b, 0x2c49d6, 0xe8c11c, 0x5b2fbe, 0xd02fc4, 0x3a3a3a,
                                             0xdcdcdc};
    return k[colour >= 0 && colour < COOP_COLOURS ? colour : 0];
}
uint32_t coop_colour_rgb(int player) {
    return coop_colour_rgb_of(player >= 0 && player < COOP_MAX ? g_settings.coop_colour[player] : 0);
}

// Player 1's own (the game's) hearts in his colour (runtime/nvn/texpatch.c): true, rgb set.
bool coop_heart_rgb(uint32_t *rgb) {
    // (render thread: the count once the game has set it; in play only - the file select's hearts stay the game's)
    // (with shared hearts they are everyone's: the Cosmetics tab's colour)
    extern int game_in_play(void);
    if (g_n < 2 || !g_settings.coop_heart_colours || g_settings.coop_health != COOP_HEALTH_OWN || !game_in_play())
        return false;
    *rgb = coop_colour_rgb(0);
    return true;
}
