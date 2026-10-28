# Local co-op

Two to eight Links on one PC, one controller each, after the Switch mod
LA_Coop_ExactStack_360 (its "8 Players" build, game 1.0.1). It works with game
versions 1.0.1 and 1.1.0 (address table `v1_0_1`). Version 1.0.0 has every
`COOP_*` address at 0x0, so co-op stays off there and its option is greyed out.

Everything is in `runtime/hle/coop.c` (game logic), `runtime/ui/coop_hud.mm`
(the other players' hearts, the "Zzz" bubbles) and hook sites in
`tools/patches.py` (`_COOP_HOOKS`). Co-op is off by default: with
`coop_players = 1`, every hook returns 0 and runs its own instruction, so the
game plays exactly as without the feature.

## How it works

Player 1 is the game's own Link: the game manager's player (`[GAME_MGR]+0x4b8`),
the one the camera, the HUD, events and the save follow. The other players'
Links are extra actors of the same class. They keep their own controllers,
hearts and colours, and the game still sees one "Link" everywhere else.

**Making the Links.** The player creator (an event action, `COOP_CREATE_SITE`)
runs once for each extra player, then for Player 1, so `+0x4b8` ends up holding
Player 1's Link, as without co-op. The creator's zone set-up runs for Player 1
only (`COOP_ZONE_SITE`), and its "a Link exists already" check is let through
while the extras are made (`COOP_DUP_SITE`).

**Spawns.** A placement at a spawn point (`COOP_PLACE_SITE`) is run for each
player on the same spot, Player 1 last (`COOP_PLACE_PLAYER_SITE` picks the
Link). A spawn check whose object is gone with more Links is skipped
(`COOP_SPAWN_CHECK_SITE`). Knocked-out players stand up again in a new area.

**Controllers.** `runtime/hle/input.c` opens the second, third... SDL gamepads
for players 2.. (npad ids 1..7, `GetNpadStyleSet` reports them as FullKey).
`COOP_CTRL_SITE` gives each Link his own entry of the npad list.
`COOP_PAD_STATE_SITE` and `COOP_APPLET_*` keep the controller manager polling
and tell the controller applet it has that many players. A knocked-out player's
input is blocked. Both sticks pressed in (L3+R3) puts a player on Player 1.

**Colours.** `COOP_COLOR_SITE` picks each Link's tunic frame from the mod's
`Playerlink.bfres`: 24 colour frames, 3 per colour set. It is served from
`<data root>/coop/romfs/region_common/actor/Playerlink.bfres` when that file
exists (`coop_romfs_override`, `runtime/services/fs.c`). The repository can't
ship it: it is the mod's edit of a game file. Without it the game still plays,
and every Link wears Player 1's tunic.

**Walking through each other.** Every character controller gets the other
characters as user capsules in its PhysX sweep (TouchedGeomType 1). With
player collision off, a Link's sweep skips any Link's capsule
(`COOP_CCT_SWEEP_SITE_0/1`, `cct_sweep`). Stacked Links share one spot, so the
check is "is it a Link", not "which Link".

**Doors and exits.**
- Player 1 walks in through a door (`COOP_ENTER_SITE`) and the others ride on
  him, so everyone arrives together and nobody is left in the doorway to walk
  everyone back out.
- With `coop_host_exits` on (the default), only Player 1 leads everyone out.
  An `AreaLevelOpen` (doors, loading zones) records any touching player actor
  whose `+0x43` bit 6 is clear, then plays the exit for the manager's player.
  Stairs (`AreaUpstairs` / `AreaDownstairs`) work the same way. At those checks
  (`COOP_EXIT_TOUCH_SITE`, `COOP_STAIRS_TOUCH/LEAVE_SITE`) another player's Link
  reads as one with the bit set.
- When a contact with a door ends, the door forgets its pending exit, whoever
  left (`main+0x76b20`). Another player's Link leaving doesn't make it forget
  (`COOP_EXIT_LEAVE_*`).

**Camera areas.** Every Link gets his own camera (`actor+0x318`), and every
camera listens to the camera-area manager, which follows Player 1. A camera
whose `+0x515` is set answers a new scroll area with the "ZeldaScroll" event,
which walks Player 1 on. On the Field the other players' cameras have it set.
In front of the Trendy Game's door this turned Player 1 back on every step, so
only Player 1's camera answers (`COOP_CAMERA_SCROLL_SITE`). The mod renamed the
ZeldaScroll event for this. The recompiled code can't catch the game's
"not found" throw that renaming causes, so `COOP_SCROLL_SITE` leaves the event
alone.

**Enemies** (`coop_targets`). While an enemy's update runs
(`COOP_ACTOR_EXEC_SITE` / `_DONE`, enemies told apart by their level name
`Enemy...-<key>`), `+0x4b8` holds the Link it targets; it is Player 1's again
after. The rules:
- An enemy targets the nearest player who is up.
- Within `TARGET_ENGAGE` (6 units, about 4 tiles) it keeps him until he is
  knocked out, so an attack, a wind-up or a boss pattern plays out on one
  player.
- Otherwise it keeps him `TARGET_LOCK` (6 s), plus `TARGET_GRACE` (2 s) after
  a fight. After that it changes only to a clearly nearer player (1.5x).
- Enemies more than 30 units from everyone are left alone.

**Hearts** (`coop_health`).
- **Shared:** the game's one set, as without co-op.
- **Each player's own:** Player 1 keeps the game's hearts (PlayerStatus `+0x84`)
  and its HUD; the others' hearts are in coop.c. The damage patches
  (`DAMAGE_*` in `tools/patches.py`) call `rt_coop_damage(c, rt_damage(...))`,
  and a hit on players 2.. comes off their own hearts. The Link a hit is on is
  found in the state code's callee-saved registers (`running_player`).
- **Gains:** hearts the game's set gains go to every player who is up: a
  heart, a fairy, a potion, a container. `COOP_HEAL_SITE_0/1` read the uncapped
  total, and `PLAYER_MAX_HEALTH` is re-read.
- **Out of hearts** (`coop_knockout`): knocked out (the default) or a game over
  for everyone.
- **Knocked out:** the player lies in the `down_lp` animation (`PLAY_ANIMATION`
  on his event component, played again after each of his own updates) with a
  breathing "Zzz" bubble over him. Another player holding A beside him (within
  1.6) for `coop_revive_s` seconds revives him with 30% of the maximum and 2 s
  of no damage (`REVIVE_SAFE`).

**HUD** (`runtime/ui/coop_hud.mm`).
- **Layout:** a "P1" tag beside the game's hearts, and the other players'
  heart rows in a corner (`coop_hud`), with no panel behind them, in their
  colours (`coop_heart_colours`, which tints Player 1's game hearts too:
  `texpatch.c`).
- **Corners:**
  - Up to 3 rows go in the chosen bottom corner, the rest in the other one.
  - Top right is a single column below the X/Y items, the rupee count and the
    compass.
  - Bottom right moves up above the "Now saving" note and the run timer.
- **Zzz bubbles:** projected from the camera seen at `COOP_CAMERA_SITE`.
- **Hidden** on the pause menu's Items and Map pages.

**The game's countdown** (Dampé's timed chamber challenges, `Dg_TimeCount`)
sits in the bottom left, where the hearts go. With co-op on, its pane
`N_Time_00` moves to the bottom centre. That is 4 bytes of
`region_common/ui/Game.arc`, found by name (`coop_romfs_patch`) and laid over
the game's reads of it by the RomFS index's patch list.

