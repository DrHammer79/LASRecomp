// Local co-op, exits, camera areas, A-button actions, event boxes, warp holes, deaths. A part of runtime/hle/coop.c: included there, in this order, as one translation unit -
// the parts share the players' state and each other's helpers (coop.c's head says how it is laid out).

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
    // (Player 1 moved far in one frame as the view turned side-on or back - stairs into a side-view room, out of one -
    // is a new room too. Not a jump alone: put back at a room's entry after a pit, everyone was put on him there.)
    extern bool game_side_view(void);  // runtime/hle/widescreen.c
    static bool side;
    static uint64_t jumped_at, turned_at;
    uint64_t f = renderer_present_count();
    float dx = h[0] - last[0], dy = h[1] - last[1], dz = h[2] - last[2];
    if (have && dx * dx + dy * dy + dz * dz > 10.0f * 10.0f) jumped_at = f;
    if (game_side_view() != side) side = !side, turned_at = f;
    if (jumped_at && turned_at && (jumped_at > turned_at ? jumped_at - turned_at : turned_at - jumped_at) <= 30 &&
        !g_room_frame)
        g_room_frame = f, jumped_at = turned_at = 0;
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
// (the other players' slots hold the game's weak pointers to components, copied: a level's components are gone with
// it, so the slots are emptied as play ends and at every level change - swapped back in later, one held the old level's)
static void act_forget(void) {
    memset(g_act, 0, sizeof g_act);
    g_act_calls = g_act_swapped = 0;
    g_act_comp = 0;
}
RT_ON_RESET(coop_act_reset) { act_forget(); }

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

