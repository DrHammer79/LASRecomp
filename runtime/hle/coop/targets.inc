// Local co-op, enemy targets: whom each enemy goes for, "Link" for the game's reads (rt_coop_player), actors told apart. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

// ---- Enemy targets (setting coop_targets). Enemies go after "Link": the game manager's player (+0x4b8). While an
// enemy's per-frame update runs (COOP_ACTOR_EXEC_*: actors are told apart by their level name at +0x18,
// "EnemyOctorok-<key>", bosses too), that is the Link it targets; Player 1's again after. The game has no "attacking"
// state common to its enemies, so an enemy close to its target (TARGET_ENGAGE) counts as fighting him: then it stays
// on him until he is knocked out - a charge, a wind-up, a boss's pattern plays out on one player. Otherwise it keeps
// a target TARGET_LOCK frames (and TARGET_GRACE after a fight: a charge carries it past him), then takes another only
// when he is clearly nearer (1.5x); before that only when its target is far off and another player is at it.
// Fighting one, it still turns to another player right at it (TARGET_MELEE) and clearly nearer, once it has kept
// its target TARGET_LOCK frames: in a room smaller than TARGET_ENGAGE (the Armos Knight's) everyone was always "close",
// and the knight kept the player he picked first - in the doorway he can't step into, hopping on the floor's edge,
// while the three players hitting him went ignored (user 2026-10-30; build/armos_fight.sh). A player away from the
// map for a moment (below it, y -1000: fallen through it, being moved) isn't one to go for.
#define TARGET_SLOTS 256
#define TARGET_LOCK 360       // (6 s)
#define TARGET_GRACE 120      // (2 s)
#define TARGET_ENGAGE 6.0f    // (about 4 tiles: a Moblin throws its spear from there)
#define TARGET_MELEE 2.5f     // (right at it: a sword's reach)
#define TARGET_FAR 30.0f      // (about two screens)
static struct { uint64_t actor, until, seen, since; int who; } g_target[TARGET_SLOTS];

static void targets_forget(void) {
    memset(g_target, 0, sizeof g_target);
    g_as_on = false, atomic_store_explicit(&g_as_link, 0, memory_order_relaxed);
}
RT_ON_RESET(coop_targets_reset) { targets_forget(); }

// (an actor's main component +0x80: its position at +0x80. Not every actor has one there: an update's "actor" at
// Animal Village held 1.0f at +0x80, and following it stopped the game - 2026-11-01, a test run)
static bool actor_pos(uint64_t actor, float out[3]) {
    uint64_t p = heap_ptr(actor) ? LD64(actor + 0x80) : 0;
    if (!heap_ptr(p)) return false;
    memcpy(out, (const void *)(uintptr_t)(p + 0x80), 12);
    return true;
}

static int nearest_player(uint64_t enemy) {  // (who is nearest it, within TARGET_FAR; 0: Player 1, or nobody near)
    float e[3], p[3], bd = TARGET_FAR * TARGET_FAR;
    int best = 0;
    if (!actor_pos(enemy, e)) return 0;
    for (int i = 0; i < g_n; i++) {
        if ((i && g_down[i]) || !player_pos(coop_player(i), p) || p[1] < -500.0f) continue;
        float d = (p[0] - e[0]) * (p[0] - e[0]) + (p[2] - e[2]) * (p[2] - e[2]);
        if (d < bd) bd = d, best = i;
    }
    return best;
}

// An enemy's slot: the top bits of a multiplicative hash (actors sit on 4 KB boundaries), a short probe; when that is
// full, the entry seen longest ago goes. *cur: the target it keeps (-1: none - an entry not seen for a while is an
// earlier enemy's: the game reuses actors' memory).
static uint32_t target_slot(uint64_t enemy, uint64_t frame, int *cur) {
    uint32_t h = (uint32_t)(((enemy >> 12) * 0x9E3779B97F4A7C15ull) >> 56) % TARGET_SLOTS, old = h;
    for (int k = 0; k < 8; k++) {
        uint32_t s = (h + k) % TARGET_SLOTS;
        if (g_target[s].actor == enemy || !g_target[s].actor) { old = s; break; }
        if (g_target[s].seen < g_target[old].seen) old = s;
    }
    h = old;
    *cur = g_target[h].actor == enemy && frame - g_target[h].seen < 30 ? g_target[h].who : -1;
    g_target[h].actor = enemy, g_target[h].seen = frame;
    return h;
}

