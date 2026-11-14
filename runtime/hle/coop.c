// Local co-op: 2-8 Links on one screen, Player 1 on the keyboard / first controller and each other player on a
// controller of his own (setting coop_players; runtime/hle/input.c gives npad 2..8 their pads).
//
// Credit: the hook points and the approach come from "The Legend of Zelda: Link's Awakening Multiplayer", a Switch
// mod by Estuko and Geek Joystick (2024; https://gamesdb.launchbox-app.com/games/details/422143-the-legend-of-zelda-links-awakening-multiplayer ,
// trailer https://www.youtube.com/watch?v=1JfE42xE-4k ). Thank you both.
//
// A port of that mod's build LA_Coop_ExactStack_360 (the "8 Players" one: an exlaunch
// "pxtch" module for 1.0.1, its subsdk9 patch table read 2026-10-27) onto the hook sites of tools/patches.py (COOP_*): the game makes one Link per player through its
// own player creator, places all of them on every spawn point, gives each the controller of his npad and a tunic
// colour set of his own (the mod's Playerlink.bfres, served from <data root>/coop/romfs: coop_romfs_override). Unlike
// the mod: the players' indices live here, not past the end of the Link object (the mod grew the allocation by 4 and
// wrote 8 bytes there); a door's walk-in runs for Player 1 only and the others ride on him (the mod ran the one shared
// walk-in once per player, which left the last player behind in the doorway - and out of the room again).
//
// Laid out in parts (runtime/hle/coop/*.inc), included below in order as one translation unit: they share the players'
// state, the actor classes and each other's helpers, which stay static to co-op rather than being exported from it.
// Here: the players (who is who, their positions), then hearts, camera, players (left behind, lost, the HUD's
// questions), targets, spawn, events, field; then the hook sites' switch (rt_coop_hook), the co-op RomFS files and
// the per-frame tick.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../game_addresses.h"
#include "../hle.h"
#include "../input_bind.h"
#include "../settings.h"
#include "../cosmetic_colour.h"

#define COOP_MAX 8

// This fork's own name and version, the log's [fork] line just before upstream's [info] line (runtime/main.c). Co-op
// isn't upstream's: a co-op bug filed on upstream's issues by mistake is told from its log at once, and sent here.
#define COOP_FORK_VERSION "0.1.0"
const char *coop_fork_name(void) {
    return "LAS Recomp Co-Op " COOP_FORK_VERSION ", a fork of LAS Recomp with local co-op - report its bugs at "
           "https://github.com/DrHammer79/LASRecomp/issues";
}

static int g_n = -1;                // players (rt_coop_players; read again out of play: coop_tick)
static uint64_t g_extra[COOP_MAX];  // players 2..8: their Link actors ([0] unused: Player 1 is the game's own)
static int g_creating = -1;         // the player coop_create is making a Link for (-1: none)
static int g_placing = -1;          // the player coop_place is placing (-1: none)
static int g_entering;              // inside coop_enter's own call of the walk-in
static uint64_t g_placed_frame;     // when the players were last put on a spawn point (renderer frame)
static uint64_t g_lost_until[COOP_MAX];  // a player lost below the world: put on Player 1 at this frame (0: he isn't;
static bool g_lost_parked[COOP_MAX];     // ... standing again, parked out of sight until then - lost_tick)

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
    const char *e = ENV("LARECOMP_COOP");
    int n = e ? atoi(e) : g_settings.coop_players;
    unsigned long at;  // LARECOMP_COOP_AT=<frame>:<n> (test): the setting changed to n at that frame, as from the menu
    int later;
    if ((e = ENV("LARECOMP_COOP_AT")) && sscanf(e, "%lu:%d", &at, &later) == 2 && renderer_present_count() >= at)
        n = later;
    return n < 1 ? 1 : n > COOP_MAX ? COOP_MAX : n;
}

// The count in effect (input.c gives players 2.. their controllers by it): the game's once it has one - it changes
// out of play only (coop_tick) - else the setting.
int coop_players_now(void) { return g_n > 0 ? g_n : coop_players_setting(); }

// Player 1's Link: the game manager's player (+0x4b8), as for the game itself. Co-op never writes that slot: the
// game's reads of it ask rt_coop_player, which gives an updating enemy the Link it goes for (g_as_link).
static bool g_as_on;                  // an enemy's update has decided whom it goes for (actor_exec_begin)
static _Atomic uint64_t g_as_link;    // ... "Link" for the game's reads meanwhile (the game thread's; 0: the slot's)
static uint64_t host_player(void) {
    uint64_t gm = LD64(GA(GAME_MGR));
    return gm ? LD64(gm + 0x4b8) : 0;
}

