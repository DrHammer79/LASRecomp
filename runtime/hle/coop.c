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
#include "../input_bind.h"
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

int coop_players_setting(void);
int rt_coop_players(void) {
    if (g_n < 0) {
        g_n = GA(COOP_CREATE_SITE) ? coop_players_setting() : 1;  // (1.0.1 only)
        if (g_n > 1) LOG("[coop] %d players\n", g_n);
    }
    return g_n;
}

int rt_coop_on(void) { return rt_coop_players() > 1; }

// False once the game is known to be a version without co-op (1.0.0: its tables have no COOP_* sites).
bool coop_supported(void) { return !g_ga || GA(COOP_CREATE_SITE); }

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

// Arrivals with player collision on: every player arrives on Player 1's spot (a spawn, a door, being brought back,
// L3+R3), where the Links' capsules would push each other apart - Player 1 along, maybe back into the doorway he came
// through - and the ones left standing in a doorway would keep him from going back in. So a player put there passes
// through the others (g_pass) until he walks off it himself: PASS_OFF from where he was put, clear of every other
// Link (pass_tick). Until then no Link's sweep is blocked by a capsule where he is, nor his by anyone's (cct_sweep).
#define PASS_OFF 0.8f
#define PASS_CLEAR 0.7f  // (two capsules' width)
static bool g_pass[COOP_MAX];
static float g_pass_at[COOP_MAX][3];
static void pass_from_here(int i) { g_pass[i] = player_pos(g_extra[i], g_pass_at[i]); }