// Each player's distance from an enemy across (squared; 1e30: knocked out, or away from the map); the nearest one.
static int player_dists(const float e[3], float d[COOP_MAX], float *bd) {
    float p[3];
    int best = -1;
    *bd = 1e30f;
    for (int i = 0; i < g_n; i++) {
        d[i] = 1e30f;
        if ((i && g_down[i]) || !player_pos(coop_player(i), p) || p[1] < -500.0f) continue;
        d[i] = (p[0] - e[0]) * (p[0] - e[0]) + (p[2] - e[2]) * (p[2] - e[2]);
        if (d[i] < *bd) *bd = d[i], best = i;
    }
    return best;
}

static int kept_target(uint64_t enemy) {  // (the target enemy_target keeps for it, read only; -1: none)
    uint32_t h = (uint32_t)(((enemy >> 12) * 0x9E3779B97F4A7C15ull) >> 56) % TARGET_SLOTS;
    for (int k = 0; k < 8; k++) {
        uint32_t s = (h + k) % TARGET_SLOTS;
        if (g_target[s].actor == enemy) return renderer_present_count() - g_target[s].seen < 30 ? g_target[s].who : -1;
        if (!g_target[s].actor) break;
    }
    return -1;
}

static int enemy_target(uint64_t enemy) {
    float e[3];
    if (!actor_pos(enemy, e)) return 0;
    float d[COOP_MAX], bd;
    int best = player_dists(e, d, &bd);
    if (best < 0 || bd > TARGET_FAR * TARGET_FAR) return 0;  // (far from everyone: off screen, nothing to keep)
    uint64_t frame = renderer_present_count();
    int cur;
    uint32_t h = target_slot(enemy, frame, &cur);
    float cd = cur >= 0 && cur < g_n ? d[cur] : 1e30f, engage = TARGET_ENGAGE * TARGET_ENGAGE;
    if (cur >= 0 && cd < 1e30f) {  // (its target is up)
        bool clearly = cd > bd * 2.25f;
        if (cd <= engage && !(clearly && bd <= TARGET_MELEE * TARGET_MELEE && frame >= g_target[h].since + TARGET_LOCK)) {
            if (g_target[h].until < frame + TARGET_GRACE) g_target[h].until = frame + TARGET_GRACE;  // fighting him
            return cur;
        }
        if (cd > engage && (frame < g_target[h].until ? !(clearly && bd <= engage && cd > 4 * engage) : !clearly))
            return cur;
    }
    if (best != cur && cur >= 0)
        CLOG("[coop] %.32s: P%d (%.1f away) -> P%d (%.1f)\n", (const char *)(uintptr_t)(enemy + 0x18), cur + 1,
             cd < 1e30f ? sqrtf(cd) : -1.0f, best + 1, sqrtf(bd));
    g_target[h].who = best, g_target[h].until = frame + TARGET_LOCK, g_target[h].since = frame;
    return best;
}