uint64_t coop_player(int i) { return i == 0 ? host_player() : i > 0 && i < COOP_MAX ? g_extra[i] : 0; }

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

#include "coop/hearts.inc"
#include "coop/camera.inc"
#include "coop/players.inc"
#include "coop/targets.inc"
#include "coop/spawn.inc"
#include "coop/events.inc"
#include "coop/field.inc"


// Alone (no co-op) only these run: the zoom's, with the zoom on - the camera (and once more as it goes off: the camera's
// own values back), the Field's boxes of screens, the shadow map's box, and on the Field the actors' updates for the
// cut grass and thrown rocks kept gone in view (zoomed out, the screen Player 1 just left is still in view); this
// boot's co-op tunic file's (the game's own tunic frames in it: COOP_COLOR_SITE).
bool coop_colours(void);
static bool alone_hook(int which) {
    switch (which) {
    case 7: return coop_colours();
    case 15: case 16: return ENV("LARECOMP_COOP_TRACE") || ENV("LARECOMP_TRACK_RESPAWN");  // (test logs, alone too)
    case 17: return zoom_on() || g_back != 0;
    case 33: case 34: case 35: return g_back_on;
    case 38: return g_zoom > 1.0f;
    }
    return false;
}

// An actor's update skipped (COOP_ACTOR_EXEC_SITE returns 1: its blr x9 not run). Its result - a std::future<void>,
// written through x8 - is empty, as an update that runs writes it: the update loop (main+0xfacb80) keeps it, waits on it
// and destroys it, and for the first update of a pass x8's slot was never written.
static int skip_update(Cpu *c) {
    ST64(c->x[8], 0);
    g_updating = 0;
    return 1;
}

