// Local co-op, each player's hearts, hits, knock-outs, revives, the sleeping pose. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// ---- Hearts (setting coop_health). Shared: the game's one set, as without co-op. Own: Player 1 keeps the game's
// (PlayerStatus +0x84, quarter hearts; its HUD), players 2.. have their own here. A hit lands on the Link whose state
// code computes it (rt_coop_damage_of, from tools/patches.py DAMAGE_*_SITES): another player's comes off his hearts and
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
// A knocked-out player's "(A) Revive" prompt is on him (below him), while a player who is up is beside him and nobody
// holds A there - not on the one who can revive him, where it went along as he walked about (user 2026-10-31).
static bool g_can_revive[COOP_MAX];  // knocked-out players with a player who is up beside them, not holding A
// A player holding A beside a knocked-out one stands still (coop_reviving: runtime/hle/input.c). A hit on him stops
// the revive: the knocked-out ones he held A beside lose their progress, and his A revives again only once he has let
// go of it (revive_hit; user 2026-10-31).
static uint32_t g_reviving[COOP_MAX];  // (whom he holds A beside: a bit per player)
static bool g_revive_hit[COOP_MAX];    // (hit while reviving, A not let go since)
// Beside a knocked-out player, a player's A is the revive's: the game doesn't get it (coop_revive_reach:
// runtime/hle/input.c), and the revive reads his A as held on his controller. An A the revive took stays its own until
// it is let go (g_revive_a): the revive done, or the player stepped away, the game would take it for a new press - a
// boulder beside him lifted the moment the revive ended (2026-11-14). Until 2026-11-14 the game got it all along: held
// beside a rock, it lifted the rock as the revive went on.
static bool g_revive_reach[COOP_MAX], g_revive_a[COOP_MAX];
static int g_game_hp = -1;       // the game's hearts last frame (for what it gained)
static int g_max_hp;             // the maximum (main+0xdf8260, every half second and whenever the hearts change)
static int g_heal_pending;       // hearts healed this frame before the game's maximum capped them (COOP_HEAL_SITE_*)