// A Zol or a Gel (AC_ENEMY_NEAREST) hops a set length at its target: hopping from right on him it lands past him, by a
// player beside him - the nearest by then, so it went for him (user 2026-10-31: on Player 2, "they still teleport to
// player 1"; build/zol.sh). It goes for the player it is on: one it lands on (within CLING_REACH across, on the ground
// with him) is its target, kept while he's up and within TARGET_ENGAGE of it; then one clearly nearer.
#define CLING_REACH 0.5f
static int clinger_target(uint64_t enemy) {
    float e[3], p[3];
    if (!actor_pos(enemy, e)) return 0;
    float d[COOP_MAX], bd, on = CLING_REACH * CLING_REACH;
    int best = player_dists(e, d, &bd), t = -1;
    if (best < 0 || bd > TARGET_FAR * TARGET_FAR) return 0;
    for (int i = 0; i < g_n; i++)
        if (d[i] < on && player_pos(coop_player(i), p) && fabsf(p[1] - e[1]) < 0.3f) on = d[i], t = i;
    uint64_t frame = renderer_present_count();
    int cur;
    uint32_t h = target_slot(enemy, frame, &cur);
    bool landed = t >= 0;  // (further off, another player has to be clearly nearer: 1.5x, as enemy_target)
    if (!landed)
        t = cur >= 0 && cur < g_n && d[cur] < 1e30f && (d[cur] <= TARGET_ENGAGE * TARGET_ENGAGE || d[cur] <= bd * 2.25f)
                ? cur
                : best;
    if (t != cur && cur >= 0)
        CLOG("[coop] %.32s: P%d -> P%d (%s)\n", (const char *)(uintptr_t)(enemy + 0x18), cur + 1, t + 1,
             landed ? "landed on him" : "the nearest");
    g_target[h].who = t;
    return t;
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

// What an actor is, by the type it was made as - kept as it is made (COOP_ACTOR_TYPE_SITE: actor_made) - or, for an
// enemy's part, its class. Until 2026-11-14 it was worked out from the actor's name (prefixes and "React", "PlayerComp"...):
// upstream's review - a kind with an unexpected name went to Player 1.
// An enemy's components update on their own (the owner at +0x48). The one it reacts to the player with - its life and
// reaction component (ENEMY_LIFE_VTABLE: "Zombie:ReactionComp", "ZolRed:EnemyReactionCom"... one class) - keeps "Link"
// (Zombie:ReactionComp +0x278: Player 1's) - a zombie walked to Player 1 whoever it targeted (tested 2026-10-28) - and
// so does its effect player (TailorPlayer: "<kind>:TailorPlayerComp", "ActorTailorPlayer" - its "Link" read,
// main+0xe4ffa4, compares Link's zone, +0xb0, with its owner's: whether its effects play): these see its target as
// "Link" too (AC_ENEMY_PART). Not the others (collision, animation...): Player 1's sword hits it whoever it targets.
// (A part counts only for an owner that is an enemy or a trap.)
// A Gel (EnemyMiniZol) goes for the player it is on, not one it was after (AC_ENEMY_NEAREST: clinger_target): one that
// jumped at its target and landed on another player clung to him, then went over to its target (user 2026-10-28). So
// do the Zols, red and green, which cling as well: one hopping after Player 1 landed on Player 2 on the way, latched
// on, and was on Player 1 the next moment (user 2026-10-30, red ones: "latches to p2 and then teleports to p1").
// A floor that crumbles under Link (AC_FLOOR: the rattling floor, a house's collapse tiles...) looks for "Link" on it:
// another player standing on it is its "Link" for its update, while Player 1 isn't (not checked in the game). The
// dungeons' collapse tiles measure the player in a job of their own instead: tile_player.
// The kinds by type come from the game's own actor table (ACTOR_TYPES: 0x58 bytes a type, +0x50 its name), read once:
// its enemy types ("Enemy..."), and the few kinds named below.
enum { AC_OTHER, AC_ENEMY, AC_ENEMY_NEAREST, AC_ENEMY_PART, AC_FLOOR, AC_TRAP };
#define AT_TYPES 1024
#define AT_SETS 32768
#define AT_WAYS 4
static uint8_t g_type_kind[AT_TYPES];
static int g_types_n = -1, g_type_likelike = -1, g_type_shop_item = -1;
typedef struct { _Atomic uint64_t actor; uint64_t key; uint16_t type; bool part; } ActorType;
#define AT_PART 0x10000  // (actor_type: an enemy's part - a reaction or effect player component)
static ActorType g_at[AT_SETS][AT_WAYS];
static bool g_at_used;
static pthread_mutex_t g_at_lock = PTHREAD_MUTEX_INITIALIZER;  // (writers: actors are made on loading threads too)
static void actor_types_forget(void) {
    pthread_mutex_lock(&g_at_lock);
    if (g_at_used) memset(g_at, 0, sizeof g_at);
    g_at_used = false;
    pthread_mutex_unlock(&g_at_lock);
}
RT_ON_RESET(coop_types_reset) { actor_types_forget(), g_types_n = -1, g_type_likelike = g_type_shop_item = -1; }

static void actor_types_read(void) {
    static const struct { const char *name; uint8_t kind; } k[] = {
        {"EnemyZolGreen", AC_ENEMY_NEAREST},      {"EnemyZolRed", AC_ENEMY_NEAREST},
        {"EnemyMiniZol", AC_ENEMY_NEAREST},       {"ObjHouseCollapseTile", AC_FLOOR},
        {"ObjKataKataFloor", AC_FLOOR},           {"ObjMatFaceFallFloor", AC_FLOOR},
        {"ObjBrokenFloorLv5", AC_FLOOR},          {"ObjBladeTrap", AC_TRAP}};
    g_types_n = 0;
    // (some types have no entry - no name -, up to a few in a row; 40 in a row: past the table's end)
    for (int t = 0, gap = 0; t < AT_TYPES && GA(ACTOR_TYPES) && gap < 40; t++) {
        uint64_t nm = LD64(GA(ACTOR_TYPES) + 0x58 * (uint64_t)t + 0x50);
        const char *s = (const char *)(uintptr_t)nm;
        g_type_kind[t] = AC_OTHER;
        if (nm < MAIN_BASE || nm >= GA(MAIN_IMAGE_SCAN_END) || s[0] < 'A' || s[0] > 'z') {
            gap++;
            continue;
        }
        gap = 0, g_types_n = t + 1;
        g_type_kind[t] = !strncmp(s, "Enemy", 5) ? AC_ENEMY : AC_OTHER;
        for (size_t i = 0; i < sizeof k / sizeof *k; i++)
            if (!strcmp(s, k[i].name)) g_type_kind[t] = k[i].kind;
        if (!strcmp(s, "EnemyLikeLike")) g_type_likelike = t;
        if (!strcmp(s, "ObjShopItem")) g_type_shop_item = t;
    }
    LOG("[coop] %d actor types read (Like Like %d, shop item %d)\n", g_types_n, g_type_likelike, g_type_shop_item);
}

static ActorType *actor_set(uint64_t actor) {  // (a multiplicative hash's top bits: actors sit on 4 KB boundaries)
    return g_at[(uint32_t)(((actor >> 4) * 0x9E3779B97F4A7C15ull) >> 49) & (AT_SETS - 1)];
}

// A part is made: the reaction component's base class's constructor (main+0x12fae0, from the enemies', the objects' -
// a Blade Trap's - and the NPCs': COOP_PART_SITE_0) or an effect player's (built in place: COOP_PART_SITE_1 / _2),
// each as it stores its class; kept until their destructors (main+0xeb410, which the enemies' reaction component's
// ends in; main+0xe3b510: COOP_PART_GONE_SITE_0 / _1). Only one whose owner (+0x48) is an enemy or a trap counts.
// An actor is made: the actor base class's constructor (main+0x1bdb0, from every actor class's - however it is made: the
// factory's, a mini-boss its builder wakes later, a Link) copies its spawn info's key and type into it (+0x98, +0xa0:
// COOP_ACTOR_TYPE_SITE, as it stores the type). Kept until the actor base class's destructor (main+0xfa7970:
// COOP_ACTOR_GONE_SITE, actor_gone). (Entries are written whole before their actor is: read without the lock.)
static void actor_made(uint64_t actor, uint64_t key, uint32_t type, bool part) {
    if (!heap_ptr(actor)) return;
    pthread_mutex_lock(&g_at_lock);
    ActorType *set = actor_set(actor), *e = NULL;
    for (int w = 0; w < AT_WAYS && !e; w++)
        if (atomic_load_explicit(&set[w].actor, memory_order_relaxed) == actor) e = &set[w];
    for (int w = 0; w < AT_WAYS && !e; w++)
        if (!atomic_load_explicit(&set[w].actor, memory_order_relaxed)) e = &set[w];
    if (!e) {
        static int said;
        if (!said++) LOG("[coop] more than %d actors in one set of the actor table: one forgotten\n", AT_WAYS);
        e = &set[0];
    }
    atomic_store_explicit(&e->actor, 0, memory_order_release);
    e->key = key, e->type = (uint16_t)type, e->part = part;
    atomic_store_explicit(&e->actor, actor, memory_order_release);
    g_at_used = true;
    pthread_mutex_unlock(&g_at_lock);
}

static void actor_gone(uint64_t actor) {
    pthread_mutex_lock(&g_at_lock);
    ActorType *set = actor_set(actor);
    for (int w = 0; w < AT_WAYS; w++)
        if (atomic_load_explicit(&set[w].actor, memory_order_relaxed) == actor)
            atomic_store_explicit(&set[w].actor, 0, memory_order_release);
    pthread_mutex_unlock(&g_at_lock);
}

static int actor_type(uint64_t actor) {  // (-1: not an actor made since co-op was on)
    ActorType *set = actor_set(actor);
    for (int w = 0; w < AT_WAYS; w++) {
        if (atomic_load_explicit(&set[w].actor, memory_order_acquire) != actor) continue;
        uint64_t key = set[w].key;
        uint16_t type = set[w].type;
        bool part = set[w].part;
        // (the entry not rewritten meanwhile, and an actor still that actor: its own key and type)
        if (atomic_load_explicit(&set[w].actor, memory_order_acquire) != actor ||
            (!part && (LD64(actor + 0x98) != key || LD16(actor + 0xa0) != type)))
            return -1;
        return part ? AT_PART : type;
    }
    return -1;
}

static uint8_t actor_kind(uint64_t actor) {
    if (g_types_n < 0) actor_types_read();
    int t = actor_type(actor);
    return t == AT_PART ? AC_ENEMY_PART : t >= 0 && t < g_types_n ? g_type_kind[t] : AC_OTHER;
}

// LARECOMP_COOP_TRACE=<name prefix>[,<another>...] (test): each such actor's position every half second
// (LARECOMP_COOP_TRACE_EVERY: every that many frames), and the player it goes for whenever that changes
// (actor_exec_begin). An enemy and its parts: "EnemyZolRed,ZolRed:".
static bool traced(uint64_t actor) {
    const char *pre = ENV("LARECOMP_COOP_TRACE"), *nm = (const char *)(uintptr_t)(actor + 0x18);
    while (pre && *pre) {
        size_t n = strcspn(pre, ",");
        if (n && !strncmp(nm, pre, n)) return true;
        pre += n + (pre[n] == ',');
    }
    return false;
}
static bool trace_pos(uint64_t actor, float out[3]) {  // (a part's: its owner's, +0x48)
    uint64_t owner = strchr((const char *)(uintptr_t)(actor + 0x18), ':') ? LD64(actor + 0x48) : actor;
    return heap_ptr(owner) && actor_pos(owner, out);
}
static void trace_actor(uint64_t actor) {
    const char *every = ENV("LARECOMP_COOP_TRACE_EVERY");
    if (!ENV("LARECOMP_COOP_TRACE") || renderer_present_count() % (every && atoi(every) > 0 ? atoi(every) : 30) ||
        !traced(actor))
        return;
    float p[3];
    if (trace_pos(actor, p))
        LOG("[trace] frame %llu %.40s (%.2f %.2f %.2f)\n", (unsigned long long)renderer_present_count(),
            (const char *)(uintptr_t)(actor + 0x18), p[0], p[1], p[2]);
}

static void down_enemy_note(uint64_t actor) {  // (an enemy updating: the knocked-out players near it - down_hit)
    bool any = false;
    for (int i = 1; i < g_n && !any; i++) any = g_down[i];
    float e[3], p[3];
    if (!any) return;
    uint8_t what = actor_kind(actor);
    if ((what != AC_ENEMY && what != AC_ENEMY_NEAREST) || !actor_pos(actor, e)) return;
    for (int i = 1; i < g_n; i++)
        if (g_down[i] && player_pos(g_extra[i], p) &&
            (p[0] - e[0]) * (p[0] - e[0]) + (p[2] - e[2]) * (p[2] - e[2]) < DOWN_ENEMY_NEAR * DOWN_ENEMY_NEAR)
            g_down_enemy_at[i] = renderer_present_count();
}

// The player standing on a floor that crumbles (AC_FLOOR; within FLOOR_REACH of its middle across, a step up or down);
// 0: Player 1 is (or nobody).
#define FLOOR_REACH 1.0f
static int floor_player(uint64_t floor) {
    float f[3], p[3];
    if (!actor_pos(floor, f)) return 0;
    for (int i = 0; i < g_n; i++)
        if (player_pos(coop_player(i), p) && fabsf(p[0] - f[0]) < FLOOR_REACH && fabsf(p[2] - f[2]) < FLOOR_REACH &&
            fabsf(p[1] - f[1]) < 1.0f)
            return i;
    return 0;
}

// A Like Like reads "Link" in its state handlers (main+0x4a64c0...0x4ab1d0, x19 the Like Like), and those run
// outside its update, where "Link" is its target: one that went for Player 2 hit him and then, at the catch
// (main+0x4a9810), flew in a straight line onto Player 1, 18 units off; Player 2 wasn't caught
// (user 2026-10-30; build/likelike.sh). Each of its nine reads (COOP_LIKELIKE_SITE_0..8: the register it loads, the
// one it loads from) gets the player it goes for (kept_target), with enemy targets on.
static const uint8_t k_likelike_reads[][2] = {{22, 21}, {21, 20}, {9, 20}, {21, 20}, {21, 20},
                                              {8, 20},  {8, 20},  {8, 21}, {21, 20}};
static int likelike_player(Cpu *c, int k) {
    uint64_t own = LD64(c->x[k_likelike_reads[k][1]] + 0x4b8), ll = c->x[19];
    int t = g_n > 1 && g_settings.coop_targets && heap_ptr(ll) && g_type_likelike >= 0 &&
                    actor_type(ll) == g_type_likelike
                ? kept_target(ll)
                : -1;
    c->x[k_likelike_reads[k][0]] = t > 0 && t < g_n && collision_of(g_extra[t]) ? g_extra[t] : own;
    return 1;
}

// A dungeon's collapse tile ("ObjDungeonCo-...": ObjDungeonCollapseTile, ...Lv05, ...Cave...) drops when the game
// manager's player is within 1.35 across of its middle, measured in a job of its own each frame (main+0xb01a40, x19 the
// tile; the drop main+0xb005b0) - not one of the actors' updates, where "Link" is swapped for the enemies: only Player 1
// set one off (user 2026-10-30, a Tal Tal Heights cave: "the crumbling stone we are by doesn't crumble for other
// players"; build/floor.sh). The Link it measures (COOP_TILE_PLAYER_SITE) is the player nearest it: the game's own
// check then goes by him. (Its job's earlier check of the player's state is still Player 1's.)
static uint64_t tile_player(uint64_t tile, uint64_t own) {
    float e[3], d[COOP_MAX], bd;
    if (g_n < 2 || !own || !actor_pos(tile, e)) return own;
    int best = player_dists(e, d, &bd);
    if (best <= 0 || !collision_of(g_extra[best])) return own;
    static uint64_t said[4];  // (once a tile, when he's on it)
    static unsigned nsaid;
    if (bd < 1.35f * 1.35f && said[0] != tile && said[1] != tile && said[2] != tile && said[3] != tile) {
        CLOG("[coop] player %d on %.40s: a collapse tile\n", best + 1, (const char *)(uintptr_t)(tile + 0x18));
        said[nsaid++ % 4] = tile;
    }
    return g_extra[best];
}

// A blade trap (ObjBladeTrap: an object by its name, not an enemy) shoots along its row or column at "Link" lined up
// with it: only Player 1 set them off (user 2026-11-01, Catfish's Maw). Its "Link" (AC_TRAP: its update and its
// reaction part's) is the nearest player within TRAP_LINE of its row or column; nobody: Player 1, who isn't either.
#define TRAP_LINE 0.75f  // (half a tile)
static int trap_target(uint64_t trap) {
    float e[3], p[3], bd = 1e30f;
    int best = 0;
    if (!actor_pos(trap, e)) return 0;
    for (int i = 0; i < g_n; i++) {
        if ((i && g_down[i]) || !player_pos(coop_player(i), p) || p[1] < -500.0f) continue;
        float ax = fabsf(p[0] - e[0]), az = fabsf(p[2] - e[2]);
        if ((ax < TRAP_LINE || az < TRAP_LINE) && fmaxf(ax, az) < bd) bd = fmaxf(ax, az), best = i;
    }
    return best;
}

// A lifted object (a pot, a rock, a bomb...: ACT_LIFT) asks for "Link" in its own update, and flies the way he faces
// when thrown: another player's pot flew the way Player 1 faced (user 2026-11-01: "for other players, throwing pots
// goes backwards"; probed: the pot's update, main+0xb27da4, through main+0xd56b80). The object another player lifted
// last (doaction_get) has him for "Link" while it updates - known by its address and its name, CARRY_FRAMES at most.
#define CARRY_FRAMES (60 * 300)  // (5 minutes)
static struct { uint64_t obj, tag, until; } g_carried[COOP_MAX];
static void carried_by(int who, uint64_t obj) {
    for (int i = 0; i < COOP_MAX; i++)
        if (g_carried[i].obj == obj) g_carried[i].obj = 0;
    if (who > 0 && heap_ptr(obj))
        g_carried[who].obj = obj, g_carried[who].tag = LD64(obj + 0x18),
        g_carried[who].until = renderer_present_count() + CARRY_FRAMES;
}
static int carrier(uint64_t actor) {  // (the other player who lifted it last; 0: none)
    for (int i = 1; i < g_n; i++)
        if (g_carried[i].obj == actor) {
            if (renderer_present_count() < g_carried[i].until && LD64(actor + 0x18) == g_carried[i].tag) return i;
            g_carried[i].obj = 0;
        }
    return 0;
}

// "Link" for the game's code: each read of the game manager's player slot (+0x4b8) in the main program is this call
// (tools/patches.py _player_read, the reads in runtime/game_addresses/coop_player_reads_<version>.txt). While an
// enemy updates on the game thread (actor_exec_begin), the game manager's reads give the Link it goes for; any other
// read - another thread's (a worker job's), outside an update, with co-op off, another object's +0x4b8 (a read whose
// base the list's scan couldn't follow) - what the slot holds. Until 2026-11-13 the slot itself was swapped for the
// whole update, which any other thread's read saw too.
// (it loads the slot itself, only when it is the game's answer: while an enemy's is given, the slot isn't read by these
// reads at all - LARECOMP_COOP_WATCH_SLOT, a test, catches any other read of it then)
uint64_t rt_coop_player(uint64_t base) {
    uint64_t as = atomic_load_explicit(&g_as_link, memory_order_relaxed);
    return as && on_game_thread() && base == LD64(GA(GAME_MGR)) ? as : LD64(base + 0x4b8);
}

static void actor_exec_begin(Cpu *c) {
    if (g_as_on) return;
    uint64_t actor = g_updating;
    if (!actor) return;
    int t = carrier(actor);
    if (t) goto swap;
    uint8_t what = actor_kind(actor);
    if (what == AC_FLOOR) {
        static uint64_t said[4];  // (once a floor: which one, whose - the last few: one stands on two floors' edge)
        static unsigned nsaid;
        if ((t = floor_player(actor)) && said[0] != actor && said[1] != actor && said[2] != actor && said[3] != actor) {
            CLOG("[coop] player %d on %.40s: its \"Link\"\n", t + 1, (const char *)(uintptr_t)(actor + 0x18));
            said[nsaid++ % 4] = actor;
        }
    } else if (!g_settings.coop_targets)
        return;
    else if (what == AC_ENEMY_PART) {
        // (its reaction to a hit goes by "Link" too: the target kept for the chase is the nearest player now, not the
        // one it stays on - another player's hit knocked it back from where its target stood, up onto a ledge: user
        // 2026-10-28, a turtle in Angler's Tunnel). A Zol's or a Gel's goes by the one it keeps (clinger_target): its
        // reaction part steers its hops, and after each hit on Player 2 knocked him back, Player 1 beside him was the
        // nearest - it went for Player 1 (user 2026-10-31, "always going on p1 when hitting p2"; build/zolhit.sh).
        uint64_t owner = LD64(actor + 0x48);
        uint8_t of = heap_ptr(owner) ? actor_kind(owner) : AC_OTHER;
        if (of != AC_ENEMY && of != AC_ENEMY_NEAREST && of != AC_TRAP) return;
        t = of == AC_ENEMY_NEAREST ? clinger_target(owner) : of == AC_TRAP ? trap_target(owner) : nearest_player(owner);
    } else if (what == AC_TRAP)
        t = trap_target(actor);
    else if (what == AC_ENEMY)
        t = enemy_target(actor);
    else if (what == AC_ENEMY_NEAREST)
        t = clinger_target(actor);
    else
        return;
swap:;
    uint64_t gm = LD64(GA(GAME_MGR)), own = gm ? LD64(gm + 0x4b8) : 0;
    if (ENV("LARECOMP_COOP_TRACE") && traced(actor)) {  // (trace_actor: whom it goes for)
        static uint64_t who[32][2];
        int k = 0;
        while (k < 31 && who[k][0] && who[k][0] != actor) k++;
        int now = t && own && collision_of(g_extra[t]) ? t : 0;
        if (who[k][0] != actor || who[k][1] != (uint64_t)now + 1) {
            float e[3] = {0};
            trace_pos(actor, e);
            LOG("[trace] frame %llu %.40s goes for P%d (at %.2f %.2f %.2f)\n", (unsigned long long)renderer_present_count(),
                (const char *)(uintptr_t)(actor + 0x18), now + 1, e[0], e[1], e[2]);
        }
        who[k][0] = actor, who[k][1] = (uint64_t)now + 1;
    }
    if (!t || !own || !collision_of(g_extra[t])) return;
    g_as_on = true;
    atomic_store_explicit(&g_as_link, g_extra[t], memory_order_relaxed);
}

static void actor_exec_end(void) {
    if (!g_as_on) return;
    g_as_on = false;
    atomic_store_explicit(&g_as_link, 0, memory_order_relaxed);
}