## Hook sites (v1_0_1)

| Site | Address | Instruction | coop.c |
|---|---|---|---|
| COOP_CREATE_SITE | 0x700083a470 | str x28, [sp, #-0x40]! | the creator, once per player (case 0) |
| COOP_ZONE_SITE | 0x700083a678 | bl main+0x83afa0 | zone set-up skipped for the extras (1) |
| COOP_DUP_SITE | 0x700083ab64 | cbnz x0 | a second Link let through (2) |
| COOP_PLACE_SITE | 0x700085d5f0 | stp x22, x21, [sp, #-0x30]! | every player on the spawn (3) |
| COOP_PLACE_PLAYER_SITE | 0x700085d610 | ldr x20, [x19, #0x4b8] | the Link placed (4) |
| COOP_SPAWN_CHECK_SITE | 0x70008ec4f8 | ldr x0, [x22, #0x208] | skipped when its object is gone (5) |
| COOP_CTRL_SITE | 0x7000d35ae4 | ldp x22, x26, [x8] | each Link's controller (6) |
| COOP_COLOR_SITE | 0x7000d35760 | blr x8 | tunic frame per player (7) |
| COOP_ENTER_SITE | 0x7000dd4950 | sub sp, sp, #0xa0 | Player 1's walk-in, the others ride (8) |
| COOP_SCROLL_SITE | 0x70000bc628 | str x8, [sp] | left as it is (9) |
| COOP_PAD_STATE_SITE | 0x7000015e5c | b.ne | controllers polled (10) |
| COOP_APPLET_SITE / _SINGLE | 0x7000881ebc / ee4 | strb | applet player count (11, 12) |
| COOP_CCT_SWEEP_SITE_0 / _1 | 0x70012a88c8 / 89d0 | cbz x8 | Link capsules skipped (13, 14) |
| COOP_ACTOR_EXEC_SITE / _DONE | 0x7000facbf8 / bfc | blr x9 / ldr x8, [sp, #8] | enemy targets, sleep pose (15, 16) |
| COOP_CAMERA_SITE | 0x7000010908 | ldr x8, [x23, #0x2a8] | camera for the bubbles (17) |
| COOP_HEAL_SITE_0 / _1 | 0x70008d0a5c / 0x70008a39d4 | strh w8, [x9, #0x84] | hearts gained (18, 19) |
| COOP_EXIT_TOUCH_SITE | 0x7000076f5c | ldrb w9, [x23, #0x43] | door touch (20) |
| COOP_STAIRS_TOUCH / LEAVE_SITE | 0x70000617e0 / 1640 | ldrb w8, [x8, #0x43] | stairs (21, 22) |
| COOP_EXIT_LEAVE_SITE / _CLEAR | 0x7000076aa0 / acc | stp x20, x19 / bl main+0x76b20 | door contact ends (23, 24) |
| COOP_CAMERA_SCROLL_SITE | 0x70000bbdcc | ldrb w8, [x19, #0x515] | only Player 1's camera scrolls (25) |

Runtime constants: `PLAYER_VTABLE`, `PLAYER_MAX_HEALTH`, `PLAY_ANIMATION`
(`runtime_v1_0_1.h`). The `case` numbers follow the order of the `COOP_*`
entries in the `HOOK_SITES` list in `runtime/hle/hooks.c`. Add new sites at its
end and in both `patches_v1_0_*.h` tables, in the same order.

## Settings (Controls tab, Co-op)

| Key | Default | |
|---|---|---|
| coop_players | 1 | 1 = off, 2..8 Links (from the next file loaded) |
| coop_collision | 0 | the Links block each other |
| coop_targets | 1 | enemies go after the nearest player |
| coop_host_exits | 1 | only Player 1 leads everyone out |
| coop_health | 1 | 0 shared, 1 each player's own |
| coop_knockout | 0 | 0 knocked out until revived, 1 game over |
| coop_revive_s | 4 | seconds of holding A to revive |
| coop_hud | 0 | 0 bottom left, 1 bottom right, 2 top right |
| coop_heart_colours | 1 | hearts in each player's colour |
| coop_colours | 0,1,2,... | each player's colour set (Green, Red, Blue, Yellow, Purple, Pink, Black, White) |

The tunic colours and the countdown move are fixed at boot (the RomFS index):
after turning co-op on or off, restart LAS Recomp.

## Tests

Address tables: `python tools/test_game_addresses.py`, and with game files
`--game v1_0_1=<extracted>` checks every hook's instruction word.

Test switches (environment):

| Variable | |
|---|---|
| `LARECOMP_COOP=<n>` | the player count, without touching the settings |
| `LARECOMP_COOP_LOG=1` | `[coop]` lines: Links made and placed, hits, knock-outs, exits, target changes, positions every 30 frames |
| `LARECOMP_COOP_DOWN=<p>,...` | those players knocked out 2 s after they are placed |
| `LARECOMP_INPUT_P2..P8=<frame>:<button>:<frames>,...` | scripted input for players 2..8, as `LARECOMP_INPUT` |

What was checked on 1.0.1, mostly with the test inputs above and
`LARECOMP_WARP_AT`:
- **Players and HUD:** 2, 3, 5 and 8 players, each on his own controller. The
  HUD at 5 players, with the bubble over a knocked-out player.
- **Hearts:** knock-out and revive, the 2 s after a revive, shared hearts.
- **Play ending:** game over and continue, and quitting to the start screen
  then loading again (`LARECOMP_REBOOT_AT`).
- **Doors and exits:**
  - Marin & Tarin's house in and out.
  - The Trendy Game in, and out and back in, with 2 and 3 players.
  - Player 2 held at a door, at a loading zone and at stairs.
- **Dungeons and 2D:** Tail Cave's rooms, a side-view room, a fall into a pit
  (the player comes back at the room's entry, Player 1 stays).
- **Enemies:** spear Moblins changing targets only after their lock.
- **Co-op off:** unchanged.

## Not done or not checked

- **Tunic colours** need the mod's `Playerlink.bfres`. Recolouring the tunic
  at run time (as the cosmetics do) would remove the file and the restart.
- **Warp holes, Manbo's warps, falls to a lower floor, event boxes** weren't
  examined. Another player may still trigger them for everyone.
- **Player 1 in another room:** the others stay where they are, off the
  camera's view, until they walk over or press L3+R3.
- **Holding A to revive** also does the game's A action (beside a rock, the
  rock is lifted). The revive goes on.
- **Shared hearts:** stacked Links hit by one attack each cost hearts.
- **The moved countdown** was checked by its bytes, not seen in a timed
  challenge.
- **Not played with extras:** shops, minigames (the Trendy Game's crane, the
  Rapids Ride, fishing) and cutscenes.