int rt_coop_hook(Cpu *c, int which) {
    if (!rt_coop_on() && !alone_hook(which)) return 0;
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
        float f;
        memcpy(&f, &c->v[0], 4);
        if (!coop_colours()) {  // (no co-op tunic file: every other player in the game's green tunic, coloured as he is
            // drawn - coop_tunic_tint; Player 1 keeps his own, the Red or Blue Mail too)
            int i = coop_index(c->x[19]);
            if (!rt_coop_on() || i <= 0) return 0;
            f = 0;
            __uint128_t v = 0;
            memcpy(&v, &f, 4);
            c->v[0] = v;
            return 0;
        }
        // (the coop Playerlink.bfres: 3 frames per colour set, all in that set's colour - 0-2 green, 3-5 red, 6-8 blue
        // ...; co-op turned off since the game started, the file still in: the game's own tunic, its set)
        int t = f >= 2.5f ? 2 : f >= 0.5f ? 1 : 0;
        int set = rt_coop_on() ? g_settings.coop_colour[coop_index(c->x[19])] : t;
        f = (float)((set >= 0 && set < COOP_COLOURS ? set : 0) * 3 + (rt_coop_on() ? t : 0));
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
        // A Link's (only Links are lost: another actor made where a Link was isn't one of them): off the map after
        // falling through it (lost_tick)
        if (rt_coop_on() && g_updating && LD64(g_updating) == GA(PLAYER_VTABLE) && lost_away(g_updating))
            return skip_update(c);
        if (heap_ptr(g_updating)) respawn_track(g_updating), trace_actor(g_updating);
        if (!rt_coop_on()) return 0;  // (alone: alone_hook)
        if (heap_ptr(g_updating)) down_enemy_note(g_updating);
        actor_exec_begin(c);
        act_update_begin(g_updating);
        return 0;
    }
    case 16:  // COOP_ACTOR_EXEC_DONE - after it
        if (!on_game_thread()) return 0;
        if (!rt_coop_on()) {  // (alone: alone_hook)
            g_updating = 0;
            return 0;
        }
        act_update_end();
        actor_exec_end();
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
    case 24:  // COOP_EXIT_LEAVE_CLEAR - bl main+0x76b20: drops its pending exit - not for another player's Link (his
              // touch kept none: exit_touch - unless anyone's step leads out, then his goes as he steps off)
        return g_leaving > 0 && g_settings.coop_host_exits;
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
        if (g_back_on && g_box != 1.0f) scale_s(c, 1, g_box), scale_s(c, 2, g_box);
        return 0;
    case 34:  // COOP_KEEP_BOX_SITE - ld1 {v0.s}[1], [x23]: s0, s1 (across) and s3, s2 (deep) the half sizes of the
              // box of screens whose objects are kept
        if (g_back_on && g_box != 1.0f)
            for (int r = 0; r < 4; r++) scale_s(c, r, g_box);
        {
            float rx, rz;
            keep_reach(&rx, &rz);
            for (int r = 0; r < 4 && (rx > 0 || rz > 0); r++) {
                float h;
                memcpy(&h, &c->v[r], 4);
                set_s(c, r, fmaxf(h, r < 2 ? rx : rz));
            }
        }
        return 0;
    case 35:  // COOP_ACTIVE_BOX_SITE - fsub s8, s0, s3: s3 x s4 the half sizes of the box of active screens, s5 its
              // offset deep - grown to take in every player
        active_box(c);  // (on the Field with the zoom: g_back_on)
        return 0;
    case 36:  // COOP_WARP_OBJ_TOUCH_SITE - ldrb w8, [x8, #0x43]: a player actor (x8) stepped into a warp hole object
        return warp_hole(c, true);
    case 37:  // COOP_WARP_OBJ_LEAVE_SITE - ldrb w8, [x8, #0x43]: ... stepped out of it
        return warp_hole(c, false);
    case 38:  // COOP_SHADOW_BOX_SITE - cbnz w1: the shadow map's light box (cascade w1: only 0 is fitted) fitted to the
              // view x2's world box around Player 1 - grown with the zoom
        if (!(uint32_t)c->x[1]) shadow_box(c->x[2]);
        return 0;
    case 39:  // COOP_TILE_PLAYER_SITE - ldr x8, [x20, #0x4b8]: the Link a collapse tile (x19) measures
        c->x[8] = tile_player(c->x[19], LD64(c->x[20] + 0x4b8));
        return 1;
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48:  // COOP_LIKELIKE_SITE_0..8
        return likelike_player(c, which - 40);  // (ldr: "Link" for a Like Like, x19)
    case 49: {  // COOP_DAMAGE_HALVED_SITE - ldrh w8, [x8, #0x84]: an enemy's hit (w20), after the game halved it or not
        // The game goes on to take w20 off its hearts (x8: PlayerStatus) and, only for a hit of 1 or more
        // (main+0xd4c788), runs what follows a hit for the Link hit (x21): his controller's rumble and the time he
        // can't be hit again (his +0x1e80). Another player's hit, off his own hearts, was 0 to it: the same contact hit
        // him again on the next frames - one hit came off his hearts two to four times (user 2026-11-01; until
        // 2026-11-13 a 30-frame gap after each of his hits hid it). So it stays a hit for the game, which takes nothing
        // off: its hearts are read as that much more.
        int32_t dmg = (int32_t)c->x[20], got = rt_coop_damage_of(c, dmg, c->x[21]);
        if (got == 0 && g_hit_own && dmg > 0) {
            c->x[8] = (uint32_t)(LD16(c->x[8] + 0x84) + dmg);
            return 1;
        }
        c->x[20] = (uint32_t)got;
        return 0;
    }
    case 50:  // COOP_RESTART_SITE - ldr x1, [x20, #0x4a8]: a Link's update (x19) looks up the player entity
        // The game keeps the save's restart point in the player entity's object (+0x680: the spot on safe ground, snapped
        // to 0.75; +0x690 the facing; +0x694 set), which each Link's update writes (main+0xd355c0) - every Link's, so it
        // was the last one updated, the first one made: a save with him away restarted Player 1 on his spot, and one with
        // him fallen through the ground had Link falling forever (user 2026-10-28). The save copies it (main+0xdfc710 ->
        // main+0xdfcb90, file +0x559c). Another player's Link finds no entity there, the game's own path for that
        // (main+0xd35788 cbz): only Player 1's keeps the restart point, as alone. (Until 2026-11-13 the others' 19
        // position copies were held on Player 1's spot while the pause menu was open.)
        if (!coop_index(c->x[19])) return 0;
        c->x[1] = 0;
        return 1;
    }
    return 0;
}