static void hearts_forget(void) {  // (a new boot / back at the title: everyone starts the next file at full hearts)
    for (int i = 0; i < COOP_MAX; i++) g_hp[i] = -1, g_down[i] = false, g_revive[i] = 0, g_safe_until[i] = 0;
    memset(g_reviving, 0, sizeof g_reviving);
    memset(g_revive_hit, 0, sizeof g_revive_hit);
    memset(g_revive_reach, 0, sizeof g_revive_reach);
    memset(g_revive_a, 0, sizeof g_revive_a);
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
// knocked out, where he can be revived. Only hits that would cost him hearts count (rt_coop_damage_of): spikes hit every
// 30 to 60 frames. A knockback with nothing off (the others' swords, a shove) - cancelled for him (dead_state) - and
// hits seconds apart (a Spark running along the wall past two knocked-out players) moved players lying out of harm's
// way onto Player 1, in the middle of his fight, and again from there (user 2026-11-01, Catfish's Maw: "still moving
// down-but-not-out players around").
#define DOWN_HITS 3
#define DOWN_HIT_FRAMES 90  // (1.5 s)
static uint64_t g_down_hit_at[COOP_MAX];
static int g_down_hits[COOP_MAX];
static bool g_down_move[COOP_MAX];
static uint64_t g_down_moved_until[COOP_MAX];  // (just put on Player 1: hits don't count - the spikes' last contact
                                               // came after him, and he was put on Player 1 again and again)
// Nor do hits with an enemy near him (down_enemy_note): its blows, which do him nothing (no hearts, no knockback).
// Put on Player 1, he was put by the enemy Player 1 was fighting, hit again - the Armos Knight's knocked-out players
// went back and forth every few seconds (user 2026-10-30).
#define DOWN_ENEMY_NEAR 8.0f
#define DOWN_ENEMY_FRAMES 30
static uint64_t g_down_enemy_at[COOP_MAX];

static void down_hit(int i) {
    uint64_t now = renderer_present_count();
    if (now < g_down_moved_until[i] || now < g_down_enemy_at[i] + DOWN_ENEMY_FRAMES) return;
    g_down_hits[i] = now - g_down_hit_at[i] < DOWN_HIT_FRAMES ? g_down_hits[i] + 1 : 1;
    g_down_hit_at[i] = now;
    if (g_down_hits[i] < DOWN_HITS || g_down_move[i]) return;
    g_down_hits[i] = 0, g_down_move[i] = true;
}

// Player 1 in an event - walked by a cutscene or a door, an item held up, a scene - while the other players stand
// still (their input is none: runtime/hle/input.c). His current state (+0x4ed the slot, +0x1340 the states, +0x28 the
// kind) is one the game's own "can he go" check (main+0xd58d70) holds apart: kinds 18, 19, 21, 22, 23, 26 (its mask
// 0x4ec0000; the check also looks at more than the kind for some others - 1, 3, 11 - which this doesn't).
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

static void revive_hit(int j) {
    if (!g_reviving[j]) return;
    for (int i = 1; i < COOP_MAX; i++)
        if (g_reviving[j] >> i & 1 && g_revive[i] > 0) {
            CLOG("[coop] player %d hit while reviving player %d: the revive stops (%.0f%% lost)\n", j + 1, i + 1,
                 g_revive[i] * 100);
            g_revive[i] = 0;
        }
    g_reviving[j] = 0, g_revive_hit[j] = true;
}

// A hit of `dmg` quarter hearts (after the difficulty setting, and an enemy's after the game halves it: the Blue
// Clothes, a Guardian Acorn - COOP_DAMAGE_HALVED_SITE) about to come off the game's hearts. (Taken at the difficulty site
// before that, Player 1's hits were halved and the others' weren't: user 2026-11-02, "P1 takes hardly any damage,
// and other players take full damage".)
static bool g_hit_own;  // (the hit just taken was another player's, off his own hearts: COOP_DAMAGE_HALVED_SITE)
// The Link hit, where the game's code holds him (tools/patches.py): the enemy hit's function x21, the fixed hits' (a
// pit, deep water) x19, the other Link hit's (main+0xd96830) x20; 0 for the event-scripted damage (main+0xd7c870, an
// event's command, no Link): Player 1's, as every event plays on him. (Until 2026-11-13 the Link was found by scanning
// the callee-saved registers for one: upstream's review.)
static int32_t coop_damage(Cpu *c, int32_t dmg, uint64_t link);
int32_t rt_coop_damage_of(Cpu *c, int32_t dmg, uint64_t link) { return coop_damage(c, dmg, link); }
static int32_t coop_damage(Cpu *c, int32_t dmg, uint64_t link) {
    (void)c;
    g_hit_own = false;
    if (!own_hearts() || dmg <= 0) return dmg;
    int i = coop_index(link);  // (Player 1's, or a Link that is no player's: 0)
    if (g_down[i]) {
        CLOG("[coop] player %d hit for %d: knocked out, unhurt\n", i + 1, dmg);
        down_hit(i);
        return 0;
    }
    if (i && renderer_present_count() < g_safe_until[i]) {
        CLOG("[coop] player %d hit for %d: just revived, unhurt\n", i + 1, dmg);
        return 0;
    }
    revive_hit(i);  // (with infinite hearts too: the hit still lands)
    if (!i) {
        CLOG("[coop] player 1 hit for %d\n", dmg);
        return dmg;
    }
    if (g_settings.cheat_hearts) return g_hit_own = true, 0;
    if (g_hp[i] < 0) g_hp[i] = g_max_hp > 0 ? g_max_hp : 12;
    g_hp[i] -= dmg;
    CLOG("[coop] player %d hit for %d (%d left)\n", i + 1, dmg, g_hp[i]);
    if (g_hp[i] > 0) return g_hit_own = true, 0;
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
            char *end;
            int i = (int)strtol(p, &end, 10) - 1;
            if (end == p) break;  // (not a number: the rest is ignored)
            p = end;
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
    // Revives: another player who is up, within 1.6 of him (height too), holding A. Not during the pause menu's hold:
    // everyone is on Player 1's spot then - A held in the menu revived a player lying far away.
    extern uint64_t input_coop_held(int player);
    extern uint64_t input_buttons_now(void);
    float step = 1.0f / (60.0f * (float)(g_settings.coop_revive_s > 0 ? g_settings.coop_revive_s : 4));
    memset(g_can_revive, 0, sizeof g_can_revive);
    memset(g_reviving, 0, sizeof g_reviving);
    memset(g_revive_reach, 0, sizeof g_revive_reach);
    for (int j = 0; j < g_n; j++)
        if (g_revive_hit[j] && !((j ? input_coop_held(j) : input_buttons_now()) & 1)) g_revive_hit[j] = false;
    for (int i = 1; i < g_n; i++) {
        if (!g_down[i]) continue;
        float pi[3];
        bool helped = false, near = false;
        if (player_pos(g_extra[i], pi))
            for (int j = 0; j < g_n; j++) {
                float pj[3];
                if (j == i || (j && g_down[j]) || !player_pos(coop_player(j), pj)) continue;
                // (height too: in a side view a player swimming above him was "beside" him - the Angler Fish, user
                // 2026-10-28)
                float dx = pj[0] - pi[0], dy = pj[1] - pi[1], dz = pj[2] - pi[2];
                if (dx * dx + dy * dy + dz * dz >= 1.6f * 1.6f) continue;
                near = true, g_revive_reach[j] = true;
                uint64_t b = j ? input_coop_held(j) : input_buttons_now();
                if (!(b & 1) || g_revive_hit[j]) continue;  // (bit 0: A)
                helped = true;
                g_reviving[j] |= 1u << i;
            }
        g_can_revive[i] = near && !helped;  // (his prompt: runtime/ui/coop_hud.mm)
        g_revive[i] = helped ? g_revive[i] + step : g_revive[i] > step ? g_revive[i] - step : 0;
        if (g_revive[i] >= 1) revive(i, "by another player");
    }
    for (int j = 0; j < g_n; j++) {  // (an A pressed beside a knocked-out player: the revive's until let go)
        bool a = (j ? input_coop_held(j) : input_buttons_now()) & 1;
        g_revive_a[j] = a && (g_revive_a[j] || g_revive_reach[j]);
    }
}
