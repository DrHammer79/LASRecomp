// Local co-op: 2-8 Links on one screen, Player 1 on the keyboard / first controller and each other player on a
// controller of his own (setting coop_players; runtime/hle/input.c gives npad 2..8 their pads).
//
// A port of the community Link's Awakening co-op mod (an exlaunch "pxtch" module for 1.0.1, its subsdk9 patch table
// read 2026-10-27) onto the hook sites of tools/patches.py (COOP_*): the game makes one Link per player through its
// own player creator, places all of them on every spawn point, gives each the controller of his npad and a tunic
// colour set of his own (the mod's Playerlink.bfres, served from <data root>/coop/romfs: coop_romfs_override). Unlike
// the mod: the players' indices live here, not past the end of the Link object (the mod grew the allocation by 4 and
// wrote 8 bytes there); a door's walk-in runs for Player 1 only and the others ride on him (the mod ran the one shared
// walk-in once per player, which left the last player behind in the doorway - and out of the room again).
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../game_addresses.h"
#include "../hle.h"
#include "../settings.h"

#define COOP_MAX 8

static int g_n = -1;                // players (rt_coop_players; read again out of play: coop_tick)
static uint64_t g_extra[COOP_MAX];  // players 2..8: their Link actors ([0] unused: Player 1 is the game's own)
static int g_creating = -1;         // the player rt_coop_create is making a Link for (-1: none)
static int g_placing = -1;          // the player rt_coop_place is placing (-1: none)
static int g_entering;              // inside rt_coop_enter's own call of the walk-in
static uint64_t g_placed_frame;     // when the players were last put on a spawn point (renderer frame)

RT_ON_RESET(coop_reset) {
    g_n = -1;
    memset(g_extra, 0, sizeof g_extra);
    g_creating = g_placing = -1;
    g_entering = 0;
    g_placed_frame = 0;
}

extern uint64_t renderer_present_count(void);

#define CLOG(...) do { if (ENV("LARECOMP_COOP_LOG")) LOG(__VA_ARGS__); } while (0)

int rt_coop_players(void) {
    if (g_n < 0) {
        int n = g_settings.coop_players;
        const char *e = getenv("LARECOMP_COOP");  // (tests: the player count without touching the settings)
        if (e) n = atoi(e);
        if (!GA(COOP_CREATE_SITE)) n = 1;  // (1.0.1 only)
        g_n = n < 1 ? 1 : n > COOP_MAX ? COOP_MAX : n;
        if (g_n > 1) LOG("[coop] %d players\n", g_n);
    }
    return g_n;
}

int rt_coop_on(void) { return rt_coop_players() > 1; }

// The player count from the setting (and LARECOMP_COOP), also before the game has booted: input.c opens the
// controllers. 1 once the game is known to be a version without co-op (its tables have no COOP_* sites).
int coop_players_setting(void) {
    if (g_ga && !GA(COOP_CREATE_SITE)) return 1;
    const char *e = getenv("LARECOMP_COOP");
    int n = e ? atoi(e) : g_settings.coop_players;
    unsigned long at;  // LARECOMP_COOP_AT=<frame>:<n> (test): the setting changed to n at that frame, as from the menu
    int later;
    if ((e = getenv("LARECOMP_COOP_AT")) && sscanf(e, "%lu:%d", &at, &later) == 2 && renderer_present_count() >= at)
        n = later;
    return n < 1 ? 1 : n > COOP_MAX ? COOP_MAX : n;
}

// Player 1's Link: the game manager's player (+0x4b8), as for the game itself - or, while an enemy's update has it
// pointing at the Link that enemy targets (actor_exec_begin), the one it holds back.
static bool g_swap_on;      // an enemy's update has the player slot (actor_exec_begin)
static uint64_t g_swapped;  // the slot's own Link meanwhile
static uint64_t host_player(void) {
    if (g_swap_on) return g_swapped;
    uint64_t gm = LD64(GA(GAME_MGR));
    return gm ? LD64(gm + 0x4b8) : 0;
}

uint64_t coop_player(int i) { return i == 0 ? host_player() : i < COOP_MAX ? g_extra[i] : 0; }

// The player a Link belongs to (0: Player 1, and anything that isn't one of the others).
int coop_index(uint64_t actor) {
    for (int i = 1; i < COOP_MAX; i++)
        if (actor && g_extra[i] == actor) return i;
    return 0;
}

// A Link's PlayerCollision component (actor +0x80; runtime/hle/doorjump.c): its position (+0x80) is Link's.
static uint64_t collision_of(uint64_t actor) {
    if (!actor || LD64(actor) != GA(PLAYER_VTABLE)) return 0;
    uint64_t p = LD64(actor + 0x80);
    return p && LD64(p) == GA(PLAYER_COLLISION_VT) ? p : 0;
}

static bool player_pos(uint64_t actor, float out[3]) {
    uint64_t p = collision_of(actor);
    if (!p) return false;
    memcpy(out, (const void *)(uintptr_t)(p + 0x80), 12);
    return true;
}

// Player i onto Player 1's spot (the mod's TeleportPlayerToAnother: the collision's position).
static void ride_on_host(int i) {
    float pos[3];
    uint64_t p = collision_of(g_extra[i]);
    if (!p || !player_pos(host_player(), pos)) return;
    memcpy((void *)(uintptr_t)(p + 0x80), pos, 12);
}

// ---- Hearts (setting coop_health). Shared: the game's one set, as without co-op. Own: Player 1 keeps the game's
// (PlayerStatus +0x84, quarter hearts; its HUD), players 2.. have their own here. A hit lands on the Link whose state
// code computes it (rt_coop_damage, from tools/patches.py DAMAGE_*_SITES): another player's comes off his hearts and
// not the game's. Hearts the game's set gains (a heart, a fairy, a potion, a container) every player gains too.
// Out of hearts, a player is knocked out (coop_knockout): he can't act and nothing hurts him until another player
// holds A beside him for coop_revive_s seconds - or the players reach another area - and he is back with 30% of the
// maximum, and nothing hurts him for REVIVE_SAFE frames (a chance to get away). Or, with "game over for everyone",
// his last hit is the game's own: a game over as Player 1's.
#define REVIVE_SAFE 120          // (2 s)
static int g_hp[COOP_MAX];       // players 2..: their hearts (quarter hearts; -1: full once the maximum is known)
static bool g_down[COOP_MAX];    // knocked out
static uint64_t g_safe_until[COOP_MAX];  // revived: unhurt until this frame
static float g_revive[COOP_MAX];  // a revive in progress (0..1)
static bool g_can_revive[COOP_MAX];  // players who are up beside a knocked-out one: holding A revives him
static int g_game_hp = -1;       // the game's hearts last frame (for what it gained)
static int g_max_hp;             // the maximum (main+0xdf8260, every half second and whenever the hearts change)
static int g_heal_pending;       // hearts healed this frame before the game's maximum capped them (COOP_HEAL_SITE_*)