// A DoAction component's update (x0 the component): once for each other player who can act, "Link" being him (for the
// game's reads: rt_coop_player), then for Player 1 into the game's own slot. (In an enemy's update "Link" is the Link
// it goes for: that again after.)
static int doaction_each(Cpu *c) {
    uint64_t gm = LD64(GA(GAME_MGR));
    if (g_act_calls || !gm || !GA(DOACTION_PENDING)) return 0;
    if (!on_game_thread()) {  // (the player slot is the game thread's: Player 1 only there)
        static int said;
        if (!said++) LOG("[coop] a DoAction component checked off the game thread: Player 1 only\n");
        return 0;
    }
    uint64_t as = atomic_load_explicit(&g_as_link, memory_order_relaxed), comp = c->x[0], args[8];
    memcpy(args, c->x, sizeof args);
    g_act_calls = 1;
    for (int i = 1; i < g_n; i++) {
        if (!collision_of(g_extra[i]) || coop_player_down(i)) continue;
        atomic_store_explicit(&g_as_link, g_extra[i], memory_order_relaxed);
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
    atomic_store_explicit(&g_as_link, 0, memory_order_relaxed);  // (Player 1: the slot's own)
    memcpy(c->x, args, sizeof args);
    guest_call(c, GA(COOP_DOACTION_SITE));
    atomic_store_explicit(&g_as_link, as, memory_order_relaxed);
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
// Started on a press of A (a_press_tick) and as the game's own check allows it (the component's flags +0x40, the
// countdown after an action): from A still held, the talk began again as it ended - his A had closed its last text.
#define CLAIM_FRAMES 30
#define ACT_PULL 3  // (levers: the grab and pull, his own state - played on Player 1 another player's A did nothing,
                    // user 2026-10-28, Angler's Tunnel)
#define ACT_LIFT 4  // (rocks, pots, bushes, bombs...: the lift, his own state)
static bool own_action(uint64_t comp) { uint8_t t = LD8(comp + 0x1a0); return t == ACT_LIFT || t == ACT_PULL; }
static struct { uint64_t comp, until; } g_claim[COOP_MAX];  // (per player)
extern uint64_t input_coop_buttons(int player);
static bool g_a_held[COOP_MAX];         // (players 2..: A on his controller last frame)
static uint64_t g_a_pressed[COOP_MAX];  // (... the frame he last pressed it, able to act: no event, not knocked out)
RT_ON_RESET(coop_claim_reset) {
    memset(g_claim, 0, sizeof g_claim); g_talking = false, g_talker = 0;
    memset(g_a_held, 0, sizeof g_a_held), memset(g_a_pressed, 0, sizeof g_a_pressed);
}

static void a_press_tick(uint64_t frame) {  // (once a frame, as the controllers are read: before the updates)
    extern uint64_t input_coop_held(int player);
    for (int i = 1; i < g_n; i++) {
        bool a = input_coop_held(i) & 1;
        if (a && !g_a_held[i] && !coop_input_blocked(i)) g_a_pressed[i] = frame;
        g_a_held[i] = a;
    }
}

static bool doaction_can(uint64_t comp) {  // (the getter's own check: main+0xe8d90)
    float cool;
    memcpy(&cool, (const void *)(uintptr_t)GA(DOACTION_COOLDOWN), 4);
    return (LD32(comp + 0x40) & 0x40000017u) == 0x17 && !(cool > 0);
}

// Shops deal with Player 1. The shopkeeper sells what "Link" carries: Player 2 bringing him the bombs got his greeting,
// and was left holding them. So the goods (ObjShopItem) are nothing to the other players' A.
static bool shop_item(uint64_t comp) {
    uint64_t owner = LD64(comp + 0x48);
    return heap_ptr(owner) && g_type_shop_item >= 0 && actor_type(owner) == g_type_shop_item;
}

static int doaction_get(Cpu *c) {
    if (!GA(DOACTION_TAKE_RET) || !on_game_thread() || !g_updating) return 0;
    bool take = c->x[30] == GA(DOACTION_TAKE_RET), check = c->x[30] == GA(DOACTION_CHECK_RET);
    int who = g_act_swapped ? g_act_swapped : g_updating == host_player() ? 0 : -1;
    uint64_t comp = slot_live(GA(DOACTION_CURRENT)), f = renderer_present_count();
    if ((!take && !check) || who < 0 || !comp) return 0;
    if (take) {
        g_claim[who].comp = comp, g_claim[who].until = f + CLAIM_FRAMES;
        if (LD8(comp + 0x1a0) == ACT_LIFT) carried_by(who, LD64(comp + 0x48));
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
        if (f - g_a_pressed[who] < 2 && g_a_pressed[who] && doaction_can(comp) &&
            !(g_claim[who].comp == comp && f < g_claim[who].until)) {
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
// (the contact as at the doors' end of contact: contact_link) - or one of a player on his spot: the box keeps one
// contact (+0x370), the last begun, and fires with it the next frame (main+0x6a980). Five Links stacked on Player 1
// by the ghost's walk to his grave began theirs with the owl's box at once: Player 3's was kept, Player 1's gone, the
// box never fired and the event waited for ever, Player 1 still in it (user 2026-10-30, twice).
static bool on_host_spot(int i) {  // (put on him - an event's walk, a door's - and not moved since: as capsule_of)
    float p[3], h[3];
    if (!player_pos(coop_player(i), p) || !player_pos(host_player(), h)) return false;
    float dx = p[0] - h[0], dz = p[2] - h[2];
    return dx * dx + dz * dz < 0.04f && fabsf(p[1] - h[1]) < 0.5f;
}
static int event_box(Cpu *c) {
    int i = contact_link(c->x[1]);
    if (i <= 0) return 0;
    static uint64_t logged_frame;
    uint64_t frame = renderer_present_count();
    bool fire = on_host_spot(i);
    if (frame > logged_frame + 30)
        CLOG("[coop] player %d in an event box (%.48s): %s\n", i + 1, (const char *)(uintptr_t)(c->x[0] + 0x18),
             fire ? "on Player 1's spot, sets it off" : "only Player 1 sets it off");
    logged_frame = frame;
    return fire ? 0 : 2;  // (2: to its ret)
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
// put him back. Lying still, the spikes' hits (rt_coop_damage: down_hit) put him on Player 1; the knockback alone
// doesn't count (a sword's or a shove's costs nothing).
#define PLAYER_STATE_DEAD 0x12
#define LINK_KIND_KNOCKED_BACK 11

static int dead_state(Cpu *c) {
    unsigned s = (unsigned)(c->x[1] & 0xff);
    int i = coop_index(c->x[0] - 0x4e0);
    if (!i || !own_hearts()) return 0;
    uint64_t st = g_down[i] && s < 0x40 ? LD64(g_extra[i] + 0x1340 + 8ull * s) : 0;
    if (heap_ptr(st) && LD8(st + 0x28) == LINK_KIND_KNOCKED_BACK) return 2;  // (to the change's ret)
    if ((s != PLAYER_STATE_DEAD && s != PLAYER_STATE_DEAD + 1) ||
        (g_settings.coop_knockout == COOP_KO_GAME_OVER && g_hp[i] == 0))
        return 0;
    LOG("[coop] player %d sent to the dead state (%#x): knocked out instead of a game over\n", i + 1, s);
    if (!g_down[i]) knock_out(i);
    return 2;  // (to the change's ret: the state stays)
}