// Player i onto Player 1's spot (the mod's TeleportPlayerToAnother: the collision's position), and his safe spot - where
// the game puts him back after a fall (Link +0x1b40) - with it: one falling (through ground that isn't loaded, see
// lost_tick) has his position set by the game, back on that spot a frame later; then the spot is Player 1's.
static void ride_on_host(int i) {
    float pos[3];
    uint64_t p = collision_of(g_extra[i]);
    if (!p || !player_pos(host_player(), pos)) return;
    memcpy((void *)(uintptr_t)(p + 0x80), pos, 12);
    memcpy((void *)(uintptr_t)(g_extra[i] + 0x1b40), pos, 12);
    memcpy(g_pass_at[i], pos, sizeof pos), g_pass[i] = true;
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

// A knocked-out player hit again and again lies in something that hurts: knocked back by each hit, he never lay still
// (user 2026-10-28: spikes in a side-view room of Angler's Tunnel, kinds 11 and 3 in turn). DOWN_HITS of them, each
// within DOWN_HIT_FRAMES of the last, and he is put on Player 1's spot as the next frame starts (down_move_tick), still
// knocked out, where he can be revived.
#define DOWN_HITS 2
#define DOWN_HIT_FRAMES 180  // (3 s)
static uint64_t g_down_hit_at[COOP_MAX];
static int g_down_hits[COOP_MAX];
static bool g_down_move[COOP_MAX];
static uint64_t g_down_moved_until[COOP_MAX];  // (just put on Player 1: hits don't count - the spikes' last contact
                                               // came after him, and he was put on Player 1 again and again)

static void down_hit(int i) {
    uint64_t now = renderer_present_count();
    if (now < g_down_moved_until[i]) return;
    g_down_hits[i] = now - g_down_hit_at[i] < DOWN_HIT_FRAMES ? g_down_hits[i] + 1 : 1;
    g_down_hit_at[i] = now;
    if (g_down_hits[i] < DOWN_HITS || g_down_move[i]) return;
    g_down_hits[i] = 0, g_down_move[i] = true;
}

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

// The walk-in (kind 16: through a door, and at a dungeon's room change - the "ZeldaScroll") is one too, though the
// game's check lets it go: the others were put on Player 1 at its start only (coop_enter) and then walked on with
// their own controllers, ahead of him into the next room while he was still in the doorway. For the whole walk-in
// they stay on him, held (coop_tick).
#define LINK_KIND_WALK_IN 16
static bool g_p1_walk_in;

// Talks (and the other A actions that play an event: a chest, a sign). A talk another player's A started
// (doaction_get) plays on Player 1, and only Player 1's controller moved its text on: Player 2 talking to Marin waited
// for Player 1 to press A. While it plays the talker's A, B and directions reach Player 1's input too
// (runtime/hle/input.c), so he reads it through and answers its questions himself. A talk walks Player 1 up to the
// one he talks to with the walk-in (kind 16), which put everyone on him as at a door: during any of these nobody is
// moved (coop_enter, the walk-in ride in coop_tick).
#define TALK_START_FRAMES 60  // (no event begun by then: there was no talk)
static bool g_talking;        // (an A action's event, from the A until the event ends)
static int g_talker;          // (whose A: 1 = player 2; 0: Player 1's own)
static uint64_t g_talker_at;
int coop_talker(void) { return g_p1_event && g_talking ? g_talker : 0; }

static void talk_started(int who, uint64_t frame) { g_talking = true, g_talker = who, g_talker_at = frame; }

static void p1_event_tick(void) {
    int kind = link_state_kind(host_player());
    bool ev = kind >= 0 && kind <= 26 && ((1u << kind) & 0x4ec0000u || kind == LINK_KIND_WALK_IN);
    if (!ev && g_talking && (g_p1_event || renderer_present_count() > g_talker_at + TALK_START_FRAMES))
        g_talking = false, g_talker = 0;
    if (ev != g_p1_event) CLOG("[coop] Player 1 %s an event (state kind %d)\n", ev ? "in" : "out of", kind);
    g_p1_event = ev;
    g_p1_walk_in = kind == LINK_KIND_WALK_IN;
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
    if (g_down[i]) {
        down_hit(i);
        return 0;
    }
    if (g_settings.cheat_hearts) return 0;
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
                           // (LARECOMP_COOP_DOWN_AT=<frame>: at that frame instead - after a warp)
    uint64_t down_at = ENV("LARECOMP_COOP_DOWN_AT") ? strtoull(ENV("LARECOMP_COOP_DOWN_AT"), NULL, 10)
                                                    : g_placed_frame + 120;
    if (!test_down && g_placed_frame && renderer_present_count() > down_at && ENV("LARECOMP_COOP_DOWN"))
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
    // Revives: another player who is up, within 1.6 of him (height too), holding A
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
                // (height too: in a side view a player swimming above him was "beside" him - the Angler Fish, user
                // 2026-10-28)
                float dx = pj[0] - pi[0], dy = pj[1] - pi[1], dz = pj[2] - pi[2];
                if (dx * dx + dy * dy + dz * dz >= 1.6f * 1.6f) continue;
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

// ---- The camera zoomed out (settings coop_zoom, coop_camera_fit, coop_zoom_fit): g_zoom, coop_zoom 10% steps out
// always (up to 60%); with coop_camera_fit, further out as far as it takes to keep every player who is up in view, up
// to coop_zoom_fit steps (up to 200%: three times as far), never in of coop_zoom, eased in and out. 1 out of play and
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
// dungeon's door glides it - went further back each frame, to 120 away (user 2026-10-28, Angler's Tunnel).
static float g_back, g_back_d0 = 24.8f;  // (the distance moved back; Player 1's distance from the eye before it)
static uint64_t g_eye_node;              // (the node whose eye was moved, 0: none)
static float g_eye_wrote[3], g_eye_moved[3];
static void camera_back_undo(void) {
    float *eye = g_eye_node && heap_ptr(g_eye_node) ? (float *)(uintptr_t)(g_eye_node + 0xb0) : NULL;
    if (eye && !memcmp(eye, g_eye_wrote, sizeof g_eye_wrote))  // (as we left it: the camera hasn't been updated since)
        for (int k = 0; k < 3; k++) eye[k] -= g_eye_moved[k];
    g_eye_node = 0;
}
static void camera_back(Cpu *c) {
    static const int keep[] = {0x560, 0x56c, 0x350, 0x734};
    static uint64_t cam_was;
    static float orig[4], wrote[4];
    uint64_t cam = c->x[23], node = LD64(cam + 0x510);
    float p[3], v[3];
    if (cam != view_camera() || !heap_ptr(node)) return;
    float back = 0;
    if (g_back_on && g_zoom > 1.0f && player_pos(host_player(), p) && cam_view(cam, p, v))  // (a camera not on him: 24.8)
        g_back_d0 = -v[2] > 5.0f && -v[2] < 60.0f ? -v[2] : 24.8f, back = (g_zoom - 1.0f) * g_back_d0;
    for (int k = 0; k < 4; k++) {
        float *f = (float *)(uintptr_t)(cam + keep[k]);
        if (cam != cam_was || *f != wrote[k]) orig[k] = *f;  // (the game's own: a new camera, or it changed it)
        *f = wrote[k] = orig[k] + back;
    }
    extern void rt_tilt_focus_add(float);  // runtime/hle/qol.c
    rt_tilt_focus_add(back);
    g_back = back;
    float *m = (float *)(uintptr_t)(cam + 0x580), *copy = (float *)(uintptr_t)(c->x[24] + 0x20);
    float *eye = (float *)(uintptr_t)(node + 0xb0);
    if (g_eye_node == node) camera_back_undo();  // (not taken off yet - two camera updates in a frame: now)
    for (int k = 0; k < 3; k++)  // (row 2: its back axis)
        g_eye_moved[k] = back * m[2 + 4 * k], eye[k] += g_eye_moved[k];
    memcpy(g_eye_wrote, eye, sizeof g_eye_wrote);
    g_eye_node = back != 0 ? node : 0;
    cam_was = cam;
    m[14] -= back, copy[14] -= back;
}

// The zoom at which a point is inside frac of the view, the camera moved back (1: in it now, or nearer). From the
// view before the move (the matrix holds it moved by g_back): x across, y up, depth d. Moved back by B = (Z - 1) d0,
// it is in when |x| <= frac (d + B) tan(fovy / 2) aspect, and as much for y.
static float back_need(const float p[3], float aspect, float frac) {
    uint64_t cam = view_camera();
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
static bool zoom_field(void) {
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    const char *lv = chaos_current_level();
    return zoom_on() && lv && !strcmp(lv, "Field");
}

// The furthest the zoom can go before the view reaches past the screens a box of that scale makes (their edge, on the
// ground at Player 1's height, less ROOM_MARGIN: a tall grass patch's reach, ground lower than him), or 0 (no camera).
#define ROOM_MARGIN 2.0f
#define ROOM_STEPS 32  // (points along each side)
static float zoom_room(float aspect, float box) {
    float p[3], most = 1e9f;
    if (!player_pos(coop_player(0), p) || !view_camera()) return 0;
    float x0 = SCREEN_W * floorf((p[0] - LOAD_W * box / 2) / SCREEN_W) + ROOM_MARGIN;
    float x1 = SCREEN_W * ceilf((p[0] + LOAD_W * box / 2) / SCREEN_W) - ROOM_MARGIN;
    float z0 = SCREEN_D * floorf((p[2] - LOAD_D * box / 2) / SCREEN_D) + ROOM_MARGIN;
    float z1 = SCREEN_D * ceilf((p[2] + LOAD_D * box / 2) / SCREEN_D) - ROOM_MARGIN;
    for (int k = 0; k < 4 * ROOM_STEPS; k++) {
        float t = (float)(k % ROOM_STEPS) / ROOM_STEPS, side = k / ROOM_STEPS;
        float c[3] = {side == 0 ? x0 : side == 1 ? x1 : x0 + t * (x1 - x0), p[1],
                      side >= 2 ? (side == 2 ? z0 : z1) : z0 + t * (z1 - z0)};
        float z = back_need(c, aspect, 1.0f);  // (the zoom it comes into view at)
        if (z > 0) most = fminf(most, z);
    }
    return most < 1e9f ? most : 0;
}

// The box for a zoom wanted: grown at once, shrunk after BOX_SHRINK_FRAMES wanting less; g_box_ready catches up once
// the screens it takes in are made.
static void box_tick(float need, float aspect) {
    uint64_t now = renderer_present_count();
    float box = 1.0f;
    while (box < BOX_MAX && zoom_room(aspect, box) < need) box += BOX_STEP;
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
    if (!play || !zoom_on()) want = 1.0f;
    else if (fit) {  // (each player who is up inside 0.7 of the view; never in of base)
        float p[3];
        for (int i = 1; i < g_n; i++)
            if (!g_down[i] && player_pos(g_extra[i], p)) want = fmaxf(want, back_need(p, aspect, 0.7f));
        want = fminf(want, top);
    }
    if (!fit || !play) g_zoom = want;
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
                           : !view_point(p, aspect, &nx, &ny, &front) ? "no camera"
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
// (one screen away; at Out 50% still in view, so bring_back never took him). His position then is the game's to set:
// put on Player 1 while he falls, he's back on that spot a frame later. A player whose height drops LOST_DROP below
// where he last stood (state kind 0; a ledge is a few units) is lost and taken off the map, LOST_AWAY from then put
// on Player 1, his safe spot with him (user 2026-10-28: gone for a moment, then back with Player 1, rather than seen
// falling and popping up). While he falls he's below the ground; as the game stands him up again where he fell from,
// he's parked there out of sight (y -1000, his update not running: standing, as a teleport needs), until it's time.
// Whether or not the camera saw him, and with "bring back" off too.
#define LOST_DROP 8.0f
#define LOST_AWAY 150  // (2.5 s)
static uint64_t g_lost_until[COOP_MAX];  // (lost: put on Player 1 at this frame; 0: he isn't)
static bool g_lost_parked[COOP_MAX];     // (... standing again, parked out of sight until then)
static void lost_tick(void) {
    static float stood[COOP_MAX];
    static bool known[COOP_MAX];
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
            LOG("[coop] player %d back on Player 1 (fell through the world)\n", i + 1);
            if (player_pos(g_extra[i], p)) stood[i] = p[1];
            continue;
        }
        if (!q || !player_pos(g_extra[i], p) || kind < 0) { known[i] = false; continue; }
        if (kind == 0) {
            stood[i] = p[1];
            known[i] = true;
        } else if (known[i] && p[1] < stood[i] - LOST_DROP) {
            g_lost_until[i] = now + LOST_AWAY, g_lost_parked[i] = false;
            LOG("[coop] player %d fell through the world (from %.1f to %.1f): off the map for %.1f s\n", i + 1,
                stood[i], p[1], LOST_AWAY / 60.0f);
        }
    }
}
static bool lost_away(uint64_t actor) { int i = coop_index(actor); return i && g_lost_until[i] && g_lost_parked[i]; }
RT_ON_RESET(coop_lost_reset) { memset(g_lost_until, 0, sizeof g_lost_until), memset(g_lost_parked, 0, sizeof g_lost_parked); }

// The pause menu takes "the player's" spot as it opens - for its map, and for a save's restart point - from the first
// Link made: another player's (Player 1's is made last: the scene's camera is the last Link's; made first, the camera
// wasn't Player 1's). Not through the actors' name map (its "Player" entry repointed at Player 1 didn't change the
// file), nor from a pointer kept anywhere found, nor at save time (his position copies all on Player 1 while paused
// didn't change it). A file saved with him far away - fallen through ground that isn't loaded - loaded with Link
// falling forever (user 2026-10-28; with 2 players, saved with Player 2 9 units away, the file held Player 2's spot).
// So as Player 1 presses + or - (the pause menu, the map), every other Link's position - every copy his components
// keep - is Player 1's for the pause, put back once it is over (checked: saved with Player 2 away, the file holds
// Player 1's spot, and Player 2 is back on his own after the menu closes, walking).
static const uint16_t k_pos_copies[][2] = {  // ([Link + component] + offset: found 2026-10-28 by value)
    {0x80, 0x80}, {0x80, 0xb0}, {0x80, 0x350}, {0x2a8, 0xb0}, {0x2d8, 0x80}, {0x2d8, 0xb0}, {0x2d8, 0x350},
    {0x2e8, 0xb0}, {0x318, 0x380}, {0x328, 0xb0}, {0x378, 0xb0}, {0x388, 0xb0}, {0x3c0, 0x350}, {0x3d0, 0x1f0},
    {0x4a0, 0xb0}, {0x5b8, 0x290}, {0x1358, 0x290}, {0x1850, 0x2f0}, {0x19d0, 0x1f0}};
#define NPOS (sizeof k_pos_copies / sizeof k_pos_copies[0])

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

static uint64_t g_p1_updated;          // (the frame Player 1's update last ran)
static uint64_t g_held_at;             // (the frame the other Links were put on Player 1; 0: they aren't)

static struct { uint64_t addr; float v[3]; } g_held[COOP_MAX * NPOS];
static int g_nheld;

static void hold_on_p1(void) {
    float h[3];
    if (g_held_at || !player_pos(host_player(), h)) return;
    g_nheld = 0;
    for (int i = 1; i < g_n; i++) {
        if (!collision_of(g_extra[i])) continue;
        for (size_t k = 0; k < NPOS; k++) {
            uint64_t comp = LD64(g_extra[i] + k_pos_copies[k][0]), a = comp + k_pos_copies[k][1];
            bool dup = false;
            for (int j = 0; j < g_nheld && !dup; j++) dup = g_held[j].addr == a;
            if (!heap_ptr(comp) || dup) continue;
            g_held[g_nheld].addr = a;
            memcpy(g_held[g_nheld].v, (const void *)(uintptr_t)a, 12);
            memcpy((void *)(uintptr_t)a, h, sizeof h);
            g_nheld++;
        }
    }
    g_held_at = renderer_present_count();
    CLOG("[coop] pause menu: the other players on Player 1's spot (%d copies)\n", g_nheld);
}

static void hold_release(void) {
    for (int j = g_nheld - 1; j >= 0; j--) memcpy((void *)(uintptr_t)g_held[j].addr, g_held[j].v, 12);
    CLOG("[coop] pause over: the other players back (%d copies)\n", g_nheld);
    g_nheld = 0, g_held_at = 0;
}

static bool g_hold_next;  // (+ or - pressed: hold as Player 1's update - which opens the menu - runs)
static void pause_tick(void) {
    extern uint64_t input_buttons_now(void);  // (runtime/hle/input.c: Player 1's buttons, by BIND_*)
    static uint64_t last;
    uint64_t b = input_buttons_now();
    uint64_t pressed = b & ~last & (1ull << BIND_PLUS | 1ull << BIND_MINUS);
    last = b;
    if (pressed && !g_p1_event) g_hold_next = true;
}

// An actor's update about to run. Player 1's on the frame + or - was pressed holds the others on his spot (the menu
// opens after the frame's updates); theirs don't run while they're held - their own code never sees the copies
// changed - and once Player 1's runs again after the pause (or 8 frames on, no menu), they're let go. True: skip it.
static bool pause_updated(uint64_t actor) {
    if (!actor) return false;
    uint64_t now = renderer_present_count();
    if (actor == host_player()) {
        if (g_held_at && (now >= g_p1_updated + 2 || now > g_held_at + 8)) hold_release();
        g_p1_updated = now;
        if (g_hold_next) g_hold_next = false, hold_on_p1();
        return false;
    }
    return g_held_at && coop_index(actor);
}
RT_ON_RESET(coop_pause_reset) { g_p1_updated = g_held_at = 0, g_nheld = 0; }

// (neither while Player 1 is in an event - nobody else can act then: they showed over the warp holes' "Where to?" map,
// 2026-10-28)
bool coop_player_screen(int i, float w, float h, float *sx, float *sy, bool *edge) {
    return i >= 1 && !g_p1_event && player_screen(i, 1.0f, w, h, sx, sy, edge);
}

// Where to show player i's "A Revive" prompt (below him), when he is up beside a knocked-out player.
bool coop_revive_prompt(int i, float w, float h, float *sx, float *sy) {
    bool edge;
    return i >= 0 && i < COOP_MAX && g_can_revive[i] && !g_p1_event && player_screen(i, -0.3f, w, h, sx, sy, &edge) &&
           !edge;
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

// The player who owns a colour set and its colour as drawn (hearts, the HUD): the co-op Playerlink.bfres's sets. Red
// is a deeper crimson than the game's own heart red, so a red player's hearts don't read as "the" hearts.
uint32_t coop_colour_rgb(int player) {
    static const uint32_t k[COOP_COLOURS] = {0x2fa043, 0x8b1a2b, 0x2c49d6, 0xe8c11c, 0x5b2fbe, 0xd02fc4, 0x3a3a3a,
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

static int nearest_player(uint64_t enemy) {  // (who is nearest it, within TARGET_FAR; 0: Player 1, or nobody near)
    float e[3], p[3], bd = TARGET_FAR * TARGET_FAR;
    int best = 0;
    if (!actor_pos(enemy, e)) return 0;
    for (int i = 0; i < g_n; i++) {
        if ((i && g_down[i]) || !player_pos(coop_player(i), p)) continue;
        float d = (p[0] - e[0]) * (p[0] - e[0]) + (p[2] - e[2]) * (p[2] - e[2]);
        if (d < bd) bd = d, best = i;
    }
    return best;
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

static bool on_game_thread(void) {  // (per call of the actor hooks: pthread_self once per thread)
    static __thread pthread_t self;
    static __thread bool have;
    if (!have) self = pthread_self(), have = true;
    return g_game_thread_known && pthread_equal(self, g_game_thread);
}

// What an actor is, from its name (+0x18), worked out once: the update hooks (COOP_ACTOR_EXEC_*) run for every actor and
// component every frame - about 21,000 a frame with the screens made out to a far zoom - and reading each name there
// (snprintf, strstr; the grass and rocks' name hashed twice) cost the game thread about 2 ms a frame. Kept by the
// actor's address and its name's first 24 bytes, folded (another actor made at the same address is worked out
// again), in a table small enough for the CPU's cache.
// An enemy's components update on their own (the owner at +0x48). The one it reacts to the player with
// (EnemyReactionComponent: "<kind>:ReactionComp", "...ReactComp") keeps "Link" (Zombie:ReactionComp +0x278: Player 1's) -
// a zombie walked to Player 1 whoever it targeted (tested 2026-10-28) - and its player follower (TailorPlayerComp):
// these see its target as "Link" too (AC_ENEMY_PART). Not the others (collision, animation...): Player 1's sword hits it
// whoever it targets.
// A Gel (EnemyMiniZol) goes for the nearest player, not one it stays on (AC_ENEMY_NEAREST): one that jumped at its
// target and landed on another player clung to him, then went over to its target (user 2026-10-28).
#define AC_SETS 16384
#define AC_WAYS 4  // (an actor's set of four: the one seen longest ago makes way)
enum { AC_OTHER, AC_ENEMY, AC_ENEMY_NEAREST, AC_ENEMY_PART };
enum { GONE_NEW, GONE_LIVE, GONE_NOTED };  // (grass and rocks: not looked at yet; looked at; its death seen)
typedef struct {
    uint64_t actor, name;  // (its name's first 24 bytes, folded)
    uint64_t key;          // (grass and rocks: its name's hash - the level object's id; 0: not one of them)
    uint32_t seen;         // (the frame it was last looked up in)
    uint8_t what, gone;
} ActorClass;
static uint64_t gone_key(const char *name);
static ActorClass g_ac[AC_SETS][AC_WAYS];
static void actor_class_forget(void) { memset(g_ac, 0, sizeof g_ac); }
static ActorClass *actor_class(uint64_t actor) {
    const char *nm = (const char *)(uintptr_t)(actor + 0x18);
    uint64_t tag = LD64(actor + 0x18) ^ LD64(actor + 0x20) ^ LD64(actor + 0x28);
    uint32_t now = (uint32_t)renderer_present_count();
    // (the set: a multiplicative hash's top bits - actors sit on 4 KB boundaries, and the low bits used 64 sets of them)
    ActorClass *set = g_ac[(uint32_t)(((actor >> 4) * 0x9E3779B97F4A7C15ull) >> 50) & (AC_SETS - 1)], *e = set;
    for (int w = 0; w < AC_WAYS; w++) {
        if (set[w].actor == actor && set[w].name == tag) return set[w].seen = now, &set[w];
        if (set[w].seen < e->seen) e = &set[w];
    }
    e->seen = now;
    char name[64];
    snprintf(name, sizeof name, "%.63s", nm);
    e->actor = actor, e->name = tag, e->key = gone_key(name), e->gone = GONE_NEW;
    // (the reaction part is "...ReactionComp" or, for 28 kinds - Zols, Armos, Pokey, Togezo... - "...ReactionCom": red
    // Zols hopped at Player 1 whoever they targeted, user 2026-10-28)
    e->what = strstr(name, "React") || strstr(name, "PlayerComp") ? AC_ENEMY_PART
              : strstr(name, "Comp") || strncmp(name, "Enemy", 5) ? AC_OTHER
              : !strncmp(name, "EnemyMiniZol", 12) ? AC_ENEMY_NEAREST : AC_ENEMY;
    if (e->what == AC_ENEMY_PART && ENV("LARECOMP_COOP_LOG")) {  // (each kind of part once: which ones go by "Link")
        static uint64_t said[64];
        static int nsaid;
        bool known = false;
        for (int k = 0; k < nsaid && !known; k++) known = said[k] == tag;
        uint64_t owner = LD64(actor + 0x48);
        if (!known && nsaid < 64) {
            said[nsaid++] = tag;
            LOG("[coop] %.40s: an enemy part that goes by \"Link\" (its owner %.40s)\n", name,
                heap_ptr(owner) ? (const char *)(uintptr_t)(owner + 0x18) : "-");
        }
    }
    return e;
}

static void actor_exec_begin(Cpu *c) {
    if (!g_settings.coop_targets || g_swap_on) return;
    uint64_t actor = g_updating;
    if (!actor) return;
    uint8_t what = actor_class(actor)->what;
    int t;
    if (what == AC_ENEMY_PART) {
        // (its reaction to a hit goes by "Link" too: the target kept for the chase is the nearest player now, not the
        // one it stays on - another player's hit knocked it back from where its target stood, up onto a ledge: user
        // 2026-10-28, a turtle in Angler's Tunnel)
        uint64_t owner = LD64(actor + 0x48);
        uint8_t of = heap_ptr(owner) ? actor_class(owner)->what : AC_OTHER;
        if (of != AC_ENEMY && of != AC_ENEMY_NEAREST) return;
        t = nearest_player(owner);
    } else if (what == AC_ENEMY)
        t = enemy_target(actor);
    else if (what == AC_ENEMY_NEAREST)
        t = nearest_player(actor);
    else
        return;
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

// The creator, once for each other player, then for Player 1 - last: the scene's camera is the last Link's (made first,
// the camera wasn't his), and it leaves the game manager's player (+0x4b8) on him. (The pause menu still finds the
// first one made: see hold_on_p1.)
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
// The others follow him a moment later (ROOM_FOLLOW frames, once he is in the new room - stairs down into a side-view
// room move him there over a few frames): each one who is up and away from him (ROOM_NEAR) is put on him, as at a door
// - left in the room before, the ones in a side-view room's plane fell forever.
#define ROOM_FOLLOW 30
#define ROOM_NEAR 4.0f
static uint64_t g_room_frame;
static void room_changed(void) {
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    const char *lv = chaos_current_level();
    if (!lv || !*lv || !strcmp(lv, "Field")) return;
    for (int i = 1; i < g_n; i++)
        if (g_down[i]) revive(i, "new room");
    g_room_frame = renderer_present_count();
}

static void room_follow_tick(void) {
    static float last[3];
    static bool have;
    float h[3], p[3];
    if (!player_pos(host_player(), h)) return;
    // (Player 1 moved far in one frame - stairs into a side-view room, a room swapped in place - is a new room too)
    float dx = h[0] - last[0], dy = h[1] - last[1], dz = h[2] - last[2];
    if (have && dx * dx + dy * dy + dz * dz > 10.0f * 10.0f && !g_room_frame) g_room_frame = renderer_present_count();
    memcpy(last, h, sizeof last), have = true;
    // (and once he stands in it - put on him while stairs still carry him up, they'd be in the floor - and each one
    // on his own feet: one falling, the game puts back where he fell from when it's done, over a move made meanwhile)
    uint64_t now = renderer_present_count();
    if (!g_room_frame || now < g_room_frame + ROOM_FOLLOW || g_p1_event || link_state_kind(host_player()) != 0) return;
    bool waiting = false;
    for (int i = 1; i < g_n; i++) {
        if (!player_pos(g_extra[i], p) || (p[0] - h[0]) * (p[0] - h[0]) + (p[1] - h[1]) * (p[1] - h[1]) +
                                                  (p[2] - h[2]) * (p[2] - h[2]) <= ROOM_NEAR * ROOM_NEAR)
            continue;
        if (link_state_kind(g_extra[i]) != 0 && now < g_room_frame + 600) {  // (10 s at most)
            waiting = true;
            continue;
        }
        CLOG("[coop] player %d follows Player 1 into the room\n", i + 1);
        ride_on_host(i);
    }
    if (!waiting) g_room_frame = 0;
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

// ---- A-button actions: lifting, throwing, talking, reading, opening a chest... An object that can be acted on has a
// DoAction component. Its update (COOP_DOACTION_SITE, main+0xea240) checks itself against "Link" (+0x4b8): in reach,
// in front of him, able to act (its +0x88). The best of the frame goes into one slot (DOACTION_PENDING, a weak_ptr),
// which the frame's end (COOP_DOACTION_FRAME_SITE, main+0xe8be0) makes the current one (DOACTION_CURRENT): what Link's
// state code acts on when A is pressed (COOP_DOACTION_GET_SITE, main+0xe8d90) and the HUD's A label shows. One slot,
// one Link: every other player's A acted on what Player 1 could act on, wherever that player stood - in front of
// something else he got nothing, and pressing A just as Player 1 did put him into the same lift as Player 1, on Player
// 1's rock. With co-op every player has his own slots (players 2..: here, swapped into the game's while the game works
// on his): each component is checked for every player, the frame's end runs for each, and another player's own update
// (COOP_ACTOR_EXEC_*) acts on his. The countdown after an action (DOACTION_COOLDOWN) stays shared.
// A component also keeps what its last check found (+0x1b4..+0x1c4: angle, distance, height, score, "can act"). The
// next component's check compares its score with the pending one's, and the action reads "can act" again
// (main+0xeae90): Player 1's findings, his check being the last. Each other player's findings for his best component
// are kept with his slots and are in it while the game works on his.
#define ACT_RES_AT 0x1b4
#define ACT_RES 0x11
static uint8_t g_act[COOP_MAX][32];              // players 2..: their pending (+0) and current (+16) slots
static uint8_t g_act_res[COOP_MAX][2][ACT_RES];  // ... their components' findings (pending, current)
static int g_act_calls;                          // inside our own calls of the game's DoAction functions
static int g_act_swapped;                        // the player whose current slot is in the game's (his update), 0: none
static uint64_t g_act_comp;                      // meanwhile: his current component, holding his findings
static uint8_t g_act_own[ACT_RES];               // ... whose own findings (Player 1's) go back after
RT_ON_RESET(coop_act_reset) {
    memset(g_act, 0, sizeof g_act);
    g_act_calls = g_act_swapped = 0;
    g_act_comp = 0;
}

static void slot_swap(uint64_t slot, uint8_t *mine) {
    uint8_t t[16];
    memcpy(t, (const void *)(uintptr_t)slot, 16);
    memcpy((void *)(uintptr_t)slot, mine, 16);
    memcpy(mine, t, 16);
}

// The component a slot (weak_ptr {ptr, control}) holds while it lives (the control block's use count, +8, is -1 once
// it is gone), else 0.
static uint64_t slot_live(uint64_t slot) {
    uint64_t p = LD64(slot), ctrl = LD64(slot + 8);
    return heap_ptr(p) && heap_ptr(ctrl) && LD64(ctrl + 8) != ~0ull ? p : 0;
}

// Player i's findings into component p (what it held before into `keep`), or back.
static void res_put(uint64_t p, const uint8_t *res, uint8_t *keep) {
    if (keep) memcpy(keep, (const void *)(uintptr_t)(p + ACT_RES_AT), ACT_RES);
    memcpy((void *)(uintptr_t)(p + ACT_RES_AT), res, ACT_RES);
}

// A DoAction component's update (x0 the component): once for each other player who can act, "Link" being him, then for
// Player 1 into the game's own slot. (In an enemy's update the player slot holds the Link it targets: put back after.)
static int doaction_each(Cpu *c) {
    uint64_t gm = LD64(GA(GAME_MGR));
    if (g_act_calls || !gm || !GA(DOACTION_PENDING)) return 0;
    if (!on_game_thread()) {  // (the player slot is the game thread's: Player 1 only there)
        static int said;
        if (!said++) LOG("[coop] a DoAction component checked off the game thread: Player 1 only\n");
        return 0;
    }
    uint64_t own = LD64(gm + 0x4b8), p1 = host_player(), comp = c->x[0], args[8];
    memcpy(args, c->x, sizeof args);
    g_act_calls = 1;
    for (int i = 1; i < g_n; i++) {
        if (!collision_of(g_extra[i]) || coop_player_down(i)) continue;
        ST64(gm + 0x4b8, g_extra[i]);
        slot_swap(GA(DOACTION_PENDING), g_act[i]);
        uint64_t pend = slot_live(GA(DOACTION_PENDING));  // (his best so far: his findings in it for the comparison)
        uint8_t keep[ACT_RES];
        if (pend && pend != comp) res_put(pend, g_act_res[i][0], keep);
        memcpy(c->x, args, sizeof args);
        guest_call(c, GA(COOP_DOACTION_SITE));
        if (pend && pend != comp) res_put(pend, keep, NULL);
        if (LD64(GA(DOACTION_PENDING)) == comp)  // (his best now)
            memcpy(g_act_res[i][0], (const void *)(uintptr_t)(comp + ACT_RES_AT), ACT_RES);
        slot_swap(GA(DOACTION_PENDING), g_act[i]);
    }
    ST64(gm + 0x4b8, p1);
    memcpy(c->x, args, sizeof args);
    guest_call(c, GA(COOP_DOACTION_SITE));
    ST64(gm + 0x4b8, own);
    g_act_calls = 0;
    return 2;  // (to its ret)
}

// The frame's end for the DoAction slots (x8: where it returns its result): for each other player on his (the
// countdown it lowers only once), then as it is for Player 1.
static void doaction_frame(Cpu *c) {
    if (g_act_calls || !GA(DOACTION_CURRENT) || !on_game_thread()) return;
    uint64_t args[9];
    memcpy(args, c->x, sizeof args);
    uint32_t cool = LD32(GA(DOACTION_COOLDOWN));
    g_act_calls = 1;
    for (int i = 1; i < g_n; i++) {
        uint64_t before, now;
        memcpy(&before, g_act[i] + 16, 8);
        slot_swap(GA(DOACTION_PENDING), g_act[i]);
        slot_swap(GA(DOACTION_CURRENT), g_act[i] + 16);
        memcpy(c->x, args, sizeof args);
        guest_call(c, GA(COOP_DOACTION_FRAME_SITE));
        slot_swap(GA(DOACTION_PENDING), g_act[i]);
        slot_swap(GA(DOACTION_CURRENT), g_act[i] + 16);
        ST32(GA(DOACTION_COOLDOWN), cool);
        memcpy(g_act_res[i][1], g_act_res[i][0], ACT_RES);
        memcpy(&now, g_act[i] + 16, 8);
        if (now != before) {
            uint64_t owner = heap_ptr(now) ? LD64(now + 0x48) : 0;
            CLOG("[coop] player %d can act on %.48s (type %u)\n", i + 1,
                 heap_ptr(owner) ? (const char *)(uintptr_t)(owner + 0x18) : now ? "?" : "nothing",
                 heap_ptr(now) ? LD8(now + 0x1a0) : 0);
        }
    }
    g_act_calls = 0;
    memcpy(c->x, args, sizeof args);
}

// Another player's update: his current slot in the game's while it runs, his findings in its component.
static void act_update_begin(uint64_t actor) {
    int i = coop_index(actor);
    if (!i || g_act_swapped || !GA(DOACTION_CURRENT)) return;
    slot_swap(GA(DOACTION_CURRENT), g_act[i] + 16);
    g_act_swapped = i;
    g_act_comp = slot_live(GA(DOACTION_CURRENT));
    if (g_act_comp) res_put(g_act_comp, g_act_res[i][1], g_act_own);
}

static void act_update_end(void) {
    if (!g_act_swapped) return;
    if (g_act_comp && slot_live(GA(DOACTION_CURRENT)) == g_act_comp) res_put(g_act_comp, g_act_own, NULL);
    slot_swap(GA(DOACTION_CURRENT), g_act[g_act_swapped] + 16);
    g_act_swapped = 0;
    g_act_comp = 0;
}

// One taker per object. In his update Link checks whether he can act (main+0xd92480: the getter's call returns to
// DOACTION_CHECK_RET; nothing there: no A action), and when A is pressed his A handler asks for the action's kind
// (returning to DOACTION_TAKE_RET; it reads the component unchecked) and starts the lift / the opening / the talk.
// Two players who can act on the same rock (side by side, or one on the other) and press A in the same frame would
// both start lifting it, as any player pressing A with Player 1 did before. So a component a player's A handler
// took is nothing to the others' checks for a moment (CLAIM_FRAMES).
// Actions that play an event (a chest's opening and its item, a sign's text...: component type +0x1a0 other than
// ACT_LIFT and ACT_PULL, own_action) play it on "Link", Player 1, and the Link whose A started it waits in his "open"
// state (main+0xdd8060) for the event to move him on - for another player, never. So another player's A on one starts the component's action
// itself (DOACTION_TRIGGER, as the "open" state does), and his own Link takes no part: the scene is Player 1's.
#define CLAIM_FRAMES 30
#define ACT_PULL 3  // (levers: the grab and pull, his own state - played on Player 1 another player's A did nothing,
                    // user 2026-10-28, Angler's Tunnel)
#define ACT_LIFT 4  // (rocks, pots, bushes, bombs...: the lift, his own state)
static bool own_action(uint64_t comp) { uint8_t t = LD8(comp + 0x1a0); return t == ACT_LIFT || t == ACT_PULL; }
static struct { uint64_t comp, until; } g_claim[COOP_MAX];  // (per player)
extern uint64_t input_coop_buttons(int player);
RT_ON_RESET(coop_claim_reset) { memset(g_claim, 0, sizeof g_claim); g_talking = false, g_talker = 0; }

// Shops deal with Player 1. The shopkeeper sells what "Link" carries: Player 2 bringing him the bombs got his greeting,
// and was left holding them. So the goods (ObjShopItem) are nothing to the other players' A.
static bool shop_item(uint64_t comp) {
    uint64_t owner = LD64(comp + 0x48);
    return heap_ptr(owner) && !strncmp((const char *)(uintptr_t)(owner + 0x18), "ObjShopItem", 11);
}

static int doaction_get(Cpu *c) {
    if (!GA(DOACTION_TAKE_RET) || !on_game_thread() || !g_updating) return 0;
    bool take = c->x[30] == GA(DOACTION_TAKE_RET), check = c->x[30] == GA(DOACTION_CHECK_RET);
    int who = g_act_swapped ? g_act_swapped : g_updating == host_player() ? 0 : -1;
    uint64_t comp = slot_live(GA(DOACTION_CURRENT)), f = renderer_present_count();
    if ((!take && !check) || who < 0 || !comp) return 0;
    if (take) {
        g_claim[who].comp = comp, g_claim[who].until = f + CLAIM_FRAMES;
        if (!who && !own_action(comp)) talk_started(0, f);  // (Player 1's own talk, chest, sign)
        return 0;
    }
    bool nothing = false;
    for (int k = 0; k < COOP_MAX && !nothing; k++)
        if (k != who && g_claim[k].comp == comp && f < g_claim[k].until) {
            static uint64_t said;
            if (f > said + 30) CLOG("[coop] player %d can't act: player %d took that a moment ago\n", who + 1, k + 1);
            said = f;
            nothing = true;
        }
    if (!nothing && who > 0 && shop_item(comp)) {
        static uint64_t said;
        if (f > said + 120) CLOG("[coop] player %d: the shop's goods are Player 1's to take\n", who + 1);
        said = f;
        nothing = true;
    }
    if (!nothing && who > 0 && !own_action(comp) && GA(DOACTION_TRIGGER)) {
        if ((input_coop_buttons(who) & 1) && !(g_claim[who].comp == comp && f < g_claim[who].until)) {
            uint64_t owner = LD64(comp + 0x48);
            Cpu saved = *c;
            c->x[0] = comp;
            guest_call(c, GA(DOACTION_TRIGGER));
            bool ok = c->x[0] & 1;
            *c = saved;
            CLOG("[coop] player %d acts on %.48s (type %u)%s\n", who + 1,
                 heap_ptr(owner) ? (const char *)(uintptr_t)(owner + 0x18) : "?", LD8(comp + 0x1a0),
                 ok ? ": its event plays on Player 1" : ": not now");
            if (ok) talk_started(who, f);
            g_claim[who].comp = comp, g_claim[who].until = f + CLAIM_FRAMES;  // (and not again at once)
        }
        nothing = true;
    }
    if (!nothing) return 0;
    ST64(c->x[0], 0);  // (the empty pointer it returns for nothing, x0 its result)
    ST64(c->x[0] + 8, 0);
    return 2;  // (to its ret)
}

// ---- Event boxes. An AreaEventBox (a fight room's lock, a boss's or mini-boss's start, a cutscene's trigger) is a bare
// collision shape (main+0x68110) whose contact signal calls the box's fire (main+0x67210, through a small functor: x0
// the box, x1 the contact; its "Link entered" signal, its switch set, the box gone if it plays once) with no check of
// who touched it: any Link walking in set it off. Each other player walking into Lanmola's sand pit played his start
// again. In a fight room another player a step ahead of Player 1 set the room's switch while Player 1 was still in the
// doorway, in his walk-in: the shutter closing on him put him back on the doorway's far side, in the room before, and
// the camera and everyone followed (Angler's Tunnel, 08E up into 07E, every time). Only Player 1's contact fires a box
// (the contact as at the doors' end of contact: contact_link).
static int event_box(Cpu *c) {
    int i = contact_link(c->x[1]);
    if (i <= 0) return 0;
    static uint64_t logged_frame;
    uint64_t frame = renderer_present_count();
    if (frame > logged_frame + 30)
        CLOG("[coop] player %d in an event box (%.48s): only Player 1 sets it off\n", i + 1,
             (const char *)(uintptr_t)(c->x[0] + 0x18));
    logged_frame = frame;
    return 2;  // (to its ret)
}

// ---- Warp holes (AreaWarpHoleEventBox: main+0x8c560 stepped in - Player 1 jumps in, the "Where to?" menu -,
// main+0x8c3d0 stepped out) keep a player actor whose +0x43 bit 6 is clear, then play the jump for the game manager's
// player: another player stepping in sent Player 1 in. At those checks (x8 the player actor and the load's target)
// another player's Link reads as one with the bit set: he sets nothing off, and his stepping out doesn't drop Player
// 1. The overworld's warp holes are objects of their own with the same check (COOP_WARP_OBJ_TOUCH / LEAVE_SITE:
// main+0xcbd3e0 / main+0xcbd250; stepped in, their update walked Player 1 - and everyone on him - slowly over to the
// hole another player stood in, user 2026-10-28). (A dungeon's warp hole back to its entrance is an exit,
// COOP_EXIT_TOUCH_SITE: Player 1 already leads it.)
static int warp_hole(Cpu *c, bool touch) {
    int i = coop_index(c->x[8]);
    if (!i) return 0;
    static uint64_t logged_frame;
    uint64_t frame = renderer_present_count();
    if (touch && frame > logged_frame + 30) CLOG("[coop] player %d at a warp hole: only Player 1 jumps in\n", i + 1);
    if (touch) logged_frame = frame;
    c->x[8] = LD8(c->x[8] + 0x43) | 0x40;
    return 1;
}

// ---- Deaths. A Link's death is his state machine (+0x4e0) changing to the dead state (0x12; 0x13 is checked with
// it), whose start (main+0xdaaf00) plays the game over (Common.bfevfl "GameOver"). A hit on another player comes off
// his own hearts (rt_coop_damage), and so do a fall into a pit or deep water (DAMAGE_FIXED_SITE: back at the room's
// entry). A death that isn't his hearts running out - an instant kill - would still be the game over for everyone:
// with each player's own hearts, another player sent to the dead state is knocked out instead and his state stays.
// (With "game over for everyone" his last hit is let through; with shared hearts a death is the game's own.)
// A knocked-out player isn't knocked back again either (the change to a state of kind 11 stays undone; the hit that
// knocked him out plays out): in spikes each knockback ended on them and the next began, and moved meanwhile his state
// put him back. Each one counts as a hit while down (down_hit): lying still, he's put on Player 1.
#define PLAYER_STATE_DEAD 0x12
#define LINK_KIND_KNOCKED_BACK 11

static int dead_state(Cpu *c) {
    unsigned s = (unsigned)(c->x[1] & 0xff);
    int i = coop_index(c->x[0] - 0x4e0);
    if (!i || !own_hearts()) return 0;
    uint64_t st = g_down[i] && s < 0x40 ? LD64(g_extra[i] + 0x1340 + 8ull * s) : 0;
    if (heap_ptr(st) && LD8(st + 0x28) == LINK_KIND_KNOCKED_BACK) {
        down_hit(i);
        return 2;  // (to the change's ret)
    }
    if ((s != PLAYER_STATE_DEAD && s != PLAYER_STATE_DEAD + 1) ||
        (g_settings.coop_knockout == COOP_KO_GAME_OVER && g_hp[i] == 0))
        return 0;
    LOG("[coop] player %d sent to the dead state (%#x): knocked out instead of a game over\n", i + 1, s);
    if (!g_down[i]) knock_out(i);
    return 2;  // (to the change's ret: the state stays)
}

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
// actor's update: lifted, cut by another player's sword - with a player within GONE_TAKER of it. The screens the game
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
static void gone_forget(void) { memset(g_gone, 0, sizeof g_gone), actor_class_forget(); }
RT_ON_RESET(coop_gone_reset) { gone_forget(); }

// A grass's or rock's key: the FNV-1a of its name, its level object's id (0: not one of them).
static uint64_t gone_key(const char *name) {
    static const char *const kinds[] = {"ObjRock-", "ObjGrass-", "ObjGrass2x2-", "ObjLawn-", "ObjSkullStone-"};
    bool one = false;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds && !one && !strncmp(name, "Obj", 3); i++)
        one = !strncmp(name, kinds[i], strlen(kinds[i]));
    if (!one) return 0;
    uint64_t key = 1469598103934665603ull;
    for (int k = 0; k < 48 && name[k]; k++) key = (key ^ (uint8_t)name[k]) * 1099511628211ull;
    return key ? key : 1;
}

static int gone_find(uint64_t key) {  // its slot, or -1: not destroyed
    for (int p = 0; p < GONE_PROBES; p++)
        if (g_gone[(key + p) % GONE_SLOTS].key == key) return (int)((key + p) % GONE_SLOTS);
    return -1;
}

static void gone_keep(uint64_t key, const float at[3]) {
    if (gone_find(key) >= 0) return;
    int k = (int)(key % GONE_SLOTS);
    for (int p = 0; p < GONE_PROBES; p++) {
        int s = (int)((key + p) % GONE_SLOTS);
        if (!g_gone[s].key) { k = s; break; }
        if (g_gone[s].at_frame < g_gone[k].at_frame) k = s;
    }
    g_gone[k].key = key, g_gone[k].at_frame = renderer_present_count();
    memcpy(g_gone[k].at, at, sizeof g_gone[k].at);
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
    return view_point(at, 16.0f / 9.0f * widescreen_factor(), &nx, &ny, &front) && front && fabsf(nx) < 1.15f &&
           fabsf(ny) < 1.15f;
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

// After an actor's update: whether it was destroyed.
static void gone_note(uint64_t actor) {
    if (!(LD32(actor + 0x40) & ACTOR_DEAD) || (LD32(actor + 0x18) & 0xffffff) != ('O' | 'b' << 8 | 'j' << 16)) return;
    ActorClass *e = actor_class(actor);
    if (!e->key || e->gone == GONE_NOTED) return;
    e->gone = GONE_NOTED;
    float at[3];
    if (!actor_pos(actor, at)) return;
    if (g_gone_dead_before && !gone_near(at, GONE_TAKER)) return;  // (its screen unloaded: it comes back whole)
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

// LARECOMP_TRACK_RESPAWN=1 (diagnostic): a rock or grass actor made again under the same name - one that came back -
// logged as "[respawn]", with where it is and whether a player is near it or it's in view (gone_watched): one made
// again in view, destroyed or not, pops (cut grass and lifted rocks came back beside Player 1 in play, 2026-10-28).
static void respawn_track(uint64_t actor) {
    if (!ENV("LARECOMP_TRACK_RESPAWN") || !heap_ptr(actor)) return;
    static struct { uint64_t key, actor; } seen[8192];
    const char *nm = (const char *)(uintptr_t)(actor + 0x18);
    if (strncmp(nm, "ObjRock", 7) && strncmp(nm, "ObjGrass", 8) && strncmp(nm, "ObjLawn", 7)) return;
    uint64_t key = 1469598103934665603ull;
    for (int k = 0; k < 64 && nm[k]; k++) key = (key ^ (uint8_t)nm[k]) * 1099511628211ull;
    int k = (int)(key % 8192), probes = 0;
    while (seen[k].key && seen[k].key != key && ++probes < 8192) k = (k + 1) % 8192;
    if (seen[k].key && seen[k].key == key && seen[k].actor != actor) {
        float at[3] = {0}, p1[3] = {0};
        actor_pos(actor, at);
        player_pos(host_player(), p1);
        LOG("[respawn] %.48s again (actor %#llx, was %#llx), frame %llu, at (%.2f %.2f %.2f)%s, Player 1 %.1f away\n",
            nm, (unsigned long long)actor, (unsigned long long)seen[k].actor,
            (unsigned long long)renderer_present_count(), at[0], at[1], at[2], gone_watched(at) ? " SEEN" : "",
            sqrtf((at[0] - p1[0]) * (at[0] - p1[0]) + (at[2] - p1[2]) * (at[2] - p1[2])));
    }
    seen[k].key = key, seen[k].actor = actor;
}

// Alone (no co-op) with the zoom on, only the zoom's hooks run: the camera, the Field's boxes of screens, and on the
// Field the actors' updates for the cut grass and thrown rocks kept gone in view (zoomed out, the screen Player 1 just
// left is still in view).
static bool zoom_hook(int which) {
    return zoom_on() && (which == 17 || (which >= 33 && which <= 35) || ((which == 15 || which == 16) && zoom_field()));
}

int rt_coop_hook(Cpu *c, int which) {
    if (!rt_coop_on() && !zoom_hook(which)) return 0;
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
        if (!rt_coop_on()) {  // (alone, zoomed out: grass and rocks kept gone, nothing else - zoom_hook)
            respawn_track(g_updating);
            bool killed = heap_ptr(g_updating) && gone_check(c, g_updating);
            if (killed) g_updating = 0;
            return killed;
        }
        // (held on Player 1's spot for the pause menu: hold_on_p1; off the map after falling through it: lost_tick)
        if (pause_updated(g_updating) || lost_away(g_updating)) {
            g_updating = 0;
            return 1;
        }
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
        respawn_track(g_updating);
        if (heap_ptr(g_updating) && gone_check(c, g_updating)) {
            g_updating = 0;
            return 1;
        }
        actor_exec_begin(c);
        act_update_begin(g_updating);
        return 0;
    }
    case 16:  // COOP_ACTOR_EXEC_DONE - after it
        if (!on_game_thread()) return 0;
        if (!rt_coop_on()) {  // (alone, zoomed out: zoom_hook)
            if (heap_ptr(g_updating)) gone_note(g_updating);
            g_updating = 0;
            return 0;
        }
        act_update_end();
        actor_exec_end();
        if (heap_ptr(g_updating)) gone_note(g_updating);
        if (g_updating) coop_actor_updated(c, g_updating);
        g_updating = 0;
        return 0;
    case 17:  // COOP_CAMERA_SITE - a camera's view matrix is set (x23; x24 the renderer's copy)
        camera_seen(c->x[23]);
        camera_back(c);
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
    case 26:  // COOP_DOACTION_SITE - a DoAction component's update (x0)
        return doaction_each(c);
    case 27:  // COOP_DOACTION_FRAME_SITE - the frame's end for the DoAction slots
        doaction_frame(c);
        return 0;
    case 28:  // COOP_DOACTION_GET_SITE - Link asks for his current DoAction (x0: where it returns it)
        return doaction_get(c);
    case 29:  // COOP_EVENT_BOX_SITE - str x19, [sp, #-0x20]!: an event box (x0) fires for a contact (x1)
        return event_box(c);
    case 30:  // COOP_WARP_HOLE_TOUCH_SITE - ldrb w8, [x8, #0x43]: a player actor (x8) stepped into a warp hole
        return warp_hole(c, true);
    case 31:  // COOP_WARP_HOLE_LEAVE_SITE - ldrb w8, [x8, #0x43]: ... stepped out of it
        return warp_hole(c, false);
    case 32:  // COOP_DEAD_STATE_SITE - sub sp, sp, #0x30: a state machine (x0) changes to state w1
        return dead_state(c);
    case 33:  // COOP_LOAD_BOX_SITE - adrp x23: s1 x s2 the box of screens whose objects are made
        if (zoom_field() && g_box != 1.0f) scale_s(c, 1, g_box), scale_s(c, 2, g_box);
        return 0;
    case 34:  // COOP_KEEP_BOX_SITE - ld1 {v0.s}[1], [x23]: s0, s1 (across) and s3, s2 (deep) the half sizes of the
              // box of screens whose objects are kept
        if (zoom_field() && g_box != 1.0f)
            for (int r = 0; r < 4; r++) scale_s(c, r, g_box);
        return 0;
    case 35:  // COOP_ACTIVE_BOX_SITE - fsub s8, s0, s3: s3 x s4 the half sizes of the box of active screens, s5 its
              // offset deep - grown to take in every player
        if (zoom_field()) active_box(c);
        return 0;
    case 36:  // COOP_WARP_OBJ_TOUCH_SITE - ldrb w8, [x8, #0x43]: a player actor (x8) stepped into a warp hole object
        return warp_hole(c, true);
    case 37:  // COOP_WARP_OBJ_LEAVE_SITE - ldrb w8, [x8, #0x43]: ... stepped out of it
        return warp_hole(c, false);
    }
    return 0;
}

// The mod's tunic colours need its Playerlink.bfres (24 colour frames: 3 per player): served from
// <data root>/coop/romfs while co-op is on (runtime/services/fs.c).
static int g_coop_dir = -1;  // (<data root>/coop/romfs there: looked at once a boot, the romfs index asks per file)
static int g_colours = -1;   // (the colour file served: fixed for the boot, as the romfs index)
static bool g_indexed;       // (this boot's romfs index is made: the game has started)
RT_ON_RESET(coop_files_reset) { g_coop_dir = g_colours = -1, g_indexed = false; }

bool coop_romfs_override(const char *path, char *out, size_t cap) {
    g_indexed = true;
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
// is made as it boots, again after Quit to Start Screen): otherwise the game's own frames stay - every Link in Player
// 1's tunic - until then.
bool coop_colours(void) { return g_colours > 0; }

// For the menu: 0 the colours are in (or come as the game starts: not started yet), 1 they come once the game starts
// again (co-op turned on since it started), 2 there is no coop/romfs Playerlink.bfres.
const char *coop_colours_path(void);
int coop_colours_state(void) {
    if (g_colours > 0) return 0;
    struct stat sb;
    return stat(coop_colours_path(), &sb) != 0 ? 2 : g_indexed ? 1 : 0;
}

// Where the colour file goes, for the menu's "Not installed" note: <data root>/coop/romfs/region_common/actor/.
const char *coop_colours_path(void) {
    extern const char *rt_data_root(void);
    static char p[1200];
    const char *root = rt_data_root();
    snprintf(p, sizeof p, "%s/coop/romfs/region_common/actor/Playerlink.bfres", root ? root : ".");
    return p;
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

// Every level change in play (the level-jump request processor: runtime/hle/chamber.c rt_level_request) is logged
// with where it goes and who was where, in what state: an area reloaded in play - its grass and rocks back - shows
// what led to it.
void coop_level_change(const char *level, const char *locator) {
    extern int game_in_play(void);
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    gone_forget();  // (a new level: everything comes back, as without co-op)
    if (!rt_coop_on() || !game_in_play()) return;
    const char *lv = chaos_current_level();
    char line[600];
    int k = snprintf(line, sizeof line, "[coop] level change in %s to %.32s / %.32s, frame %llu:", lv && *lv ? lv : "?",
                     level, locator, (unsigned long long)renderer_present_count());
    for (int i = 0; i < g_n && k < (int)sizeof line - 64; i++) {
        float p[3];
        if (!player_pos(coop_player(i), p)) k += snprintf(line + k, sizeof line - k, " P%d none", i + 1);
        else
            k += snprintf(line + k, sizeof line - k, " P%d (%.1f %.1f %.1f) kind %d%s", i + 1, p[0], p[1], p[2],
                          link_state_kind(coop_player(i)), i && g_down[i] ? " down" : "");
    }
    LOG("%s\n", line);
}

// Once per frame on the game thread (runtime/hle/input.c, npad 1's poll).
void coop_tick(Cpu *c) {
    g_game_thread = pthread_self(), g_game_thread_known = true;
    camera_back_undo();
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
    if (g_p1_walk_in && !g_talking)  // (a talk walks Player 1 up to the one he talks to, a walk-in too: coop_enter)
        for (int i = 1; i < g_n; i++)
            if (collision_of(g_extra[i])) ride_on_host(i);
    pass_tick();
    {   // LARECOMP_COOP_MOVE=<frame>:<player>:<x>:<y>:<z>[:hold] (test): that player put there at that frame (left far
        // away); with :hold, kept there from then on
        static int done;
        unsigned long at;
        int i;
        float v[3];
        char hold[8] = "";
        uint64_t p;
        if (ENV("LARECOMP_COOP_MOVE") &&
            sscanf(ENV("LARECOMP_COOP_MOVE"), "%lu:%d:%f:%f:%f:%7s", &at, &i, &v[0], &v[1], &v[2], hold) >= 5 &&
            (!done || !strcmp(hold, "hold")) && renderer_present_count() >= at && i > 1 && i <= g_n &&
            (p = collision_of(g_extra[i - 1]))) {
            if (!done) LOG("[coop] test: player %d put at (%.2f %.2f %.2f)%s\n", i, v[0], v[1], v[2], hold);
            done = 1;
            memcpy((void *)(uintptr_t)(p + 0x80), v, sizeof v);
        }
    }
    hearts_tick(c);
    sleep_tick(c);
    if (play) down_move_tick(c), pause_tick(), lost_tick(), leash_tick(), room_follow_tick();
    extern uint64_t input_coop_buttons(int player);
    uint64_t frame = renderer_present_count();
    static int test_die;  // LARECOMP_COOP_DIE=<player>,... (test): those players sent to the dead state 3 s after they
                          // are placed, as an instant kill does
    if (!test_die && g_placed_frame && frame > g_placed_frame + 180 && ENV("LARECOMP_COOP_DIE"))
        for (const char *p = ENV("LARECOMP_COOP_DIE"); (test_die = 1) && *p; p += *p == ',') {
            uint64_t link = coop_player((int)strtol(p, (char **)&p, 10) - 1);
            if (collision_of(link)) {
                LOG("[coop] test: Link %#llx (player %d) sent to the dead state\n", (unsigned long long)link,
                    coop_index(link) + 1);
                Cpu saved = *c;
                c->x[0] = link + 0x4e0, c->x[1] = PLAYER_STATE_DEAD;
                guest_call(c, GA(COOP_DEAD_STATE_SITE));
                *c = saved;
            }
            if (!*p) break;
        }
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
        if (g_zoom != 1.0f) snprintf(line + k, sizeof line - k, " zoom %.3f", g_zoom);
        LOG("%s\n", line);
    }
}