static void hearts_forget(void) {  // (a new boot / back at the title: everyone starts the next file at full hearts)
    for (int i = 0; i < COOP_MAX; i++) g_hp[i] = -1, g_down[i] = false, g_revive[i] = 0, g_safe_until[i] = 0;
    g_game_hp = -1;
    g_max_hp = 0;
    g_heal_pending = 0;
}
RT_ON_RESET(coop_hearts_reset) { hearts_forget(); }

static bool own_hearts(void) { return rt_coop_on() && g_settings.coop_health == COOP_HEALTH_OWN; }

static int game_hearts(void) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return ps ? (int)(int16_t)*(const uint16_t *)(uintptr_t)(ps + 0x84) : -1;
}

static int revived_hearts(void) {  // 30% of the maximum, in whole quarters, at least one heart
    int q = (g_max_hp * 3 + 9) / 10;
    return q < 4 ? 4 : q;
}

static void knock_out(int i) {
    g_hp[i] = 0, g_down[i] = true, g_revive[i] = 0;
    LOG("[coop] player %d knocked out\n", i + 1);
}

static void revive(int i, const char *how) {
    g_down[i] = false, g_revive[i] = 0;
    g_hp[i] = revived_hearts();
    g_safe_until[i] = renderer_present_count() + REVIVE_SAFE;
    LOG("[coop] player %d revived (%s) with %d/%d\n", i + 1, how, g_hp[i], g_max_hp);
}

int coop_player_down(int i) { return own_hearts() && i > 0 && i < COOP_MAX && g_down[i]; }

// Player 1 in an event - walked by a cutscene or a door, an item held up, a scene - while the other players stand
// still (their input is none: runtime/hle/input.c). His current state (+0x4ed the slot, +0x1340 the states, +0x28 the
// kind) is one the game's own "can he go" check (main+0xd58d70) holds apart: kinds 18, 19, 22, 23, 24, 26.
static bool g_p1_event;
static bool heap_ptr(uint64_t p);
static int link_state_kind(uint64_t link) {  // (-1: not a Link)
    if (!link || LD64(link) != GA(PLAYER_VTABLE)) return -1;
    unsigned slot = LD8(link + 0x4ed);
    uint64_t st = slot < 0x40 ? LD64(link + 0x1340 + 8ull * slot) : 0;
    return heap_ptr(st) ? LD8(st + 0x28) : -1;
}

static void p1_event_tick(void) {
    int kind = link_state_kind(host_player());
    bool ev = kind >= 0 && kind <= 26 && ((1u << kind) & 0x4ec0000u);
    if (ev != g_p1_event) CLOG("[coop] Player 1 %s an event (state kind %d)\n", ev ? "in" : "out of", kind);
    g_p1_event = ev;
}

int coop_input_blocked(int i) { return coop_player_down(i) || (rt_coop_on() && i > 0 && g_p1_event); }

// The Link whose code is running: Link's state functions keep him in a callee-saved register (x19 / x20 at the damage
// sites); the others are checked too. 0: Player 1 (or no Link found).
static int running_player(Cpu *c) {
    for (int r = 19; r <= 28; r++)
        if (c->x[r] && c->x[r] == host_player()) return 0;
        else if (coop_index(c->x[r])) return coop_index(c->x[r]);
    for (int r = 0; r < 19; r++)
        if (coop_index(c->x[r])) return coop_index(c->x[r]);
    return 0;
}

// A hit of `dmg` quarter hearts (after the difficulty setting) about to come off the game's hearts.
int32_t rt_coop_damage(Cpu *c, int32_t dmg) {
    if (!own_hearts() || dmg <= 0) return dmg;
    int i = running_player(c);
    if (!i) return dmg;
    if (g_down[i] || g_settings.cheat_hearts) return 0;
    if (renderer_present_count() < g_safe_until[i]) {
        CLOG("[coop] player %d hit for %d: just revived, unhurt\n", i + 1, dmg);
        return 0;
    }
    if (g_hp[i] < 0) g_hp[i] = g_max_hp > 0 ? g_max_hp : 12;
    g_hp[i] -= dmg;
    CLOG("[coop] player %d hit for %d (%d left)\n", i + 1, dmg, g_hp[i]);
    if (g_hp[i] > 0) return 0;
    if (g_settings.coop_knockout == COOP_KO_GAME_OVER) {
        g_hp[i] = 0;
        LOG("[coop] player %d out of hearts: game over\n", i + 1);
        return 999;  // (the game's own hearts: its game over)
    }
    knock_out(i);
    return 0;
}

// An animation of Link's model by name (Playerlink.bfres: "sleep_lp"...), as an event's PlayAnimation does
// (main+0x119810: the actor's play-animation callback at +0xb0, called with the name {ptr, len}, the blend time, a
// mode and a start frame). Game thread; the name and its view go on the guest stack for the call.
// (what a pointer can be: a heap object, a vtable - main past its code -, code)
static bool heap_ptr(uint64_t p) { return p >= HEAP_BASE && p < HEAP_BASE + HEAP_MAX && !(p & 7); }
static bool vtable_ptr(uint64_t p) { return p >= MAIN_BASE + GA(MAIN_CODE_SIZE) && p < GA(MAIN_IMAGE_SCAN_END) && !(p & 7); }
static bool code_ptr(uint64_t p) { return p >= MAIN_BASE && p < MAIN_BASE + GA(MAIN_CODE_SIZE) && !(p & 3); }

// The object an event's PlayAnimation runs on for Link: his event component (the action dispatcher main+0x1049a0's
// actor - enemies hold theirs at +0x12e0): shared-from-this (+8 points at itself), its +0xb0 the play-animation
// callback (a std::function's callable: a vtable whose call slot +0x30 is code). Found among the pointers Link's
// actor holds (kept per Link while he lives: anim_forget).
static uint64_t g_anim_actor[COOP_MAX], g_anim_target[COOP_MAX];

static void anim_forget(void) {
    memset(g_anim_actor, 0, sizeof g_anim_actor);
    memset(g_anim_target, 0, sizeof g_anim_target);
}
RT_ON_RESET(coop_anim_reset) { anim_forget(); }

static bool event_component(uint64_t o) {
    if (!heap_ptr(o) || LD64(o + 8) != o) return false;
    uint64_t f = LD64(o + 0xb0), vt = heap_ptr(f) ? LD64(f) : 0;
    return vtable_ptr(vt) && code_ptr(LD64(vt + 0x30));
}

static uint64_t anim_target(uint64_t actor) {
    for (int i = 0; i < COOP_MAX; i++)
        if (g_anim_actor[i] == actor && event_component(g_anim_target[i])) return g_anim_target[i];
    uint64_t found = 0;
    for (uint64_t off = 0; off < 0x2000 && !found; off += 8) {
        uint64_t o = LD64(actor + off);
        if (o != actor && event_component(o)) found = o;
    }
    CLOG("[coop] Link %#llx: event component %#llx\n", (unsigned long long)actor, (unsigned long long)found);
    for (int i = 0; i < COOP_MAX; i++)
        if (!g_anim_actor[i] || g_anim_actor[i] == actor) { g_anim_actor[i] = actor, g_anim_target[i] = found; break; }
    return found;
}