// The mod's Playerlink.bfres (24 colour frames: 3 per player), if someone has it: served from <data root>/coop/romfs
// while co-op is on (runtime/services/fs.c), its colour sets then picked by COOP_COLOR_SITE. Not needed: without it the
// tunics are coloured as they are drawn (coop_tunic_tint, below).
static int g_coop_dir = -1;  // (<data root>/coop/romfs there: looked at once a boot, the romfs index asks per file)
static int g_colours = -1;   // (the colour file served: fixed for the boot, as the romfs index)
RT_ON_RESET(coop_files_reset) { g_coop_dir = g_colours = -1; }

bool coop_romfs_override(const char *path, char *out, size_t cap) {
    // (that one file only: other game files copied there from the mod's own romfs - its whole set - aren't served)
    if (!rt_coop_on() || strcmp(path, "region_common/actor/Playerlink.bfres")) return false;
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

// ---- Tunic colours as the Links are drawn (no edited game file: upstream's review, item 7). Without the co-op
// Playerlink.bfres every other player wears the game's green tunic (COOP_COLOR_SITE), and the renderers ask, for each
// draw sampling a tunic map (runtime/nvn/texpatch.c texpatch_tunic_kind), whose Link it is: a Link's draws carry his
// bone matrices (uniform buffer 3), the first one's translation his position (seen 2026-11-13: within 0.05 of it).
// That player's colour then comes from the per-draw HSV tint the Cosmetics tab's live recolours use
// (runtime/nvn/recolour.c): the green tunic's hue window moved to his colour, the shading kept - the leather, skin
// and hair outside the window stay. Player 1's tunic is tinted only when he chose another colour than Green (his Red
// or Blue Mail lies outside the window: it stays). The players' positions are taken as each frame starts (game
// thread) for the render thread.
#define TUNIC_NEAR 0.75f
static float g_tunic_at[COOP_MAX][3];
static _Atomic int g_tunic_n;
static void tunic_tick(void) {
    int n = 0;
    for (int i = 0; i < g_n && i < COOP_MAX; i++) {
        float p[3];
        if (player_pos(coop_player(i), p)) memcpy(g_tunic_at[i], p, sizeof p);
        else g_tunic_at[i][1] = -1e9f;
        n = i + 1;
    }
    atomic_store_explicit(&g_tunic_n, rt_coop_on() && !coop_colours() ? n : 0, memory_order_release);
}
void recolour_pack(float tint[8], float shift, float sat, float val, float centre, float width, float min_sat,
                   float spread);
void coop_tunic_tint(const float *bones, uint32_t size, float tint[8]) {
    int n = atomic_load_explicit(&g_tunic_n, memory_order_acquire);
    if (!n || !bones || size < 48) return;
    float at[3] = {bones[3], bones[7], bones[11]}, bd = TUNIC_NEAR * TUNIC_NEAR;
    int who = -1;
    for (int i = 0; i < n; i++) {
        float dx = g_tunic_at[i][0] - at[0], dy = g_tunic_at[i][1] - at[1], dz = g_tunic_at[i][2] - at[2];
        if (dx * dx + dy * dy + dz * dz < bd) bd = dx * dx + dy * dy + dz * dz, who = i;
    }
    if (who < 0 || g_settings.coop_colour[who] == 0) return;  // (Green: the tunic as it is)
    // the green tunic's typical colour - or the Cosmetics tab's tunic colour, the green map recoloured to it on upload
    float h, sv, v, shift, sat, val;
    uint32_t cos;
    col_hsv(col_target(&g_settings.tunic, COL_SLOT_TUNIC, &cos) ? cos : coop_colour_rgb_of(0), &h, &sv, &v);
    col_mapping(h, sv, v, coop_colour_rgb(who), &shift, &sat, &val);
    recolour_pack(tint, shift, sat, val, h, 0.09f, 0.18f, 1.0f);
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

// Play ended (a game over, back at the title / file select: the game manager let its Link go - game_in_play): the
// Links, the enemies' targets, the cameras are gone with the level - nothing of them is kept. The game kills its Link
// then, and the next file's creator makes him again (seen 2026-11-13: a game over's Continue); the others' Links are
// killed with his, as the game kills an actor (ACTOR_KILL), and the creator makes theirs again too. Left alive they
// went on into the next file: until 2026-11-13 they were parked under the level and taken back by its players, and
// one that wasn't (fewer players, co-op turned off) copied Player 1 - with co-op off, the game's own creator would find
// one of them for its "a Link exists already" check (COOP_DUP_SITE) and make no Link.
static void play_ended(Cpu *c) {
    for (int i = 1; i < COOP_MAX; i++) {
        if (!g_extra[i] || LD64(g_extra[i]) != GA(PLAYER_VTABLE)) continue;
        CLOG("[coop] play ended: player %d's Link %#llx killed\n", i + 1, (unsigned long long)g_extra[i]);
        uint64_t saved[31], sp = c->sp, pc = c->pc;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = g_extra[i];
        guest_call(c, GA(ACTOR_KILL));
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp, c->pc = pc;
    }
    memset(g_extra, 0, sizeof g_extra);
    targets_forget();
    camera_back_put();  // (the camera can go on into the next file: its own values, not the moved-back ones)
    cameras_forget();
    anim_forget();
    hearts_forget();
    memset(g_asleep, 0, sizeof g_asleep);
    memset(g_sleep_anim, 0, sizeof g_sleep_anim);
    lost_forget();
    act_forget();
    g_p1_event = g_p1_walk_in = false, g_talking = false, g_talker = 0;
}

// Every level change in play (the level-jump request processor: runtime/hle/chamber.c rt_level_request) is logged
// with where it goes and who was where, in what state: an area reloaded in play - its grass and rocks back - shows
// what led to it.
void coop_level_change(const char *level, const char *locator) {
    extern int game_in_play(void);
    extern const char *chaos_current_level(void);  // runtime/ui/chaos_overlay.mm
    actor_class_forget();  // (a new level: its actors' addresses are others')
    g_level_changed = renderer_present_count();
    if (!g_act_calls && !g_act_swapped) act_forget();  // (the old level's components: none of the slots' any more)
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
    tunic_tick();
    if (g_as_on) {  // (no update runs as the frame starts: an update whose end went unseen leaves "Link" no one's)
        static int said;
        if (!said++) LOG("[coop] an enemy's update ended unseen: \"Link\" back to the game's\n");
        g_as_on = false, atomic_store_explicit(&g_as_link, 0, memory_order_relaxed);
    }
    camera_back_undo();
    extern int game_in_play(void);
    static bool in_play;
    bool play = game_in_play();
    if (in_play && !play) play_ended(c);
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
    g_hud_host = host_player(), g_hud_cam = view_camera();  // (for the HUD: as the frame starts)
    if (!rt_coop_on()) return;
    p1_event_tick();
    a_press_tick(renderer_present_count());
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
    if (play) down_move_tick(c), lost_tick(), leash_tick(), room_follow_tick();
    extern uint64_t input_coop_buttons(int player);
    uint64_t frame = renderer_present_count();
    static int test_die;  // LARECOMP_COOP_DIE=<player>,... (test): those players sent to the dead state 3 s after they
                          // are placed, as an instant kill does
    if (!test_die && g_placed_frame && frame > g_placed_frame + 180 && ENV("LARECOMP_COOP_DIE"))
        for (const char *p = ENV("LARECOMP_COOP_DIE"); (test_die = 1) && *p; p += *p == ',') {
            char *end;
            int who = (int)strtol(p, &end, 10) - 1;
            if (end == p) break;  // (not a number: the rest is ignored)
            p = end;
            uint64_t link = coop_player(who);
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
        // (k: what snprintf would have written - past the end once a part is cut short: kept to the line from then on)
#define LINE_K(n) (k = k + (n) < (int)sizeof line ? k + (n) : (int)sizeof line - 1)
        for (int i = 0; i < g_n && k < (int)sizeof line - 40; i++) {
            float p[3] = {0};
            player_pos(coop_player(i), p);
            LINE_K(snprintf(line + k, sizeof line - k, " P%d (%.2f %.2f %.2f)", i + 1, p[0], p[1], p[2]));
        }
        float hp[3], v[3], sx = 0, sy = 0;
        if (g_cam[0] && player_pos(host_player(), hp)) {
            cam_view(g_cam[0], hp, v);
            bool on = coop_project(hp, 1280, 720, &sx, &sy);
            LINE_K(snprintf(line + k, sizeof line - k, " | cam %#llx P1 view (%.2f %.2f %.2f) screen %d (%.0f %.0f) fovy %.3f",
                            (unsigned long long)g_cam[0], v[0], v[1], v[2], on, sx, sy,
                            *(const float *)(uintptr_t)(g_cam[0] + 0x564)));
        }
        if (g_zoom != 1.0f) snprintf(line + k, sizeof line - k, " zoom %.3f", g_zoom);
#undef LINE_K
        LOG("%s\n", line);
    }
}