static void play_anim(Cpu *c, uint64_t actor, const char *name, float blend) {
    uint64_t target = actor && GA(PLAY_ANIMATION) ? anim_target(actor) : 0;
    if (!target) { CLOG("[coop] no animation target for Link %#llx\n", (unsigned long long)actor); return; }
    actor = target;
    Cpu saved = *c;
    size_t n = strlen(name);
    c->sp = (c->sp - 0x80) & ~0xfull;
    uint64_t str = c->sp + 0x40, view = c->sp + 0x20;
    memcpy((void *)(uintptr_t)str, name, n + 1);
    ST64(view, str);
    ST64(view + 8, n);
    c->x[0] = LD64(actor + 0xb0);
    c->x[1] = view;
    c->x[2] = 0;  // (mode 0: start, as the action's first call)
    __uint128_t v0 = 0, v1 = 0;
    float zero = 0;
    memcpy(&v0, &blend, 4);
    memcpy(&v1, &zero, 4);
    c->v[0] = v0, c->v[1] = v1;
    guest_call(c, GA(PLAY_ANIMATION));
    *c = saved;
}

// Knocked-out players lie face down on the ground ("down_lp", Link knocked flat; the bed scenes' "sleep_lp" lies on his
// back where the bed would be, beside him) until they are revived, then stand up ("wait"). His own state code
// (standing still: "wait") would play its animation again over it, so after each of his updates (coop_actor_updated) a
// sleeper whose animation was changed gets his back, at once. The animation playing: Link's animation controller
// (+0x1568) layer 0 ([+0x78], 0x18 bytes a layer) +0x10, the animation's info.
static bool g_asleep[COOP_MAX];
static uint64_t g_sleep_anim[COOP_MAX];  // (that info while he lies there)
RT_ON_RESET(coop_sleep_reset) { memset(g_asleep, 0, sizeof g_asleep); memset(g_sleep_anim, 0, sizeof g_sleep_anim); }

#define SLEEP_ANIM "down_lp"  // (PlayerAnimation.gsheet)

static uint64_t anim_playing(uint64_t actor) {
    uint64_t ctrl = LD64(actor + 0x1568), layers = heap_ptr(ctrl) ? LD64(ctrl + 0x78) : 0;
    return heap_ptr(layers) ? LD64(layers + 0x10) : 0;
}

static void sleep_tick(Cpu *c) {
    for (int i = 1; i < g_n; i++) {
        bool want = g_down[i] && collision_of(g_extra[i]);
        if (want == g_asleep[i]) continue;
        // (the hit that knocked him out plays out first: its state - kind 11, knocked back - ends with its own
        // animation, so it would never end under the sleep pose, and he'd be stuck in it after a revive)
        if (want && link_state_kind(g_extra[i]) != 0) continue;
        play_anim(c, g_extra[i], want ? SLEEP_ANIM : "wait", want ? 0.2f : 0.3f);
        g_asleep[i] = want;
        g_sleep_anim[i] = want ? anim_playing(g_extra[i]) : 0;
        CLOG("[coop] player %d %s (animation %#llx, state kind %d)\n", i + 1, want ? "falls asleep" : "wakes up",
             (unsigned long long)g_sleep_anim[i], link_state_kind(g_extra[i]));
    }
    if (ENV("LARECOMP_COOP_LOG"))  // (his state for a while after: does he get going again)
        for (int i = 1; i < g_n; i++) {
            static int last[COOP_MAX];
            int k = link_state_kind(g_extra[i]);
            if (k != last[i]) CLOG("[coop] player %d state kind %d\n", i + 1, k);
            last[i] = k;
        }
}

// After an actor's update (COOP_ACTOR_EXEC_DONE): a sleeping player whose own code changed his animation sleeps on.
static void coop_actor_updated(Cpu *c, uint64_t actor) {
    int i = coop_index(actor);
    if (!i || !g_asleep[i] || !g_sleep_anim[i] || anim_playing(actor) == g_sleep_anim[i]) return;
    if (link_state_kind(actor) != 0) return;  // (hit again while down: that knockback plays out, then he lies again)
    play_anim(c, actor, SLEEP_ANIM, 0);
    static int logged;
    if (logged++ < 3) CLOG("[coop] player %d: animation changed while asleep - " SLEEP_ANIM " again\n", i + 1);
}

// Once per frame (coop_tick): the maximum, what the game's hearts gained, revives.
static void hearts_tick(Cpu *c) {
    if (!own_hearts()) {  // (shared hearts: nobody is knocked out - a switch from own hearts wakes them)
        for (int i = 0; i < COOP_MAX; i++) g_down[i] = false, g_revive[i] = 0;
        g_game_hp = -1, g_heal_pending = 0;
        return;
    }
    uint64_t frame = renderer_present_count();
    int h = game_hearts();
    static uint64_t next_max;
    if ((frame >= next_max || !g_max_hp || h != g_game_hp) && GA(PLAYER_MAX_HEALTH) && LD64(GA(PLAYER_STATUS_PTR))) {
        next_max = frame + 30;
        Cpu saved = *c;  // (the poll this runs in reads its own registers afterwards)
        c->x[0] = GA(PLAYER_STATUS_PTR);
        guest_call(c, GA(PLAYER_MAX_HEALTH));
        int m = (int)(int16_t)c->x[0];
        *c = saved;
        if (m > 0 && m != g_max_hp) CLOG("[coop] maximum hearts %d\n", m);
        if (m > g_max_hp && g_max_hp)  // (a heart container: everyone who is up gets the new heart)
            for (int i = 1; i < g_n; i++)
                if (!g_down[i] && g_hp[i] >= 0) g_hp[i] += m - g_max_hp;
        if (m > 0) g_max_hp = m;
    }
    if (!g_max_hp) return;
    static int test_down;  // LARECOMP_COOP_DOWN=<player>,... (test): those players knocked out 2 s after they're placed
    if (!test_down && g_placed_frame && renderer_present_count() > g_placed_frame + 120 && ENV("LARECOMP_COOP_DOWN"))
        for (const char *p = ENV("LARECOMP_COOP_DOWN"); (test_down = 1) && *p; p += *p == ',') {
            int i = (int)strtol(p, (char **)&p, 10) - 1;
            if (i > 0 && i < g_n) knock_out(i);
            if (!*p) break;
        }
    static int test_hp;  // LARECOMP_COOP_HP=<player>:<quarter hearts> (test): his hearts then, 2 s after he's placed
    if (!test_hp && g_placed_frame && renderer_present_count() > g_placed_frame + 120 && ENV("LARECOMP_COOP_HP")) {
        int i, q;
        test_hp = 1;
        if (sscanf(ENV("LARECOMP_COOP_HP"), "%d:%d", &i, &q) == 2 && i > 1 && i <= g_n) g_hp[i - 1] = q;
    }
    // what the game's hearts gained - or, from a heart / healing amount, all of it, even what Player 1 had no room for
    int gained = g_game_hp >= 0 && h > g_game_hp ? h - g_game_hp : 0;
    if (g_heal_pending > gained) gained = g_heal_pending;
    g_heal_pending = 0;
    g_game_hp = h;
    for (int i = 1; i < g_n; i++) {
        if (g_hp[i] < 0 || g_settings.cheat_hearts) g_hp[i] = g_max_hp;
        if (!g_down[i]) g_hp[i] = g_hp[i] + gained > g_max_hp ? g_max_hp : g_hp[i] + gained;
    }
    // Revives: another player who is up, within 1.6 of him, holding A
    extern uint64_t input_coop_buttons(int player);
    extern uint64_t input_buttons_now(void);
    float step = 1.0f / (60.0f * (float)(g_settings.coop_revive_s > 0 ? g_settings.coop_revive_s : 4));
    memset(g_can_revive, 0, sizeof g_can_revive);
    for (int i = 1; i < g_n; i++) {
        if (!g_down[i]) continue;
        float pi[3];
        bool helped = false;
        if (player_pos(g_extra[i], pi))
            for (int j = 0; j < g_n; j++) {
                float pj[3];
                if (j == i || (j && g_down[j]) || !player_pos(coop_player(j), pj)) continue;
                float dx = pj[0] - pi[0], dz = pj[2] - pi[2];
                if (dx * dx + dz * dz >= 1.6f * 1.6f) continue;
                g_can_revive[j] = true;  // (his "A Revive" prompt: runtime/ui/coop_hud.mm)
                uint64_t b = j ? input_coop_buttons(j) : input_buttons_now();
                helped |= (b & 1) != 0;  // (bit 0: A)
            }
        g_revive[i] = helped ? g_revive[i] + step : g_revive[i] > step ? g_revive[i] - step : 0;
        if (g_revive[i] >= 1) revive(i, "by another player");
    }
}

// ---- Cameras (COOP_CAMERA_SITE: x23 a camera whose view matrix was just set - columns at +0x580 / +0x590 / +0x5a0
// and the translation at +0x5b0; vertical field of view +0x564, radians). The view is Player 1's own camera (every
// Link has one, +0x318: COOP_CAMERA_SCROLL_SITE) when it's among them, else the one Player 1 is in front of and near
// the middle of; the HUD projects a point with it (coop_project).
#define CAMS 12
static uint64_t g_cam[CAMS], g_main_cam;

static void cameras_forget(void) { memset(g_cam, 0, sizeof g_cam); g_main_cam = 0; }
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

// World point -> where it is in the view: x, y from -1 to 1 across it (out of that range: off screen), *front whether
// it is in front of the camera. False without a camera yet.
static bool view_point(const float p[3], float aspect, float *nx, float *ny, bool *front) {
    uint64_t cam = view_camera();
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

// World point -> window position (pixels, for a w x h window); false when off screen or no camera yet.
// (UI thread: guest memory stays mapped, so a camera gone a moment ago reads as garbage - off screen - not a fault)
bool coop_project(const float p[3], float w, float h, float *sx, float *sy) {
    float nx, ny;
    bool front;
    if (!view_point(p, w / h, &nx, &ny, &front) || !front) return false;
    *sx = (nx * 0.5f + 0.5f) * w, *sy = (0.5f - ny * 0.5f) * h;
    return nx > -1.2f && nx < 1.2f && ny > -1.2f && ny < 1.2f;
}

// A player on screen: his head (just above him: lying down) for the "Zzz" bubble, or his feet (below him) for the
// "A Revive" prompt. Off screen, or behind the camera, it's kept to the window's edge in his direction (*edge): the
// bubble marks where a knocked-out player was left.
static bool player_screen(int i, float lift, float w, float h, float *sx, float *sy, bool *edge) {
    float p[3], nx, ny;
    bool front;
    if (i < 0 || i >= COOP_MAX || g_n < 2 || !player_pos(coop_player(i), p)) return false;
    p[1] += lift;
    if (!view_point(p, w / h, &nx, &ny, &front)) return false;
    if (!front) nx = -nx, ny = -ny;  // (behind: mirrored through the camera)
    const float mx = 0.9f, my = 0.8f;  // (the bubble's room at the edges)
    float k = fmaxf(fabsf(nx) / mx, fabsf(ny) / my);
    *edge = !front || k > 1;
    if (*edge && k > 0) nx /= fmaxf(k, 1), ny /= fmaxf(k, 1);
    *sx = (nx * 0.5f + 0.5f) * w, *sy = (0.5f - ny * 0.5f) * h;
    return true;
}

// ---- The camera zoomed out (setting coop_camera): its field of view widened by this (runtime/hle/widescreen.c
// rt_fovy, also the culling pass's, and the sky and sea's reach) - a set amount, or (auto) enough to keep every
// player who is up in view, eased in and out, up to ZOOM_MAX. 1 out of play and without co-op.
#define ZOOM_MAX 1.5f
static float g_zoom = 1.0f;
float coop_camera_zoom(void) { return g_zoom; }

static void zoom_tick(bool play) {
    static const float set[] = {1.0f, 1.1f, 1.25f, 1.5f};
    float want = 1.0f;
    int mode = g_settings.coop_camera;
    if (!play || !rt_coop_on()) want = 1.0f;
    else if (mode >= COOP_CAM_NORMAL && mode < COOP_CAM_AUTO) want = set[mode];
    else if (mode == COOP_CAM_AUTO) {  // (how far out each player is at the game's own view: x, y beyond 0.7 zoom out)
        extern float widescreen_factor(void);  // (runtime/hle/widescreen.c: the window's aspect over 16:9)
        float far = 0, p[3], nx, ny, aspect = 16.0f / 9.0f * widescreen_factor();
        bool front;
        for (int i = 1; i < g_n; i++)
            if (!g_down[i] && player_pos(g_extra[i], p) && view_point(p, aspect, &nx, &ny, &front) && front)
                far = fmaxf(far, fmaxf(fabsf(nx), fabsf(ny)) * g_zoom);
        want = fminf(fmaxf(far / 0.7f, 1.0f), ZOOM_MAX);
    }
    if (mode != COOP_CAM_AUTO || !play) g_zoom = want;
    else g_zoom += (want - g_zoom) * (want > g_zoom ? 0.06f : 0.02f);  // (out quicker than back in)
    if (fabsf(g_zoom - want) < 0.002f) g_zoom = want;
}

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
    for (int i = 1; i < g_n; i++) {
        float p[3], nx, ny;
        bool front;
        if (!g_settings.coop_leash || g_p1_event || !player_pos(g_extra[i], p) ||
            !view_point(p, aspect, &nx, &ny, &front) || (front && fabsf(nx) < 1.05f && fabsf(ny) < 1.05f)) {
            away[i] = 0;
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

bool coop_player_screen(int i, float w, float h, float *sx, float *sy, bool *edge) {
    return i >= 1 && player_screen(i, 1.0f, w, h, sx, sy, edge);
}

// Where to show player i's "A Revive" prompt (below him), when he is up beside a knocked-out player.
bool coop_revive_prompt(int i, float w, float h, float *sx, float *sy) {
    bool edge;
    return i >= 0 && i < COOP_MAX && g_can_revive[i] && player_screen(i, -0.3f, w, h, sx, sy, &edge) && !edge;
}

// The HUD (runtime/ui/coop_hud.mm): each player's hearts, maximum, knocked out / revive progress and colour.
typedef struct { int hp, max, down; float revive; uint32_t rgb; } CoopHudRow;
uint32_t coop_colour_rgb(int player);

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

// The player who owns a colour set and its colour as drawn (hearts, the HUD): the co-op Playerlink.bfres's sets.
uint32_t coop_colour_rgb(int player) {
    static const uint32_t k[COOP_COLOURS] = {0x2fa043, 0xd8262a, 0x2c49d6, 0xe8c11c, 0x5b2fbe, 0xd02fc4, 0x3a3a3a,
                                             0xdcdcdc};
    int s = player >= 0 && player < COOP_MAX ? g_settings.coop_colour[player] : 0;
    return k[s >= 0 && s < COOP_COLOURS ? s : 0];
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

// ---- Enemy targets (setting coop_targets). Enemies go after "Link": the game manager's player (+0x4b8). While an
// enemy's per-frame update runs (COOP_ACTOR_EXEC_*: actors are told apart by their level name at +0x18,
// "EnemyOctorok-<key>", bosses too), that is the Link it targets; Player 1's again after. The game has no "attacking"
// state common to its enemies, so an enemy close to its target (TARGET_ENGAGE) counts as fighting him: then it stays
// on him until he is knocked out - a charge, a wind-up, a boss's pattern plays out on one player. Otherwise it keeps
// a target TARGET_LOCK frames (and TARGET_GRACE after a fight: a charge carries it past him), then takes another only
// when he is clearly nearer (1.5x); before that only when its target is far off and another player is at it.
#define TARGET_SLOTS 256
#define TARGET_LOCK 360       // (6 s)
#define TARGET_GRACE 120      // (2 s)
#define TARGET_ENGAGE 6.0f    // (about 4 tiles: a Moblin throws its spear from there)
#define TARGET_FAR 30.0f      // (about two screens)
static struct { uint64_t actor, until, seen; int who; } g_target[TARGET_SLOTS];

static void targets_forget(void) {
    memset(g_target, 0, sizeof g_target);
    g_swap_on = false, g_swapped = 0;
}
RT_ON_RESET(coop_targets_reset) { targets_forget(); }

static bool actor_pos(uint64_t actor, float out[3]) {  // (an actor's main component +0x80: its position at +0x80)
    uint64_t p = actor ? LD64(actor + 0x80) : 0;
    if (!p) return false;
    memcpy(out, (const void *)(uintptr_t)(p + 0x80), 12);
    return true;
}

static int enemy_target(uint64_t enemy) {
    float e[3], p[3];
    if (!actor_pos(enemy, e)) return 0;
    float d[COOP_MAX], bd = 1e30f;
    int best = -1;
    for (int i = 0; i < g_n; i++) {
        d[i] = 1e30f;
        if ((i && g_down[i]) || !player_pos(coop_player(i), p)) continue;
        d[i] = (p[0] - e[0]) * (p[0] - e[0]) + (p[2] - e[2]) * (p[2] - e[2]);
        if (d[i] < bd) bd = d[i], best = i;
    }
    if (best < 0 || bd > TARGET_FAR * TARGET_FAR) return 0;  // (far from everyone: off screen, nothing to keep)
    // its slot: the top bits of a multiplicative hash (actors sit on 4 KB boundaries), a short probe; when that is
    // full, the entry seen longest ago goes
    uint64_t frame = renderer_present_count();
    uint32_t h = (uint32_t)(((enemy >> 12) * 0x9E3779B97F4A7C15ull) >> 56) % TARGET_SLOTS, old = h;
    for (int k = 0; k < 8; k++) {
        uint32_t s = (h + k) % TARGET_SLOTS;
        if (g_target[s].actor == enemy || !g_target[s].actor) { old = s; break; }
        if (g_target[s].seen < g_target[old].seen) old = s;
    }
    h = old;
    // (an entry not seen for a while is an earlier enemy's: the game reuses actors' memory)
    bool known = g_target[h].actor == enemy && frame - g_target[h].seen < 30;
    int cur = known ? g_target[h].who : -1;
    float cd = cur >= 0 && cur < g_n ? d[cur] : 1e30f, engage = TARGET_ENGAGE * TARGET_ENGAGE;
    g_target[h].actor = enemy, g_target[h].seen = frame;
    if (cur >= 0 && cd < 1e30f) {  // (its target is up)
        if (cd <= engage) {  // fighting him
            if (g_target[h].until < frame + TARGET_GRACE) g_target[h].until = frame + TARGET_GRACE;
            return cur;
        }
        bool clearly = cd > bd * 2.25f;
        if (frame < g_target[h].until ? !(clearly && bd <= engage && cd > 4 * engage) : !clearly) return cur;
    }
    if (best != cur && cur >= 0)
        CLOG("[coop] %.32s: P%d (%.1f away) -> P%d (%.1f)\n", (const char *)(uintptr_t)(enemy + 0x18), cur + 1,
             cd < 1e30f ? sqrtf(cd) : -1.0f, best + 1, sqrtf(bd));
    g_target[h].who = best, g_target[h].until = frame + TARGET_LOCK;
    return best;
}

static uint64_t g_updating;  // the actor whose update is running (COOP_ACTOR_EXEC_*), on the game thread
static pthread_t g_game_thread;  // (coop_tick's: only the game thread's updates take part)
static bool g_game_thread_known;

static bool on_game_thread(void) { return g_game_thread_known && pthread_equal(pthread_self(), g_game_thread); }

static void actor_exec_begin(Cpu *c) {
    if (!g_settings.coop_targets || g_swap_on) return;
    uint64_t actor = g_updating;
    if (!actor || strncmp((const char *)(uintptr_t)(actor + 0x18), "Enemy", 5)) return;
    char name[64];  // (not "EnemyReactionComponent"s)
    snprintf(name, sizeof name, "%.63s", (const char *)(uintptr_t)(actor + 0x18));
    if (strstr(name, "Component")) return;
    int t = enemy_target(actor);
    uint64_t gm = LD64(GA(GAME_MGR)), own = gm ? LD64(gm + 0x4b8) : 0;
    if (!t || !own || !collision_of(g_extra[t])) return;
    g_swap_on = true, g_swapped = own;
    ST64(gm + 0x4b8, g_extra[t]);
}

static void actor_exec_end(void) {
    if (!g_swap_on) return;
    uint64_t gm = LD64(GA(GAME_MGR));
    if (gm) ST64(gm + 0x4b8, g_swapped);
    g_swap_on = false, g_swapped = 0;
}

// The player creator (an event action, main+0x83a470): the game makes its Link (the manager's +0x4b8). With co-op
// it is run once for each other player first - their Links stay from then on, as the mod's - then for Player 1, so
// the manager ends up holding his, as without co-op.
// The Links made for the other players outlive a game over and a return to the title (the game keeps them, as Player
// 1's): play_ended forgets which player is which, and the next file's players take these back - any seen updating in
// the last MADE_ALIVE frames - rather than getting new ones beside them (left over, they copied Player 1). One that
// isn't taken back (fewer players now) is parked: out of sight below the level, its update skipped.
#define MADE_MAX 32
#define MADE_ALIVE 120
static struct { uint64_t actor, seen; } g_made[MADE_MAX];
RT_ON_RESET(coop_made_reset) { memset(g_made, 0, sizeof g_made); }

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

static uint64_t made_take_back(void) {  // (a live Link made earlier that no player holds now)
    uint64_t now = renderer_present_count();
    for (int k = 0; k < MADE_MAX; k++) {
        uint64_t a = g_made[k].actor;
        if (!a || coop_index(a) || a == host_player() || now - g_made[k].seen > MADE_ALIVE) continue;
        if (LD64(a) == GA(PLAYER_VTABLE)) return a;
    }
    return 0;
}

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
    for (int i = 1; i < g_n; i++) {
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
static int link_at(double x, double y, double z) {
    for (int i = 0; i < g_n; i++) {
        float p[3];
        if (!player_pos(coop_player(i), p)) continue;
        double dx = x - p[0], dz = z - p[2], dy = y - (p[1] + 0.72);
        if (dx * dx + dz * dz < 0.04 && dy > -0.5 && dy < 0.5) return i;
    }
    return -1;
}

// PhysX character-controller sweep (COOP_CCT_SWEEP_SITE_*): x28 a touched geometry, x19 the swept volume (its
// centre: doubles at +8). The game gives each character controller the others as user capsule obstacles
// (TouchedGeomType eUSER_CAPSULE = 1: no owner, just the capsule - endpoints as doubles at +0x2c / +0x44, radius
// +0x5c). With player collision off, a Link's sweep skips the capsule of another Link: they walk through each
// other; everything else still blocks them. (Any Link's: stacked Links share one spot, so which one a capsule is
// can't be told - and a Link's own capsule isn't among his obstacles.)
static bool cct_sweep(Cpu *c) {
    uint64_t g = c->x[28];
    if (LD32(g) != 1 || g_settings.coop_collision) return false;
    double vol[3], p0[3], p1[3];
    memcpy(vol, (const void *)(uintptr_t)(c->x[19] + 8), 24);
    memcpy(p0, (const void *)(uintptr_t)(g + 0x2c), 24);
    memcpy(p1, (const void *)(uintptr_t)(g + 0x44), 24);
    if (link_at(vol[0], vol[1], vol[2]) < 0) return false;
    return link_at((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2, (p0[2] + p1[2]) / 2) >= 0;
}

// ---- Exits (setting coop_host_exits). Doors and loading zones are AreaLevelOpen actors: the contact signal of an
// AreaLevelOpen's collision (main+0x76dc0 -> main+0x76e90) keeps a touching player actor - +0x43 bit 6 clear - as its
// pending exit (+0x400), and its update (main+0x75320) plays the exit out for the game manager's player once he can
// go (main+0xd58d70; main+0x75ac0: the exit's event - the walk out, the level jump), whoever touched it. Stairs
// (AreaUpstairs / AreaDownstairs: main+0x617a0 stepped on, main+0x61600 off) keep the player actor standing on them
// (+0x3f0); their update (main+0x60140) checks him and starts their event, which takes Player 1 up or down. At those
// checks (x[actor] the player actor, x[zone] the exit, the site's ldrb loading +0x43 into x[out]) another player's
// Link reads as one with the bit set: his touch takes no one along and his stepping off doesn't drop Player 1's.
static int exit_touch(Cpu *c, int actor, int zone, int out, bool touch) {
    int i = coop_index(c->x[actor]);
    if (!i || !g_settings.coop_host_exits) return 0;
    static uint64_t logged_frame;
    uint64_t frame = renderer_present_count();
    if (touch && frame > logged_frame + 30)
        CLOG("[coop] player %d at an exit (%.48s): only Player 1 leads out\n", i + 1,
             (const char *)(uintptr_t)(c->x[zone] + 0x18));
    if (touch) logged_frame = frame;
    c->x[out] = LD8(c->x[actor] + 0x43) | 0x40;
    return 1;
}

// The Link (player index) a collision contact is with: the contact (+0x10) -> its body's owner component (+0x438,
// as main+0x3ee40 reads it) -> the actor (+0x48). -1: not a Link. (COOP_EXIT_LEAVE_*: a door's contact ending)
static int g_leaving = -1;
static int contact_link(uint64_t contact) {
    uint64_t p = heap_ptr(contact) ? LD64(contact + 0x10) : 0, o = heap_ptr(p) ? LD64(p + 0x438) : 0;
    uint64_t actor = heap_ptr(o) ? LD64(o + 0x48) : 0;
    if (!actor) return -1;
    return actor == host_player() ? 0 : coop_index(actor) ? coop_index(actor) : -1;
}

// ---- Camera areas (COOP_CAMERA_SCROLL_SITE). Every Link the player creator makes has a camera of his own (+0x318),
// and every camera listens to the camera-area manager (main+0x836270, its signal +0x750), which follows the game
// manager's player: Player 1. When he steps into another scroll area, a camera with +0x515 set answers with the
// "ZeldaScroll" event (main+0xbc540): Player 1 walks on into the new area while the view scrolls. On the Field Player
// 1's camera has it clear and the other Links' cameras set (measured), so Player 1 walked a "scroll" at every
// scroll-area line - just in front of the Trendy Game's door one turns him back down, every time. Only Player 1's
// camera answers.
// Player 1 into another room (his own camera's answer: a new camera area): indoors - dungeons, houses, caves, not
// the overworld's screens - knocked-out players are back, on him, as in a new area (coop_place).
static void room_changed(void) {
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    const char *lv = chaos_current_level();
    if (!lv || !*lv || !strcmp(lv, "Field")) return;
    for (int i = 1; i < g_n; i++)
        if (g_down[i]) {
            revive(i, "new room");
            ride_on_host(i);
        }
}

static int camera_scroll(Cpu *c) {
    uint64_t me = host_player(), mine = me ? LD64(me + 0x318) : 0, cam = c->x[19];
    if (cam && cam == mine) room_changed();
    if (!mine || cam == mine) return 0;
    CLOG("[coop] camera %#llx (not Player 1's %#llx) would scroll to a new camera area: skipped (frame %llu)\n",
         (unsigned long long)cam, (unsigned long long)mine, (unsigned long long)renderer_present_count());
    c->x[8] = 0;
    return 1;
}

int rt_coop_hook(Cpu *c, int which) {
    if (!rt_coop_on()) return 0;
    switch (which) {
    case 0:  // COOP_CREATE_SITE
        return coop_create(c);
    case 1:  // COOP_ZONE_SITE - bl main+0x83afa0: the creator's zone set-up, Player 1's Link only. It returns a
        // shared_ptr in x0's {ptr, control} (sp+0x40), which the creator then releases: skipped, it is empty
        if (g_creating <= 0) return 0;
        ST64(c->x[0], 0);
        ST64(c->x[0] + 8, 0);
        return 1;
    case 2:  // COOP_DUP_SITE - cbnz x0: "a Link exists already" let through while the creator makes the other Links
        return g_creating >= 0 ? 1 : 0;
    case 3:  // COOP_PLACE_SITE
        return coop_place(c);
    case 4:  // COOP_PLACE_PLAYER_SITE - ldr x20, [x19, #0x4b8]: the Link being placed
        c->x[20] = g_placing > 0 ? g_extra[g_placing] : LD64(c->x[19] + 0x4b8);
        return 1;
    case 5:  // COOP_SPAWN_CHECK_SITE - its object (x22) can be gone with more Links: then skipped (the mod: always)
        return c->x[22] ? 0 : 2;
    case 6: {  // COOP_CTRL_SITE - ldp x22, x26, [x8]: Link's controller (shared_ptr) from the npad list
        int i = coop_index(c->x[19]);
        c->x[22] = LD64(c->x[8] + 16 * i);
        c->x[26] = LD64(c->x[8] + 16 * i + 8);
        static uint32_t logged;
        if (!(logged & (1u << i))) {
            logged |= 1u << i;
            CLOG("[coop] player %d reads controller %#llx; list:", i + 1, (unsigned long long)c->x[22]);
            for (int k = 0; k < 10; k++) CLOG(" %#llx", (unsigned long long)LD64(c->x[8] + 16 * k));
            CLOG("\n");
        }
        return 1;
    }
    case 7: {  // COOP_COLOR_SITE - blr x8 with s0 = the tunic frame (0 green, 1 red, 3 blue)
        extern bool coop_colours(void);
        if (!coop_colours()) return 0;
        float f;
        memcpy(&f, &c->v[0], 4);
        // (the coop Playerlink.bfres: 3 frames per colour set, all in that set's colour - 0-2 green, 3-5 red, ...)
        int t = f >= 2.5f ? 2 : f >= 0.5f ? 1 : 0, set = g_settings.coop_colour[coop_index(c->x[19])];
        f = (float)((set >= 0 && set < COOP_COLOURS ? set : 0) * 3 + t);
        __uint128_t v = 0;
        memcpy(&v, &f, 4);
        c->v[0] = v;
        return 0;
    }
    case 8:  // COOP_ENTER_SITE
        return coop_enter(c);
    case 9:  // COOP_SCROLL_SITE - str x8, [sp]: event entry "ZeldaScroll". The mod renames it ("eldaScroll": no such
        // entry) and the Switch catches the event system's "not found" throw further up; recompiled code can't
        // unwind (fatal: tested 2026-10-27). Left as it is: it plays as without co-op.
        return 0;
    case 10:  // COOP_PAD_STATE_SITE - b.ne: controllers polled whatever the manager's state
        return 1;
    case 11:  // COOP_APPLET_SITE - strb w20, [sp, #0x11]: the controller applet's player count
        ST8(c->sp + 0x10, g_n);
        ST8(c->sp + 0x11, g_n);
        return 1;
    case 12:  // COOP_APPLET_SINGLE_SITE - strb w8, [sp, #0x15]: not single-player
        ST8(c->sp + 0x15, 0);
        return 1;
    case 13: case 14:  // COOP_CCT_SWEEP_SITE_0 / _1 - cbz x8: a touched geometry's sweep
        return cct_sweep(c) ? 2 : 0;
    case 15: {  // COOP_ACTOR_EXEC_SITE - blr x9: an update callback (x0; [x0+8] its first capture: for an actor's, him)
        if (!on_game_thread()) return 0;
        uint64_t a = heap_ptr(c->x[0]) ? LD64(c->x[0] + 8) : 0;
        g_updating = heap_ptr(a) ? a : 0;
        int made = made_index(g_updating);
        if (made >= 0) {
            g_made[made].seen = renderer_present_count();
            if (!coop_index(g_updating) && g_updating != host_player() && g_creating < 0) {  // (left over: parked)
                uint64_t p = collision_of(g_updating);
                if (p && LD32(p + 0x84) != 0xc47a0000u) {  // (y -1000)
                    CLOG("[coop] Link %#llx left over: parked\n", (unsigned long long)g_updating);
                    ST32(p + 0x84, 0xc47a0000u);
                }
                g_updating = 0;
                return 1;
            }
        }
        actor_exec_begin(c);
        return 0;
    }
    case 16:  // COOP_ACTOR_EXEC_DONE - after it
        if (!on_game_thread()) return 0;
        actor_exec_end();
        if (g_updating) coop_actor_updated(c, g_updating);
        g_updating = 0;
        return 0;
    case 17:  // COOP_CAMERA_SITE - a camera's view matrix is set (x23)
        camera_seen(c->x[23]);
        return 0;
    case 18: case 19: {  // COOP_HEAL_SITE_0 / _1 - strh w8, [x9, #0x84]: the hearts after a heal; w23 the total uncapped
        int before = game_hearts(), total = (int)(int16_t)c->x[23];
        if (before >= 0 && total > before) g_heal_pending += total - before;
        return 0;
    }
    case 20:  // COOP_EXIT_TOUCH_SITE - ldrb w9, [x23, #0x43]: a player actor (x23) touched an AreaLevelOpen (x20)
        return exit_touch(c, 23, 20, 9, true);
    case 21:  // COOP_STAIRS_TOUCH_SITE - ldrb w8, [x8, #0x43]: a player actor (x8) stepped on stairs (x19)
        return exit_touch(c, 8, 19, 8, true);
    case 22:  // COOP_STAIRS_LEAVE_SITE - ldrb w8, [x8, #0x43]: ... stepped off them
        return exit_touch(c, 8, 19, 8, false);
    case 23:  // COOP_EXIT_LEAVE_SITE - a contact with an AreaLevelOpen ends (x1 the contact)
        g_leaving = contact_link(c->x[1]);
        CLOG("[coop] door contact ends: %s %d (frame %llu)\n", g_leaving < 0 ? "not a Link" : "player",
             g_leaving + 1, (unsigned long long)renderer_present_count());
        return 0;
    case 24:  // COOP_EXIT_LEAVE_CLEAR - bl main+0x76b20: drops its pending exit - not for another player's Link
        return g_leaving > 0;
    case 25:  // COOP_CAMERA_SCROLL_SITE - ldrb w8, [x19, #0x515]: camera x19 would scroll to a new camera area
        return camera_scroll(c);
    }
    return 0;
}

// The mod's tunic colours need its Playerlink.bfres (24 colour frames: 3 per player): served from
// <data root>/coop/romfs while co-op is on (runtime/services/fs.c).
static int g_coop_dir = -1;  // (<data root>/coop/romfs there: looked at once a boot, the romfs index asks per file)
static int g_colours = -1;   // (the colour file served: fixed for the boot, as the romfs index)
RT_ON_RESET(coop_files_reset) { g_coop_dir = g_colours = -1; }

bool coop_romfs_override(const char *path, char *out, size_t cap) {
    if (!rt_coop_on()) return false;
    extern const char *rt_data_root(void);
    const char *root = rt_data_root();
    if (!root) return false;
    if (g_coop_dir < 0) {
        snprintf(out, cap, "%s/coop/romfs", root);
        struct stat sb;
        g_coop_dir = stat(out, &sb) == 0 && S_ISDIR(sb.st_mode);
    }
    if (!g_coop_dir) return false;
    snprintf(out, cap, "%s/coop/romfs/%s", root, path);
    FILE *f = fopen(out, "rb");
    if (!f) return false;
    fclose(f);
    if (!strcmp(path, "region_common/actor/Playerlink.bfres")) {  // (the RomFS index serves it: this boot's colours)
        g_colours = 1;
        LOG("[coop] player colours: %s\n", out);
    }
    return true;
}

// The players' colours are in this boot's Playerlink.bfres only when co-op was on as the game started (the RomFS index
// is made at boot): otherwise the game's own frames stay - every Link in Player 1's tunic - until a restart.
bool coop_colours(void) { return g_colours > 0; }

// For the menu: 0 the colours are in, 1 they come with a restart (co-op turned on since the start), 2 there is no
// coop/romfs Playerlink.bfres.
int coop_colours_state(void) {
    if (g_colours > 0) return 0;
    extern const char *rt_data_root(void);
    const char *root = rt_data_root();
    char p[1200];
    snprintf(p, sizeof p, "%s/coop/romfs/region_common/actor/Playerlink.bfres", root ? root : ".");
    struct stat sb;
    return stat(p, &sb) == 0 ? 1 : 2;
}

// The game's countdown (timed challenges: Dg_TimeCount in region_common/ui/Game.arc) sits in the bottom left, where
// the other players' hearts go (runtime/ui/coop_hud.mm). With co-op it moves to the bottom centre: the x of its pane
// N_Time_00 (its parts span -580..-408 of the 1280-wide layout), 4 bytes of the archive laid over the game's reads of
// it (runtime/services/fs.c, `rd` reads the RomFS image, the file at `off`). Found by name: any game version.
bool coop_romfs_patch(const char *path, uint64_t off, uint64_t size, ssize_t (*rd)(void *, uint64_t, uint64_t),
                      uint64_t *at, uint32_t *val) {
    static const char want[] = "blyt/Dg_TimeCount.bflyt";
    if (strcmp(path, "region_common/ui/Game.arc") || !rt_coop_on()) return false;
    uint8_t h[0x20];
    uint16_t hs, n;
    uint32_t data;
    if (rd(h, 0x14, off) != 0x14 || memcmp(h, "SARC", 4)) return false;
    memcpy(&hs, h + 4, 2), memcpy(&data, h + 0xc, 4);
    if (rd(h, 0xc, off + hs) != 0xc || memcmp(h, "SFAT", 4)) return false;
    memcpy(&n, h + 6, 2);
    uint64_t sfnt = off + hs + 0xc + 16ull * n, names = sfnt + 8;
    if (off + data <= names || data > size) return false;
    uint8_t *fat = malloc(16ull * n), *nm = malloc(off + data - names + 1);
    bool ok = false;
    uint64_t file = 0, len = 0;
    if (rd(fat, 16ull * n, off + hs + 0xc) == (ssize_t)(16ull * n) &&
        rd(nm, off + data - names, names) == (ssize_t)(off + data - names)) {
        nm[off + data - names] = 0;
        for (int i = 0; i < n && !ok; i++) {
            uint32_t attr, s, e;
            memcpy(&attr, fat + 16 * i + 4, 4), memcpy(&s, fat + 16 * i + 8, 4), memcpy(&e, fat + 16 * i + 12, 4);
            uint64_t no = (attr & 0xffff) * 4ull;
            if (no + sizeof want <= off + data - names && !memcmp(nm + no, want, sizeof want))
                ok = true, file = off + data + s, len = e - s;
        }
    }
    free(fat), free(nm);
    if (!ok || len > 0x10000) return false;
    uint8_t *f = malloc(len);
    ok = false;
    if (rd(f, len, file) == (ssize_t)len) {
        uint16_t fh;
        memcpy(&fh, f + 6, 2);
        for (uint64_t p = fh; p + 0x30 <= len && !ok;) {  // (sections: the panes, x at +0x2c)
            uint32_t sz;
            memcpy(&sz, f + p + 4, 4);
            if (!memcmp(f + p, "pan1", 4) && !strncmp((const char *)f + p + 0xc, "N_Time_00", 24)) {
                float x = 494;
                *at = file + p + 0x2c, memcpy(val, &x, 4), ok = true;
            }
            if (sz < 8) break;
            p += sz;
        }
    }
    free(f);
    if (ok) LOG("[coop] the game's countdown moved to the bottom centre (Game.arc +%#llx)\n", (unsigned long long)(*at - off));
    return ok;
}

// Play ended (back at the title / file select): the Links, the enemies' targets, the cameras are gone with the level -
// nothing of them is kept.
static void play_ended(void) {
    memset(g_extra, 0, sizeof g_extra);
    targets_forget();
    cameras_forget();
    anim_forget();
    hearts_forget();
    memset(g_asleep, 0, sizeof g_asleep);
    memset(g_sleep_anim, 0, sizeof g_sleep_anim);
}

// Once per frame on the game thread (runtime/hle/input.c, npad 1's poll).
void coop_tick(Cpu *c) {
    g_game_thread = pthread_self(), g_game_thread_known = true;
    extern int game_in_play(void);
    static bool in_play;
    bool play = game_in_play();
    if (in_play && !play) play_ended();
    in_play = play;
    zoom_tick(play);
    if (!play && GA(COOP_CREATE_SITE)) {  // (out of play: the player count for the next file - its Links come as it loads)
        int n = coop_players_setting();
        if (n != g_n) {
            extern void input_style_changed(void);  // (the game reads which controllers are connected again)
            LOG("[coop] %d players from the next file\n", n);
            g_n = n;
            input_style_changed();
        }
    }
    if (!rt_coop_on()) return;
    p1_event_tick();
    hearts_tick(c);
    sleep_tick(c);
    if (play) leash_tick();
    extern uint64_t input_coop_buttons(int player);
    uint64_t frame = renderer_present_count();
    for (int i = 1; i < g_n; i++) {
        if (!collision_of(g_extra[i])) continue;
        // both sticks pressed in: back to Player 1 (the mod's CheckAndTeleport)
        uint64_t b = input_coop_buttons(i);
        if ((b & 0x30) == 0x30) ride_on_host(i);
    }
    static uint64_t next;
    if (ENV("LARECOMP_COOP_LOG") && frame >= next) {
        next = frame + 30;
        char line[512];
        int k = snprintf(line, sizeof line, "[coop] frame %llu", (unsigned long long)frame);
        for (int i = 0; i < g_n && k < (int)sizeof line - 40; i++) {
            float p[3] = {0};
            player_pos(coop_player(i), p);
            k += snprintf(line + k, sizeof line - k, " P%d (%.2f %.2f %.2f)", i + 1, p[0], p[1], p[2]);
        }
        float hp[3], v[3], sx = 0, sy = 0;
        if (g_cam[0] && player_pos(host_player(), hp)) {
            cam_view(g_cam[0], hp, v);
            bool on = coop_project(hp, 1280, 720, &sx, &sy);
            k += snprintf(line + k, sizeof line - k, " | cam %#llx P1 view (%.2f %.2f %.2f) screen %d (%.0f %.0f) fovy %.3f",
                          (unsigned long long)g_cam[0], v[0], v[1], v[2], on, sx, sy, *(const float *)(uintptr_t)(g_cam[0] + 0x564));
        }
        LOG("%s\n", line);
    }
}
