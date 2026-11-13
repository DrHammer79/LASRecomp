# Local co-op

Two to eight Links on one PC, one controller each, after the Switch mod
[The Legend of Zelda: Link's Awakening Multiplayer](https://gamesdb.launchbox-app.com/games/details/422143-the-legend-of-zelda-links-awakening-multiplayer)
by Estuko and Geek Joystick ([trailer](https://www.youtube.com/watch?v=1JfE42xE-4k)), its build
LA_Coop_ExactStack_360 (the "8 Players" build, game 1.0.1). It works with game
versions 1.0.1 and 1.1.0 (address table `v1_0_1`). Version 1.0.0 has every
`COOP_*` address at 0x0, so co-op stays off there and its option is greyed out.

Everything is in `runtime/hle/coop.c` (game logic: laid out in parts,
`runtime/hle/coop/*.inc` - hearts, camera, players, targets, spawn, events,
field - included by it in order as one translation unit, since they share the
players' state and each other's helpers), `runtime/ui/coop_hud.mm`
(the other players' hearts, the "Zzz" bubbles) and hook sites in
`tools/patches.py` (`_COOP_HOOKS`). Co-op is off by default: with
`coop_players = 1`, every hook returns 0 and runs its own instruction, so the
game plays exactly as without the feature - unless "Zoom out" is set to
Always, when the zoom's own hooks run alone (`alone_hook`, under "Camera
zoom"), or after a session with co-op this boot: the tunic file still in
("Colours") and the Links left over from it ("Making the Links").

## How it works

Player 1 is the game's own Link: the game manager's player (`[GAME_MGR]+0x4b8`),
the one the camera, the HUD, events and the save follow. The other players'
Links are extra actors of the same class. They keep their own controllers,
hearts and colours, and the game still sees one "Link" everywhere else.

**Making the Links.** The player creator (an event action, `COOP_CREATE_SITE`)
runs once for each extra player, then for Player 1, so `+0x4b8` ends up holding
Player 1's Link, as without co-op. The creator's zone set-up runs for Player 1
only (`COOP_ZONE_SITE`), and its "a Link exists already" check is let through
while the extras are made (`COOP_DUP_SITE`). Player 1 has to be last: the
scene's camera is the last Link made's. Made first, he got the save right (see
"The pause menu" below) but the camera showed the ground somewhere else.

The creator can run again for one area: a dungeon loaded fresh, a door's
failed entry. The extras it made the first time were still alive, so every run
added another set: copies of a player standing about, moving with his
controller. Each Link the creator makes is remembered (`g_made`). A run takes
back a live one that no player holds before making a new one. One that is left
over anyway is put out of the way (under the map) and its update is skipped
(`COOP_ACTOR_EXEC_SITE`). That goes on with co-op turned off too (`g_leftover`:
the game keeps the Links through a Game Over's Continue and a return to the
title, and they copied Player 1), and they don't keep the save's restart point
(see "The save's restart point").

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
Player 1's own controller follows the one in use (#455: a press, or a stick
pushed while his rests, on another pad makes it his), but never one that
another player has: their pads' events go to their slots before that check,
and neither a pad added nor the one left when his is unplugged becomes his if
it's in a slot (`tools/test_active_pad.c`, with two players and three pads).
- The settings menu takes every controller's sticks (`runtime/ui/menu.mm`):
  each controller's are kept apart, and a direction is the furthest any of
  them pushes it, on from 0.5 and off below 0.35. Before (upstream's), every
  pad's stick events set the same keys at 0.3: with five pads, one resting
  stick's noise undid another's held direction and a stick resting near 0.3
  stepped the menu by itself (user 2026-11-01: "menu freaks out randomly with
  no one doing controllers on it"). Not checked with real pads.
- The slots follow the player count in effect (`coop_players_now`: the game's,
  which changes out of play only). Changed mid-play, a pad let go took over
  Player 1's Link at its next press while its own Link stood still.
- A pad gets its own open before a slot takes it, also when the count changed
  before its "added" event (set with the menu up): the slot's open was its only
  one, and letting the slot go closed it until it was plugged in again.
- A quick tap (pressed and released before the game polls) reaches players
  2.. as it reaches Player 1 (`coop_event_press`, `note_press`).
- `+` and `-` (the pause menu, the map) are Player 1's: the menu and a save take
  their spot from his (the hold, below), so the other players' are not passed on.
- Rumble: npads 1..7 rumble the other players' own controllers
  (`rumble_slot`; IPC 211 reports theirs mounted while they have one).
- Whose controller is whose: the Controls tab's "Player N controller" rows.
  Chosen, the row waits for a button (`input_assign_begin`); the next press,
  on any controller, gives that one to the player, taken from whoever had it
  (they get the player's own). The press is taken before the menus see it
  (`input_assign_event`, first in the event loops); Escape or 10 s gives up.
  Each player's is remembered (`coop_pad_path_N` in the ini, `pad_key`): on
  the desktop by SDL's device path (for an Xbox-type pad its XInput slot,
  which follows the order they connect in); on a phone or tablet, or where SDL
  gives no path (iOS, macOS's MFi pads, Android), by the controller's GUID
  ("guid:..."): its kind, so two of the same kind aren't told apart there and
  the first to connect goes to the first player it was assigned to. As they
  connect, one with a player's key goes to him, a slot whose player's
  controller is connected is kept for it, and Player 1's isn't taken over by a
  press on a spare one. Only with co-op on (2 players or more): alone, Player
  1's controller is whichever one is used, as upstream's (#455).
- On the start screen, before the game has taken the controllers, the Controls
  tab names the ones connected in the order they'll be taken.
- Windows: SDL reads Xbox-type controllers through XInput, 4 at most (its
  RawInput and WGI drivers are off by default). A fifth isn't seen: an 8BitDo
  SN30 Pro in X-input mode over Bluetooth, with four Xbox 360 pads in, didn't
  even start in Windows (Device Manager: code 10). In its Switch or D-input
  mode it isn't an XInput device. The colour rows of players 5.. without a
  controller say so.

The game reads which controllers are connected when the npad's "style set
changed" event is signalled. A controller turned on after the game had read
them (at the start screen) gave a Link that never moved. Those events are kept
(`hid_bind_style_event`), and `input_style_changed()` signals them again
whenever the player count changes out of play.

**Colours** (`coop_tunic_tint`, 2026-11-13: no edited game file, as upstream's
review asked, item 7). Every player's tunic is coloured as it is drawn. The
game's Link material samples his tunic map (`MI_body_alb.0`, `.1`, `.2` in
`Playerlink.bfres`: green, Red Mail, Blue Mail - recognised on upload,
`texpatch_tunic_kind`). Each Link's draws carry his bone matrices (uniform
buffer 3), and the first one's translation is his position (found with a
probe: within 0.05 of it), so the renderer (Vulkan `exec.c`, Metal
`renderer.m`) asks for each such draw whose Link it is, and that player's
colour comes from the per-draw HSV tint the Cosmetics tab's live recolours use
(`runtime/nvn/recolour.c`): the green tunic's hue window (or the Cosmetics
tab's tunic colour's, the green map being recoloured to it on upload) moved
to his colour, its saturation and value scaled, the shading kept; the leather,
skin and hair, outside the window, stay. The other players wear the green
tunic (`COOP_COLOR_SITE`: frame 0) and take their colour this way; Player 1
keeps the game's own tunic (the Red and Blue Mail stay as they are) and is
tinted only when he chose another colour than Green. Checked with 8 players
(the default colours): green (Player 1's Blue Mail), red, blue, yellow,
purple, pink, black and white tunics, each shaded as the game's.
- The players' positions for it are taken as each frame starts (game thread,
  `tunic_tick`); a draw goes to the nearest within 0.75.
- The co-op mod's `Playerlink.bfres` (24 colour frames, 3 per colour set) is
  still used if it's there (`<data root>/coop/romfs/region_common/actor/`,
  `coop_romfs_override`, picked up as the RomFS is indexed: as the game boots,
  again after Quit to Start Screen): `COOP_COLOR_SITE` then picks each Link's
  set, and nothing is tinted. Only that file is served from there. It isn't
  needed, and the repository doesn't ship it (an edit of a game file). Co-op's
  RomFS files go before a randomizer seed's (an older seed's tunic
  `Playerlink.bfres`).
Red is a deeper crimson (0x8b1a2b) than the game's heart red, so a red
player's hearts don't read as the game's own.

**Walking through each other.** Every character controller gets the other
characters as user capsules in its PhysX sweep (TouchedGeomType 1). With
player collision off, a Link's sweep skips any Link's capsule
(`COOP_CCT_SWEEP_SITE_0/1`, `cct_sweep`). Stacked Links share one spot, so the
check is "is it a Link", not "which Link".

With player collision on, the Links arriving on Player 1's spot pushed each
other apart, and the ones left standing at a door kept Player 1 from going
back in. A player put on Player 1 (a spawn, a door, being brought back,
L3+R3) now passes through the others (`g_pass`) until he walks off the spot
himself: 0.8 from where he was put, clear of every other Link (`pass_tick`).
Until then the sweep skips a capsule where a passing player is, and every Link
capsule in a passing player's own sweep. In side-view rooms (the camera zone
thin in z: `game_side_view`, `widescreen.c`) Links never block each other: one
jumping onto another stood on his head and stuck a second or two.

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
  (`COOP_EXIT_LEAVE_*`) - with `coop_host_exits` off his own touch is kept, so
  then his leaving drops it, as Player 1's does.
- **New rooms.** When Player 1's camera scrolls to a new camera area indoors,
  or he moves more than 10 units in one frame as the view turns side-on or back
  within half a second (stairs into a side-view room, or out of one), the
  others follow him (`room_follow_tick`). A jump alone isn't a new room: put
  back at a room's entry after a pit, everyone else was put on him there
  (checked: Player 1 moved 11 units in Bottle Grotto 08C, Player 2 stays; the
  build before put him on Player 1). That waits for Player 1 to stand
  in the room (state kind 0, no event), then puts each player more than 4
  units away on him once that player is on his own feet. Put on him while the
  stairs still carried him, they were in the floor. A player still falling had
  the game's fall respawn put him back where he fell from, over the move. Each
  player gets at most 10 s. A knocked-out player stands up in a new room
  indoors (`room_changed`).

**Camera areas.** Every Link gets his own camera (`actor+0x318`), and every
camera listens to the camera-area manager, which follows Player 1. A camera
whose `+0x515` is set answers a new scroll area with the "ZeldaScroll" event,
which walks Player 1 on. On the Field the other players' cameras have it set.
In front of the Trendy Game's door this turned Player 1 back on every step, so
only Player 1's camera answers (`COOP_CAMERA_SCROLL_SITE`). The mod renamed the
ZeldaScroll event for this. The recompiled code can't catch the game's
"not found" throw that renaming causes, so `COOP_SCROLL_SITE` leaves the event
alone.

**A-button actions** (lifting, throwing, opening chests, talking, reading).
- An object that can be acted on has a DoAction component. Its update
  (`COOP_DOACTION_SITE`) checks it against "Link" (`+0x4b8`): in reach, in
  front of him, able to act. The best one of the frame goes into one slot,
  which the frame's end (`COOP_DOACTION_FRAME_SITE`) makes the current one:
  what Link's state code acts on when A is pressed (`COOP_DOACTION_GET_SITE`)
  and what the HUD's A label shows.
- With one slot, every player's A acted on what Player 1 could act on, from
  wherever that player stood. In front of something else he got nothing, and
  pressing A in the same moment as Player 1 put him into Player 1's lift, on
  Player 1's rock.
- Now each player has his own slots. The other players' slots are kept in
  coop.c and swapped into the game's while it works on theirs: every
  component is checked once per player, with "Link" being him for the
  check (the game's reads of `+0x4b8` give him: `rt_coop_player`, below); the
  frame's end runs once per player; and another player's own update
  (`COOP_ACTOR_EXEC_*`) acts on his own slot. A component also keeps what its
  last check found (score, "can act"), so each player's findings are put back
  into it while the game works on his.
- One taker per object: when a player's A takes something, it is nothing to
  the others for half a second (`CLAIM_FRAMES`). Two players beside the same
  rock pressing A together don't both lift it.
- Actions that play an event (a chest, a sign, talking: any type but the
  lift, 4, and a lever's pull, 3 - the player's own states) play it on
  "Link", Player 1. The Link whose A starts one waits in
  his "open" state for the event to move him on, which for another player
  never came. So another player's A on one starts the component's action
  itself (`DOACTION_TRIGGER`) and his Link stays as he is: on a press of A made
  while he can act (`a_press_tick`), and as the game's own check allows it
  (`doaction_can`: the component's flags, the countdown after an action). On A
  held, the talk began again as it ended: his A had closed its last text. Items from chests go
  into the shared inventory, and Player 1 holds the item up in the item-get
  scene, whoever opened the chest.
- **Talking.** A talk another player starts plays on Player 1 too, and only
  Player 1's controller moved its text on. While it plays, the talker's A, B
  and directions reach Player 1's input as well (`coop_talker`,
  `runtime/hle/input.c`), so he reads it through and answers its questions
  himself. A talk walks Player 1 up to the one talked to with the walk-in
  state (kind 16), which put everyone on Player 1 as at a door. From an A
  that starts an event (anyone's talk, chest or sign: `talk_started`) until
  that event ends, nobody is moved (`coop_enter`, the walk-in ride in
  `coop_tick`); doors and room changes still bring everyone along.
- **Shops** deal with Player 1: the shopkeeper sells what "Link" carries.
  Player 2 bringing him the bombs got his greeting and was left holding them,
  so the goods (`ObjShopItem`) are nothing to the other players' A. Rupees are
  shared anyway.
- "Link" is only another player for the components' checks. The camera, the
  HUD, events, saving and the camera areas still follow Player 1.

**Events** (`p1_event_tick`). While Player 1 is in an event (walked by a
cutscene, an item held up, a talk), the others get no input
(`coop_input_blocked`, which also blocks the scripted test input). Link's
state kind tells it: slot `+0x4ed`, states at `+0x1340`, kind at the state's
`+0x28`. The event kinds are the ones the game's "can he go" check
(`main+0xd58d70`) holds apart by its mask `0x4ec0000`: 18, 19, 21, 22, 23 and
26 (the check also looks past the kind for some others - 1, 3, 11 - which the
copy doesn't). Before, the others
walked about during Marin's talk with the walrus, and a door's slam played
again for each player walking in.

**Walk-ins.** Through a door and at a dungeon's room change Player 1 is walked
in (Link state kind 16). The game's "can he go" check lets that kind go, so the
others were put on him at its start only and then walked on ahead of him into
the next room. For the whole walk-in they stay on him and get no input, as in
the other events.

**Event boxes.** An `AreaEventBox` (a fight room's lock, a boss's or mini-boss's
start, a cutscene's trigger) is a bare collision shape whose contact calls the
box's fire (`main+0x67210`) for any Link that walks in. Each other player walking
into Lanmola's pit played his start again. In a fight room, another player a step
ahead of Player 1 shut the doors while Player 1 was still in the doorway. The
closing shutter put Player 1 back on the doorway's far side, and the camera and
everyone followed him into the room before, every time (Angler's Tunnel, 08E up
into 07E). Only Player 1's contact fires a box (`COOP_EVENT_BOX_SITE`), or
that of a player standing on his spot (put there by an event's walk or a
door's, not moved since). A box keeps one contact (`+0x370`), the last one
begun, and fires with it the next frame (`main+0x6a980`). Five Links stacked on
Player 1 at the ghost's grave began theirs with the owl's box at once: another
player's was kept and Player 1's was gone, so the box never fired.

**Collapse tiles** (the dungeons' and caves' cracked floor tiles,
`ObjDungeonCollapseTile...`) crumbled under Player 1 only (user 2026-10-30, a
Tal Tal Heights cave, `Tamaranch04`: "the crumbling stone we are by doesn't
crumble for other players"). A tile checks the game manager's player in a job
of its own each frame (main+0xb01a40, x19 the tile), not in the actors' update
where the enemies' "Link" is swapped: within 1.35 across of its middle, the
drop starts (main+0xb005b0). `COOP_TILE_PLAYER_SITE` (the job's
`ldr x8, [x20, #0x4b8]`) gives it the player nearest it instead (`tile_player`;
knocked-out players and ones away from the map aren't taken), and the game's own
check goes by him. The job's earlier check of the player's state (main+0x3ed00,
then his +0x1568) is still Player 1's. `tools/coop_tests/floor.sh`: Player 2 put on a
Catfish's Maw tile falls through it within a second and comes back up at the
pit's edge; before, he stood on it, and walked over one, and it held. The same
in the user's cave (Player 2 put on the tile at 68.25, 21.75). Player 1 on a
tile: the same frames as before.

The levels name their objects cut short, not by kind: a Catfish's Maw tile is
`ObjDungeonCo-99000F005B920361` (an `ObjDungeonCollapseTileLv05`). The first
fix (2026-10-30) matched the kinds' full names and never fired; it also swapped
"Link" during the tile's update, which the tile doesn't read.

**Other floors that crumble** (`AC_FLOOR`: level names starting
`ObjHouseColl`, `ObjKataKataFl`, `ObjMatFaceFa`, `ObjBrokenFlo` - prefixes only
one kind has; `ObjKataKata-` in Turtle Rock could also be the lava button block
`ObjKataKataLava`, so it isn't one) keep the enemies' swap: while one updates
with another player on it (within 1 unit of its middle across, a step up or
down) and Player 1 not, the game manager's player is that player
(`floor_player`), logged once a floor. Not checked in the game.

**Warp holes.** An `AreaWarpHoleEventBox` keeps a player actor whose `+0x43`
bit 6 is clear, then plays the jump and the "Where to?" menu for Player 1,
whoever stepped in. Another player's Link reads as one with the bit set
(`COOP_WARP_HOLE_TOUCH/LEAVE_SITE`). The overworld's warp holes are objects of
their own with the same check (`COOP_WARP_OBJ_TOUCH/LEAVE_SITE`, main+0xcbd3e0 /
main+0xcbd250): another player stepping in set the object's "player inside"
flag, and its update walked Player 1 - everyone riding on him - slowly over to
the hole and jumped him in. A dungeon's warp hole back to its entrance
is an exit, which Player 1 already leads (`COOP_EXIT_TOUCH_SITE`).

**Deaths.** A Link dies by his state machine (`+0x4e0`) changing to the dead
state (0x12; 0x13 is checked with it), whose start plays the game over. Hits on
the other players, and their falls into pits or deep water (`DAMAGE_FIXED_SITE`),
come off their own hearts. With each player's own hearts, another player sent to
the dead state any other way, such as an instant kill, is knocked out instead
(`COOP_DEAD_STATE_SITE`). With "game over for everyone" his last hit is let
through, and with shared hearts a death is the game's own.

**Enemies** (`coop_targets`). While an enemy's update runs
(`COOP_ACTOR_EXEC_SITE` / `_DONE`, enemies told apart by their level name
`Enemy...-<key>`), "Link" is the Link it targets; Player 1 again after.

*How "Link" is given* (`rt_coop_player`, 2026-11-13). The game reads "Link"
from its game manager's player slot (`[GAME_MGR]+0x4b8`). Every 64-bit load
at `+0x4b8` in the main program - 696 in 1.0.1, 531 of them with the game
manager as their base as a pass through each function sees it, the rest with
a base it couldn't follow; no `ldp` reaches it - is listed in
`runtime/game_addresses/coop_player_reads_v1_0_1.txt` (made by
`tools/coop_player_reads.py` from the game's main) and replaced
(`tools/patches.py` `_player_read`) by a call that loads the slot and asks
`rt_coop_player(base, value)`. It gives the Link the updating enemy goes for
(`g_as_link`, set by `actor_exec_begin`, cleared by `actor_exec_end`) only on
the game thread and only when the base is the game manager; any other read -
another thread's, outside an update, co-op off, another object's `+0x4b8` -
gets what the slot holds, Player 1. The 11 sites that are hooks already (the
placement's, the collapse tile's, the Like Like's nine) aren't patched: they
decide for themselves. The slot itself is never written. Until then it was
swapped for the whole update (written with the target, put back after), which
upstream's review (2026-11-12) pointed out a worker job reading "Link" during
an enemy's update would see too. The A-button checks (`doaction_each`) give
"Link" the same way. A read of "Link" outside its enemy's update is answered
at that read: the Like Like's states (`COOP_LIKELIKE_SITE_0..8`: x19 holds the
Like Like, fetched by its state code - the player it goes for) and the
collapse tiles' job (`COOP_TILE_PLAYER_SITE`, x19 the tile: the player nearest
it) - the places that fetch the player for that object, as upstream's review
(item 1) asked, not a swap. Which register holds the object differs at each:
looked for on 2026-11-13, the x0 a read's function was called with (its
`this`) is that object at none of these ten (a state object, overwritten
first), so they can't be told from the code's shape alone. Four of the Zol's
reads (`main+0x799ab4`, `0x79ae3c`, `0x79d96c`, `0x79df00`) are patched reads;
in its update they give its target; they never ran in the tests.

Its components update on their own (the owner at `+0x48`). The one it
reacts to the player with keeps "Link" (`Zombie:ReactionComp +0x278`), and a
zombie walked to Player 1 whoever it targeted. So the swap also covers its
reaction component (the game's `EnemyReactionComponent`: `...ReactionComp`,
`...ReactComp`, and for 28 kinds - Zols, Armos, Pokey, Togezo, Ghini... -
`...ReactionCom`: missed, red Zols hopped at Player 1 whoever they targeted)
and its player follower (`TailorPlayerComp`), not the others
(collision, animation): Player 1's sword hits it whoever it targets. Checked
with the graveyard zombies by Marin and Tarin's house, and a Moblin and a red
Zol west of it with Player 1 17 units away: each goes for the player it
targets. The reaction component also reacts to hits by "Link": it sees the
nearest player (`nearest_player`), not the one the enemy stays on - another
player's hit knocked a turtle back from where its target stood, up onto a
ledge (Angler's Tunnel). A Gel (`EnemyMiniZol`) goes for the player it is on
(`clinger_target`): one that jumped at its target and landed on another player
clung to him, then went over to its target. So do the Zols (`EnemyZolRed`,
`EnemyZolGreen`), which cling too: a red one hopping after Player 1 latched on
Player 2 on the way and was on Player 1 the next moment (user 2026-10-30).
Going for the nearest wasn't enough: a Zol hops a set length, so hopping from
right on Player 2 it landed past him, by Player 1 beside him, who was the
nearest by then (user 2026-10-31; `tools/coop_tests/zol.sh` with Player 2 held 3 units
off). A player it lands on (within `CLING_REACH`, 0.5, on the ground with it)
is its target, kept while he is up and within `TARGET_ENGAGE`; past that,
another player has to be clearly nearer (1.5x). A Zol's or a Gel's reaction
component goes by that target too, not the nearest: it steers the hops, and
each hit on Player 2 knocked him back past Player 1, the nearest then - the
Zol went for Player 1 (user 2026-10-31, "always going on p1 when hitting p2";
`tools/coop_tests/zolhit.sh`: real hits - `tools/coop_tests/zol.sh` has infinite hearts, so none of
the Zols' hits landed). So a hit from another player knocks it back as from its
target's side. The rules for other enemies:
- An enemy targets the nearest player who is up.
- Within `TARGET_ENGAGE` (6 units, about 4 tiles) it keeps him until he is
  knocked out, so an attack, a wind-up or a boss pattern plays out on one
  player - unless, after `TARGET_LOCK`, another player is right at it
  (`TARGET_MELEE`, 2.5 units) and clearly nearer (1.5x). The Armos Knight's
  room is smaller than 6 units across: he kept the player he picked first, in
  the doorway he can't step into, hopping on the floor's edge while the
  players hitting him went ignored (`tools/coop_tests/armos.sh`, `tools/coop_tests/armos_fight.sh`;
  `LARECOMP_COOP_TRACE=EnemyDeguArmos` logs where he goes). Alone he follows
  Link, stops 1 to 1.5 units off him and slams every 10 s or so.
- A player away from the map for a moment (y below -500: fallen through it,
  being moved) isn't targeted.
- Otherwise it keeps him `TARGET_LOCK` (6 s), plus `TARGET_GRACE` (2 s) after
  a fight. After that it changes only to a clearly nearer player (1.5x).
- Enemies more than 30 units from everyone are left alone.

**Blade traps** (`ObjBladeTrap`: named as an object, not an enemy) shot only
at Player 1 lined up with them (user 2026-11-01, Catfish's Maw). They are
`AC_TRAP`: in a trap's update and its reaction part's (`BladeTrap:ReactionCom`),
"Link" is the nearest player within 0.75 of its row or column (`trap_target`),
nobody: Player 1. Checked in Catfish's Maw 07E (a trap in each corner):
Player 2 put in line with the top two, Player 1 in the middle - both go for
Player 2 at once and hit him within 20 frames; the build before went on
going for Player 1, and Player 2 stood in line unhurt.

**Thrown objects.** A lifted object (a pot, a rock, a bomb: an `ACT_LIFT`
action) asks for "Link" in its own update, and flies the way he faces when
thrown: another player's pot flew the way Player 1 faced (user 2026-11-01:
"throwing pots goes backwards"; found by probes on the reads: the pot's
update, main+0xb27da4, through main+0xd56b80). The object another player
lifted last (`doaction_get`: `carried_by`) has him for "Link" while it updates
(`carrier`: by its address and its name, 5 minutes at most). Checked in Marin
and Tarin's house: Player 2 lifts a pot facing west, turns east and throws - it
flies east (before: north, Player 1's way); Player 1 the same: east, as before.

**Like Likes** read "Link" in their state handlers (main+0x4a64c0 to
main+0x4ab1d0, x19 the Like Like), which run outside its update, where "Link"
is its target. One that went for Player 2 hit him, and at the catch
(main+0x4a9810) flew in a straight line onto Player 1, 18 units off, in about
15 frames; Player 2 wasn't caught (user 2026-10-30; `tools/coop_tests/likelike.sh`, Player
2 put a unit from one at Goponga Swamp's phone booth). Its nine reads of the
game manager's player (`COOP_LIKELIKE_SITE_0..8`, `likelike_player`) now get
the player it goes for (`kept_target`: `enemy_target`'s, read only), with
enemy targets on and two or more players; for anything that isn't a Like Like
(by its name, x19) they read as before. Tested: Player 2 is hit, caught (state
kind 23) and held about 3 s, then spat out; the Like Like stays on him, Player
1 doesn't move. The game's own "Did...did that thing eat it?!" follows (the
shield is shared). Player 1 as its target: it walks to him and catches him, as
before.

**Cut grass and thrown rocks.** On the Field the game makes a level object
again (grass, a rock) when its screen comes back into the screens it makes
around Player 1 and its actor is gone: the screens come back fresh, as on the
Game Boy. There is no "destroyed" record in the game: the thrown rock's actor
stays as a dead stub while its screen is kept, and once the screen is let go
(outside the box of screens kept around Player 1) and made again, the spawner
makes a new actor for an entry with none (found by watching the rock's id in
memory, the calls on its actor through its break, and the backtrace of its new
actor's set-up, `main+0xc44a00`). Alone, the screen made again is one well
past the view. With co-op, grass and rocks came back beside the players
(2026-10-28, the user's log: a thrown rock back on the stairs in Player 1's way),
and zoomed out the view had reached past the screens made.
- The view is kept inside the screens made (`zoom_room`, the boxes grown with
  the zoom: "The screens around Player 1"), and the box of screens kept
  reaches every player (`keep_reach`, `COOP_KEEP_BOX_SITE`): a screen anyone is
  in, or can see, isn't let go, so it isn't made again in view. A player can't
  be outside the screens made for long anyway: he falls through and is brought
  back (`lost_tick`).
- Until 2026-11-13 each one destroyed was also remembered by name, and one made
  again near a player or in view was refused at the actor factory
  (`coop_spawn_skip` in `runtime/hle/souls.c`) or killed on its first update
  (`gone_check`). Upstream's review (item 3) asked for the cause instead. With
  the boxes as above it no longer acted: in the scripted runs of 2026-11-13
  (`rock_run.sh`, `zoom_solo_grass.sh`, 4-player and 2-player runs on the
  Field) it kept nothing gone, and with it taken out none of the grass and
  rock actors made again was in view or near a player (1,433 in all). It is gone, and
  `souls.c` is upstream's again. `LARECOMP_TRACK_RESPAWN=1` still logs each one
  made again, with SEEN when in view or near a player. Not replayed: the user's
  Tal Tal Heights save, where broken boulders flashed back at a screen's edge
  (2026-10-30; `rock_flash.sh` needs it).

**Hearts** (`coop_health`).
- **Shared:** the game's one set, as without co-op.
- **Each player's own:** Player 1 keeps the game's hearts (PlayerStatus `+0x84`)
  and its HUD; the others' hearts are in coop.c. The damage patches
  (`DAMAGE_*` in `tools/patches.py`) call `rt_coop_damage_of(c, rt_damage(...),
  link)`, and a hit on players 2.. comes off their own hearts. The Link hit is
  the one the game's code holds: the enemy-hit function's x21
  (`COOP_DAMAGE_HALVED_SITE`, or the third lsl site where a version has no
  halving site), the fixed hits' (a pit, deep water) x19, the second lsl
  site's function (`main+0xd96830`) x20; the first lsl site is an event's
  "damage" command (`main+0xd7c870`) with no Link, Player 1's, as every event
  plays on him. Until 2026-11-13 the Link was found by scanning the
  callee-saved registers for one (`running_player`), which upstream's review
  pointed out nothing guarantees. Checked: Player 2 held in Angler's Tunnel's
  spikes is hit 19 times in 25 s and Player 1 never; the Like Like's hit on
  Player 2 is his.
- **Halved hits:** an enemy's hit (the third `DAMAGE_LSL_SITES`) is halved by
  the game after it is computed: flag 0x6f (`main+0x8d1a30`) or PlayerStatus
  `+0x9c` negative (the Blue Clothes, a Guardian Acorn). Both ways meet at an
  `ldrh w8, [x8, #0x84]` (`COOP_DAMAGE_HALVED_SITE`) with the hit in w20, and the
  split runs there, so every player's hit is halved as Player 1's is. Taken at
  the lsl before that, Player 1 lost a quarter heart where the others lost a
  half (user 2026-11-02: "P1 takes hardly any damage, and other players take
  full damage"; it was also behind 2026-11-01's "way more damage than p1").
  Checked: the seed save's spear Moblin, every hit a quarter on both players
  (before: Player 2 a half). Player 1's hits are logged too now.
- **Gains:** hearts the game's set gains go to every player who is up: a
  heart, a fairy, a potion, a container. `COOP_HEAL_SITE_0/1` read the uncapped
  total, and `PLAYER_MAX_HEALTH` is re-read.
- **Out of hearts** (`coop_knockout`): knocked out (the default) or a game over
  for everyone.
- **Knocked out:** the player lies in the `down_lp` animation (`PLAY_ANIMATION`
  on his event component, played again after each of his own updates) with a
  breathing "Zzz" bubble over him. Another player holding A beside him (within
  1.6, height too: in a side view a player swimming above him had the prompt)
  for `coop_revive_s` seconds revives him with 30% of the maximum and 2 s
  of no damage (`REVIVE_SAFE`). A player holding A there stands still (his
  stick and d-pad read 0: `coop_reviving`). A hit on him stops it
  (`revive_hit`): the progress is lost, and his A counts again only once he
  lets go of it. An "(A) Revive" prompt shows under the knocked-out player
  while someone who is up is beside him and nobody holds A
  (`coop_revive_prompt`; under the one who could revive him, it went along
  as he walked about), its button as the game's
  prompts show Switch A (`confirm_glyph`: an Xbox pad's B, a PlayStation pad's
  circle by default); two on one spot are stacked,
  each moved below the one it overlaps at most once (moved while it was
  within `h` of it, `y + h` could still test within it by a rounding: the HUD
  looped forever and the game locked up with two players by a knocked-out
  one).
- **Knocked out in spikes:** each spike hit knocked him back (kind 11), the
  knockback ended on the spikes and the next began - he never lay still. A
  knocked-out player isn't knocked back again (`dead_state`: a change to a
  state of kind 11 stays undone; the hit that knocked him out plays out), and
  three hits that would cost him hearts, each within 1.5 s of the last
  (`DOWN_HITS`, `DOWN_HIT_FRAMES`; the spikes hit every 30 to 60 frames), put
  him on Player 1's spot
  (`down_move_tick`): his state changed to the first of kind 0 through the
  game's own change (`COOP_DEAD_STATE_SITE`'s function), then taken off the
  map half a second and put on Player 1 as a player lost below the world is.
  Moved while knocked back or falling - the collision's position, or every
  copy of it a standing Link keeps (one of those held a pointer then: written
  over, the game crashed) - his state put him back in the spikes. For 2 s
  after, hits don't count (the spikes' last contact followed him), and one
  already on Player 1's spot isn't moved. Checked in Angler's Tunnel's spike
  room (06B): Player 2 knocked out in the spikes is put on Player 1 once and
  lies there; the build before kept him bouncing in them (kinds 11 and 3).
  Hits with an enemy within 8 units of him in the last half second don't
  count either (`down_enemy_note`, as enemies update): they do him nothing,
  and put on Player 1 he was put by the enemy Player 1 fought - the Armos
  Knight's knocked-out players went back and forth every few seconds. Until
  2026-11-01 two hits within 3 s did it, and a knockback with nothing off
  (cancelled for him) counted too: players knocked out by a wall in Catfish's
  Maw, a Spark passing every few seconds, were put on Player 1 four times in
  15 s, and again from there (user: "still moving down-but-not-out players
  around"). Re-checked in the spike room: knocked out, hit three times in
  half a second, put on Player 1.
- **Hit again at once:** one contact came off another player's hearts two to
  four times in the same moment (a third of all their hits in a session: user
  2026-11-01, "other players seem to be taking way more damage than p1"; a
  blade trap's one hit, four times). The cause (found 2026-11-13): the game's
  enemy-hit function takes the hit off its hearts and then, only for a hit of
  1 or more (`main+0xd4c788`: `cmp w24, #1; b.lt`), runs what follows a hit
  for the Link hit - his controller's rumble and the time he can't be hit
  again (his `+0x1e80`). Another player's hit, kept off his own hearts, was 0
  to the game, so nothing stopped the same contact hitting him again on the
  next frames. Now (`COOP_DAMAGE_HALVED_SITE`, case 49) the game sees the
  hit's real size and reads its hearts as that much more, so it takes nothing
  off them, and each Link gets his own time unhurt as Player 1 does. Until
  then another player's hit within 30 frames of his last cost nothing
  (`HURT_GAP`), which hid it; it is gone (upstream's review, item 3).
- **Getting up:** a hit that knocks a player out leaves him in the knocked-back
  state (kind 11), which never ended under the lying pose, so a revived player
  couldn't move until a door reset him. The pose waits for his state to be
  back at kind 0 (`sleep_tick`), and isn't played again after his update while
  it isn't (`coop_actor_updated`).
- **Left behind:** a knocked-out player stands up in a new room indoors, and
  after 30 s out of the camera's view (`LEASH_DOWN`), when he is also put on
  Player 1.

**HUD** (`runtime/ui/coop_hud.mm`).
- **Layout:** a "P1" tag beside the game's hearts, and the other players'
  heart rows in a corner (`coop_hud`), with no panel behind them, in their
  colours (`coop_heart_colours`, which tints Player 1's game hearts too:
  `texpatch.c`).
- **Corners:**
  - 3 rows (4 with 7 other players) go in the chosen bottom corner, the rest
    in the other one,
    from its bottom up (Player 5 in the corner, 6 above him), in the corner
    itself. While the game's "Now saving." note shows - for 2 s after a save
    (`g_save_frame`, set by `fs.c`'s commit; a warp's autosave showed it from
    20 to 70 frames after) - they move above it.
  - Top right is a single column below the X/Y items, the rupee count and the
    compass.
  - Bottom right moves up above the "Now saving" note and the run timer.
- **Zzz bubbles:** projected from Player 1's camera (`host+0x318`, else the
  one seen at `COOP_CAMERA_SITE`), with the zoom's field of view. A player
  out of view has his bubble pinned to the screen's edge on his side, with a
  rim in his colour (`player_screen`). The revive bar is green on a dark
  track, so it shows on the white player's bubble. The HUD (the UI thread)
  projects with Player 1's Link and the view camera as the frame starts
  (`g_hud_host`, `g_hud_cam`, set in `coop_tick`), not as the game thread has
  them mid-update (an enemy's update points the player slot at its target).
- **Hidden** on the pause menu's Items and Map pages.

**Camera zoom** (`coop_zoom`, `coop_camera_fit`, `coop_zoom_fit`, `zoom_tick`).
The zoom `g_zoom` is how many times further the view reaches. The base is 1.0
to 1.6 in 0.1 steps (Camera zoom, Normal to Out 60%; default Out 50%). With
"fit everyone" on, the camera zooms out further until every player who is up
is inside 70% of the view (`back_need`), up to "Fit up to" (Out 10% to Out
200%: 1.1 to 3.0; default Out 100%, 2.0 - upstream's review asked for the
default near 2x; past it the overworld makes and draws much more, for a strong
PC, as the option's description says), eased in and out, and never closer than the
base. Out of play it is 1. The sky and sea margin (`rt_skysea_margin`) grows
with it (`coop_camera_reach()`).
- **On with co-op, always, or off** ("Zoom out", `coop_zoom_mode`, `zoom_on`):
  With co-op (the default) zooms with 2 players or more, as before, so a game
  alone keeps the game's own camera. Always zooms playing alone too: of the
  co-op hooks only the zoom's run then (`alone_hook`: `COOP_CAMERA_SITE`, the
  three boxes of screens, the shadow map's box; the actors' updates only for
  `LARECOMP_COOP_TRACE` / `LARECOMP_TRACK_RESPAWN`). With no other player
  fit adds nothing (its rows show only with co-op). Off: 1 with co-op too. The
  section is shown without co-op ("Camera", under Players); hidden on 1.0.0.
  Switched off in play the zoom goes to 1 at once (eased, the view widened for
  seconds over screens no longer made), and `COOP_CAMERA_SITE` runs once more
  alone to put the camera's own values back.
- **Who counts:** players who are up, not one lost below the world (parked at
  y −1000 he took the zoom and the boxes of screens to their largest).

**On the Field the camera is moved back** (`camera_back`, at
`COOP_CAMERA_SITE`, right after the camera's view matrix is set). The view
camera's eye (its node, camera `+0x510`, position at `+0xb0`; the culling pass
builds its own view from it) goes back along the view's back axis by Player
1's distance from it times `g_zoom - 1`, and so do the view matrix (`+0x580`)
and the renderer's copy of it (`x24 +0x20`). The eye's move is taken off
again as the next frame starts (`camera_back_undo`, from `coop_tick`, before
the camera's update): a camera that goes on from where its eye is (the walk in
through Angler's Tunnel's door glides it) went further back every frame, to
120 away, the walk-in dragged on and the others stood stuck above the door. With it, by the same distance (each the game's
own value plus the move, the game's own again at 1, and put back to the game's
own as the next frame starts too):
- the far plane (`+0x56c`): at 3x the far ground was cut off, black;
- the shadows' reach (`+0x560`, `+0x350`, `+0x734`, 24 each): past it a wedge
  of the ground was all in shadow;
- the tilt-shift's focus (`qol.c` `rt_tilt_focus_add`: the post-process
  object's `+0x40` and `+0x80`, 26.5): Player 1 was blurred and the band shown
  sharp sat near the screen's bottom.

These are kept for the camera they're in (`g_back_cam`): when the view camera
changes, the last one gets its own values back first (`camera_back_put`), and
so does the camera as play ends. Read again from moved values, the move added
up each time it was the view again. They are put back as each frame starts
(`camera_back_undo`) as well: the camera eases some of them itself, and from
the moved values it settled short of where it eased them to every frame. An
event waiting for the camera to get there waited for ever: at the ghost's
grave the owl flew off, the camera came back to Player 1 and he stayed in the
event (user 2026-10-30; replayed from the user's save, `tools/coop_tests/grave.sh`:
stuck at zoom 1.5 every time, finished at 1 and with this).

A view matrix the game hasn't set again since it was moved isn't moved again
(`g_m14_wrote`): only what the frame's start took off (the eye, the kept
values) is put back. While the pause menu is up the game leaves the matrix as
it was, and moved again each frame it went (zoom - 1) x 24.8 further back a
frame - 12 units at 1.5, thousands by the time a file was loaded from the
menu, and the game hung (user 2026-10-30, File 3). Unseen until then: the menu
covers the view, and closing it sets the matrix again. As old as the camera
moved back (the build before the grave's fix did the same: `tools/coop_tests/ic.sh` with
"2400:PLUS:8", the `[coop] frame` lines' view depth).

**The shadow map's box** (`COOP_SHADOW_BOX_SITE`, `shadow_box`). The game
draws one 2048 x 2048 shadow map a frame (a depth-only pass), and a
screen-space pass (704 x 396) looks each pixel up in it through a matrix in
the scene's constants. main+0x100a460 fits the map: it takes a world box
around Player 1 from the view (`+0x200` min, `+0x210` max: about 34 x 16 x 33
on the Field, set again each frame), turns its corners into the light's space
and makes their bounds the light's projection; main+0x1009f30 then makes the
lookups' matrix from it (the renderer's "View" `+0x2a0`). Zoomed out, the view
reached past the box: the shadows at its edge ran on as dark diagonal stripes
across the far ground (user 2026-10-29, past about 2.3 in a 3840x2071 window),
and past it nothing cast one. The hook (on the fit's first instruction, for
cascade 0, the only one fitted) grows the box across and deep by `g_zoom`, as
much as the view grows, so the map's texels cover as much of the screen as at
1 (its height stays). A second fit in the same frame, the game not having set
the box since, doesn't grow it again. It costs nothing measurable: the game
draws every caster into the map anyway (checked 2026-10-30 at Martha's Bay,
1920x1035: the same draws a frame with and without it at 2.0, 2.6 and 3.0,
GPU time within 0.2 ms either way, 60 fps). Checked clean at 1.6, 2.0, 2.3,
2.6 and 3.0, and at 2.6 in a 16:9 window; the far trees now cast shadows too.
Found from the lookups' matrix (the same at 1 and 2.6, about 47 units across
in the light's space), the object holding it and the functions storing there.

The first version widened the field of view instead (`rt_fovy` multiplies its
tangent by `coop_camera_zoom()`, the culling pass's too), which is still what
**indoors** uses. On the Field its top edge ran toward the horizon: each step
out needed far more of the world made and drawn (a box of 2 for 2.5, more
than 3 for 3), and at 2.5 the game's own thread took 13 ms a frame (game
code, single-threaded: the GPU took 2.6) and its frame pacing held 30 fps.
Moved back, the view grows evenly: 3x took the game's thread 6.6 ms with the
game's own box, 11 ms with a player 40 away (the boxes grown, the active
screens taken out to him).

**The screens around Player 1** (`COOP_LOAD_BOX_SITE`, `COOP_KEEP_BOX_SITE`,
`COOP_ACTIVE_BOX_SITE`, `zoom_room`, `box_tick`, `active_box`). A level's
screens (on the Field 15 x 12 units each, `Field_01A.leb` .. `Field_16P.leb`)
are handled each frame by main+0x85c280, with three boxes centred on Player 1:
- A screen meeting the first gets its objects made: tall grass, grass, rocks,
  trees, enemies, and its ground (`MapStatic`, one per screen). The size comes
  from a table by the level's kind (main+0x1701e40; the Field, kind 3: 75 x
  60, which is 7 x 7 screens).
- One outside the second (90 x 72) loses them again.
- Only those meeting the third are active: their ground solid, their objects
  running (actor flag `+0x40` bit 1). It's much smaller (main+0x1701ed4 by the
  screen's type, plus a screen around another point: about 4 x 3 screens).

Found by backtracing a tall grass actor's constructor while Player 1 walked
into a new screen (to the loader's queue, then the queue's filler), by
moving Player 1 across a screen's middle on a set frame (a column is made
when its middle comes within 45, not at the screen's edge), and by watching
which `MapStatic` lost bit 1 under a player who fell through.

That caused two things. Zoomed out, the view reached past the screens made:
the ground there shows bare earth where tall grass goes, dark ovals (the
user's "weird shadows" zoomed far out), the rest is missing, and further out
is the sky's black. And another player in view, one screen past the active
ones (north of Player 1 most of all, where the view reaches furthest), fell
through the ground and was taken off the map. With co-op on the Field:
- **The active box takes in every player** (`active_box`), and the screen
  around each, so a player stands wherever the view lets him be. Making every
  screen made active instead held 30 fps at 2.5 (13 x 13 screens' objects
  running). Only in play, and only players inside the screens made: as a file
  loads the Links stand at the map's origin or where they were in the level
  before, and taken in, the box woke much of the Field at once and the game ran
  out of memory (`[heaps] standard allocation FAILED`; the user's File 3
  loaded from Angler's Tunnel, four players).
- **The first two boxes grow with the zoom** (`g_box`, 1 to `BOX_MAX` 2 in
  0.25 steps, `box_tick`): the smallest that covers the zoom wanted, grown at
  once, shrunk after 5 s of wanting less. Without a camera or Player 1 yet (a
  file loading) they stay as they are: "no room" read as "too small", and the
  load made 13 x 13 screens.
- **The zoom stays inside the screens made** (`zoom_room`): points along their
  edge (the screens the first box meets, less 2 for a tall grass patch's
  reach), on the ground at Player 1's height, are put through the view, and the
  zoom is held below the one at which the first comes into view, at once, not
  eased. It uses the box whose screens are made by now (`g_box_ready`, 90
  frames after it grew; they come about 40 frames on), so as players spread the
  zoom stops for a moment (2.5 with a player 40 away), then goes on.

The dark ovals still seen far out at Ukuku Prairie are holes in its ground:
they're there at the normal view.

**The co-op hooks' own cost.** The actor update hooks (`COOP_ACTOR_EXEC_*`)
run for every actor and component every frame, about 21,000 a frame with the
screens made out far. Reading each name there cost the game thread about 2 ms
a frame; what an actor is now is worked out once (`actor_class`, a 4-way table
by address and the name's start), the grass and rocks' check only for names
starting `Obj`, the game thread check once per thread: 1.5 ms left at 2.5
(with the hooks off, measured: 11.1 ms; on, 12.6).

Measured again on 2026-11-13 (i7-13700K, the seed's start on the Field,
standing; the game thread's CPU time a frame from `LARECOMP_LOG_PACE=1`; the
co-op hooks' calls counted, and one call in 256 of every hook timed):

| | game thread CPU a frame | co-op hook calls a frame | a hook call |
|---|---|---|---|
| alone, zoom off | 1.36 ms | 7,662 | about 31 ns (0.24 ms a frame) |
| alone, "Zoom out" Always (1.5) | 1.72 ms | 7,640 | |
| 4 players, zoom 1 | 1.75 ms | 6,226 | |
| 4 players, zoom 2 | 2.95 ms | 6,363 | about 79 ns (0.5 ms a frame) |
| 4 players, zoom 3 | 4.82 ms | 8,230 | |

All held 60 fps. Alone, with co-op off, the hooks still cost about 0.24 ms a
frame of the game thread (their sites run, and return at once) - about 1.4%
of a 60 fps frame here; a slower CPU pays more. Deciding that co-op is off is
cheap (the count is kept: `rt_coop_players`); the cost is the hook sites'
dispatch itself - the call from the game's code into `rt_hook`, the site's
lookup (`hook_id`'s cache), the switch. Taking it away with co-op off would
need the code generator to check a flag at each site before the call (a
change to every hook, upstream's `tools/gen.py`): not done. The zoom is the larger cost:
3x took the game thread 2.8 times as long as 1x with 4 players, so the
default fit is now 2x (Out 100%). Not measured on a weaker machine (an M1
Air, a phone) as upstream's review asked: none was at hand.

Effects drawn as a mesh around the camera, sized for the game's own view, showed
their edge once the view was wider: the Mysterious Woods fog as a hard-edged
oval. `widescreen_effect_stretch` already widened their viewport for wide
windows; it now also stretches it by the zoom, both ways (`exec.c`,
`renderer.m`): the fog ring, haze layer and light shafts of the woods, and the
dungeon darkness. Checked at Out 60% and 2.0, 16:9, 21:9 and 32:9: no edge
(`LARECOMP_NO_ZOOM_STRETCH=1` shows the oval it was). With the camera moved
back the woods' fog is a mesh around Player 1, not the camera: stretched by the
zoom alone its edge showed in the window's corners (its near side shrinks more
than its middle), so moved back it is stretched by 1 + 1.6 (zoom - 1). Checked
at 2 and 3: no edge. Yarna Desert's warm haze is a disc around Player 1 too
(fragment shader `16c48ff5e6bcfea3`, found by skipping shaders): moved back its
edge showed as a hard circle on the sand. It refracts the picture behind it, so
stretched it ghosted a copy of the rocks over the water beside the desert; moved
back past 1.05 it isn't drawn (`widescreen_effect_hidden`, both renderers).
Checked at Out 50% and 3: no circle, no ghosting. Nor is the woods' fog seen
from outside them (`39ae199ddc1701dc`: a box of fog over their edge, 18
vertices reading the depth, not drawn inside, where the ring, haze and shafts
are): sized for the game's view from beside the woods, moved back its sides
showed - a flat blue band with hard edges over the woods from Mabe Village
(user 2026-10-30; found as the one draw of the pass between the scene and its
lighting, `LARECOMP_DEBUG_FRAME`). Checked at (55, 107) at 2.6: the woods
clear, no band.

**Indoors** (any level but the Field: houses, caves, dungeons) the zoom is
"Indoor zoom" (`coop_zoom_indoor`, Normal by default), without fit. The game
draws only the room its camera frames, one screen: zoomed out, every dungeon
room and house sat small in black, most houses with their backdrop's glow a hard
circle (`tools/coop_tests/zoom_audit.sh`, 2.0 at 40 places). The interior backdrop
(`BACKDROP_HASHES`) is stretched by the zoom both ways, so a raised Indoor zoom
shows the game's own soft glow out to the edges. Options weighed for indoors:
- **A, the game's view (the default):** rooms are framed to fit the screen.
- **B, zoom capped to the room:** the same as A; every room is one screen.
- **C, the outdoor zoom everywhere:** small rooms in black.
- **D, a setting of its own:** "Indoor zoom", Normal to Out 60%.

What the audit saw outdoors at 2.0: no effect with an edge; at the island's
east coast (Tal Tal's east, Yarna) the sea ends in the far corner from about
1.7 (where the world ends).

**Bring back players off screen** (`coop_leash`, `leash_tick`). A player out of
Player 1's view for 3 s (`LEASH_FRAMES`) while standing (kind 0), or for 10 s
whatever he is doing (`LEASH_ANYWAY`), is put on Player 1. A knocked-out one
waits 30 s (`LEASH_DOWN`), then is revived and put on him. Nothing happens
while Player 1 is in an event. A player more than 30 units away who isn't
brought back is logged every 10 s with the reason.

**Players lost below the world** (`lost_tick`). On the Field only the screens
around Player 1 have solid ground (see "The screens around Player 1": with
co-op the active ones now take in every player, so this is for a player who
gets past the screens made). Another player past them (left behind, or
running off) falls through, and the game puts him back on the
spot he fell from, where he falls again, for good. At Out 50% he is still in
view, so bring-back never takes him, and a teleport doesn't stick while he
falls (the game sets his position). A player whose height drops 8 below where
he last stood (a ledge is a few units) is lost and taken off the map, and 2.5 s
on (`LOST_AWAY`) put on Player 1, with bring-back on or off (the user's call:
gone for a moment, then back with Player 1, rather than seen falling and
popping up). While he falls he's below the ground; as the game stands him up
where he fell from, he's parked out of sight (y −1000, his update not running)
until it's time, then put on Player 1 standing, which a teleport needs.
Teleports (`ride_on_host`) also set the Link's safe spot (`Link+0x1b40`, where
the game puts him back after a fall). Only on the Field: elsewhere the ground
is all there, and swimming down or a long ladder in a side-view room dropped a
player 8 too. Forgotten at a spawn and as play ends (`lost_forget`).

Where the ground is solid (the active screens) was found later the same day:
see "The screens around Player 1". It explains what was seen before: on Ukuku
Prairie another player stood 30 units east and 20 west of Player 1, while at
Tal Tal Heights idle players fell a few steps from him as he crossed into the
next screen.

**The save's restart point** (`COOP_RESTART_SITE`). A file saved with
another player far away restarted Player 1 on that player's spot, and one
saved with him fallen through the ground loaded with Link falling forever at
the map's origin (the user's two files, 2026-10-28: restart point y -7). Found
on 2026-11-13, as upstream's review (item 2) asked:
- The save copies the restart point (file `+0x559c`) from the player
  entity's object: the game manager's `+0x4a8` is the player entity,
  `main+0x11b70` gives its object (`+0x88`), and `main+0xdfc710` ->
  `main+0xdfcb90` copies the object's `+0x680` (the spot, snapped to 0.75)
  when its `+0x694` is set, else `+0x660` (the arrival point).
- Each Link's update (`main+0xd355c0`, x19 the Link) looks up that object
  (`ldr x1, [x20, #0x4a8]` at `main+0xd35778`, then `main+0x11b70`) and,
  standing on safe ground, writes its own spot into `+0x680` (and its facing
  `+0x690`, `+0x694` set). Every Link did, so it was the last one updated:
  the first one made (Player 1's is made last). Found by watching the save
  buffer's `+0x559c` being written (`LARECOMP_WATCH_ADDR`), then the object's
  `+0x680`.
- At `COOP_RESTART_SITE` another player's Link (or one left over, co-op off
  too) finds no entity, and the game's own path for no object (`cbz` at
  `main+0xd35788`) skips that part of his update: only Player 1's keeps the
  restart point, as alone. Checked with `tools/coop_tests/savetest.sh`: 2
  players, Player 1 at 197.62 and Player 2 9 units away: the file holds
  197.25 (Player 1's spot, snapped); 3 players, Player 3 (the first made) away
  and Player 1 still: 194.25, Player 1's.
- Until then, while the pause menu was open, the other Links' 19 position
  copies were held on Player 1's spot (`hold_on_p1`, `k_pos_copies`, found by
  value) and their updates skipped: that is gone.

**The game's countdown** (Dampé's timed chamber challenges, `Dg_TimeCount`)
sits in the bottom left, where the hearts go. With co-op on, its pane
`N_Time_00` moves to the bottom centre. That is 4 bytes of
`region_common/ui/Game.arc`, found by name (`coop_romfs_patch`) and laid over
the game's reads of it by the RomFS index's patch list, while co-op is on
(the patch's `on`).

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
| COOP_CAMERA_SITE | 0x7000010908 | ldr x8, [x23, #0x2a8] | camera for the bubbles; moved back on the Field (17) |
| COOP_HEAL_SITE_0 / _1 | 0x70008d0a5c / 0x70008a39d4 | strh w8, [x9, #0x84] | hearts gained (18, 19) |
| COOP_EXIT_TOUCH_SITE | 0x7000076f5c | ldrb w9, [x23, #0x43] | door touch (20) |
| COOP_STAIRS_TOUCH / LEAVE_SITE | 0x70000617e0 / 1640 | ldrb w8, [x8, #0x43] | stairs (21, 22) |
| COOP_EXIT_LEAVE_SITE / _CLEAR | 0x7000076aa0 / acc | stp x20, x19 / bl main+0x76b20 | door contact ends (23, 24) |
| COOP_CAMERA_SCROLL_SITE | 0x70000bbdcc | ldrb w8, [x19, #0x515] | only Player 1's camera scrolls (25) |
| COOP_DOACTION_SITE | 0x70000ea240 | sub sp, sp, #0x60 | a DoAction component checked for each player (26) |
| COOP_DOACTION_FRAME_SITE | 0x70000e8be0 | stp x20, x19, [sp, #-0x20]! | each player's current DoAction (27) |
| COOP_DOACTION_GET_SITE | 0x70000e8d90 | stp x20, x19, [sp, #-0x20]! | one taker per object (28) |
| COOP_EVENT_BOX_SITE | 0x7000067210 | str x19, [sp, #-0x20]! | an event box fires for Player 1 only - or a player on his spot (29) |
| COOP_WARP_HOLE_TOUCH / LEAVE_SITE | 0x700008c5ac / c410 | ldrb w8, [x8, #0x43] | warp holes (30, 31) |
| COOP_DEAD_STATE_SITE | 0x7000d34660 | sub sp, sp, #0x30 | another player's death is a knock-out (32) |
| COOP_LOAD_BOX_SITE | 0x700085c430 | adrp x23 | the box whose screens are made, grown with the zoom (33) |
| COOP_KEEP_BOX_SITE | 0x700085c628 | ld1 {v0.s}[1], [x23] | the box whose screens are kept, the same (34) |
| COOP_ACTIVE_BOX_SITE | 0x700085c848 | fsub s8, s0, s3 | the active screens' box takes in every player (35) |
| COOP_WARP_OBJ_TOUCH / LEAVE_SITE | 0x7000cbd42c / d290 | ldrb w8, [x8, #0x43] | the overworld's warp hole objects (36, 37) |
| COOP_SHADOW_BOX_SITE | 0x700100a460 | cbnz w1 | the shadow map's box grown with the zoom (38) |
| COOP_TILE_PLAYER_SITE | 0x7000b01c70 | ldr x8, [x20, #0x4b8] | a collapse tile measures the player nearest it (39) |
| COOP_LIKELIKE_SITE_0..8 | 0x70004a65f4, a7040, a7fb8, a89f4, a8ce8, a9400, a9898, aac54, ab1f0 | ldr x22/x21/x9/x8, [x20 or x21, #0x4b8] | a Like Like's "Link" is the player it goes for (40-48) |
| COOP_DAMAGE_HALVED_SITE | 0x7000d4c750 | ldrh w8, [x8, #0x84] | an enemy's hit split after the game halves it (49) |
| COOP_RESTART_SITE | 0x7000d35778 | ldr x1, [x20, #0x4a8] | a Link's update looks up the player entity, for the save's restart point: only Player 1's finds it (50) |

Runtime constants: `PLAYER_VTABLE`, `PLAYER_MAX_HEALTH`, `PLAY_ANIMATION`,
`DOACTION_CURRENT` / `_PENDING` / `_COOLDOWN` (the slots), `DOACTION_TAKE_RET` /
`_CHECK_RET` (who asks), `DOACTION_TRIGGER` (an action started)
(`runtime_v1_0_1.h`). The `case` numbers follow the order of the `COOP_*`
entries in the `HOOK_SITES` list in `runtime/hle/hooks.c`. Add new sites at its
end and in both `patches_v1_0_*.h` tables, in the same order.

## Settings (Controls tab)

Right under Inputs (`coop_options` in `runtime/ui/controls_ui.mm`): with co-op
off, the Players row and the Camera heading (`zoom_options`; its zoom rows
only with "Zoom out" Always). On, four headings:

| Heading | Row | Key | Default | |
|---|---|---|---|---|
| Co-op | Players | coop_players | 1 | 1 = off, 2..8 Links (from the next file loaded); greyed out on 1.0.0 |
| | Tunic colors | | | shown only when the file is missing or comes with Quit to Start Screen |
| | Player N color | coop_colours | 0,1,2,... | each player's colour set (Green, Red, Blue, Yellow, Purple, Pink, Black, White); its description names his controller; red when he has none or shares the colour, with a note after the label saying which ("no controller", "same as Player 2") |
| | Player N controller | coop_pad_path_N | (none) | choose, then press a button on the controller for that player; swapped with whoever had it, remembered by device path |
| Co-op hearts | Hearts | coop_health | 1 | 0 shared, 1 each player's own |
| | Out of hearts | coop_knockout | 0 | 0 knocked out until revived, 1 game over for all (own hearts) |
| | Revive time | coop_revive_s | 4 | seconds of holding A to revive (knocked out) |
| | Other players' hearts | coop_hud | 0 | 0 bottom left, 1 bottom right, 2 top right (own hearts) |
| | Hearts in player colors | coop_heart_colours | 1 | hearts in each player's colour, Player 1's too (own hearts) |
| Camera | Zoom out | coop_zoom_mode | 0 | 0 with co-op (2+ players), 1 always (alone too), 2 off; shown without co-op |
| | Camera zoom | coop_zoom | 5 | the least zoom, 10% steps out: 0 normal .. 6 out 60% |
| | Zoom out to fit everyone | coop_camera_fit | 1 | further out to keep every player in view (shown with co-op) |
| | Fit up to | coop_zoom_fit | 10 | the furthest fit goes, 10% steps: 1 out 10% .. 20 out 200% (3x as far); default out 100% (2x) |
| | Indoor zoom | coop_zoom_indoor | 0 | houses, caves, dungeons: 0 normal .. 6 out 60%, no fit |
| Co-op rules | Bring lost players back | coop_leash | 1 | players off screen are brought back to Player 1 |
| | Only Player 1 triggers exits | coop_host_exits | 1 | only Player 1 leads everyone out |
| | Enemies target the nearest player | coop_targets | 1 | enemies go after the nearest player |
| | Players block each other | coop_collision | 0 | the Links block each other |

The tunic colours are fixed when the game boots (the RomFS index): after
turning co-op on, Quit to Start Screen and play again (checked: the colours
load). Turned off, every Link wears the game's own tunic from the file until
then, and the countdown is back in its corner.

## Tests

Address tables: `python tools/test_game_addresses.py`, and with game files
`--game v1_0_1=<extracted>` checks every hook's instruction word. It also checks
that a version with co-op has every `COOP_*` address (one left at 0 drops its
hook), and `tools/version_map.py` keeps the 1.0.1 tables' own values for what
1.0.0 doesn't have (0 there). The scripted runs named below are in
`tools/coop_tests/` (its README: the save seed they start from, hiding real
controllers); their logs and frames go to `build/`.

Test switches (environment):

| Variable | |
|---|---|
| `LARECOMP_COOP=<n>` | the player count, without touching the settings |
| `LARECOMP_COOP_LOG=1` | `[coop]` lines: Links made and placed, hits, knock-outs, exits, target changes, positions every 30 frames |
| `LARECOMP_COOP_DOWN=<p>,...` | those players knocked out 2 s after they are placed |
| `LARECOMP_COOP_DOWN_AT=<frame>` | with `LARECOMP_COOP_DOWN`: at that frame instead (after a warp) |
| `LARECOMP_COOP_DIE=<p>,...` | those players sent to the dead state 3 s after they are placed |
| `LARECOMP_COOP_HP=<p>:<q>` | player p's hearts set to q quarters 2 s after he is placed |
| `LARECOMP_COOP_AT=<frame>:<n>` | the player count changed to n at that frame, as from the menu (late controllers) |
| `LARECOMP_INPUT_P2..P8=<frame>:<button>:<frames>,...` | scripted input for players 2..8, as `LARECOMP_INPUT` |
| `LARECOMP_COOP_MOVE=<frame>:<p>:<x>:<y>:<z>[:hold]` | player p put there at that frame (left far away); `:hold` keeps him there |
| `LARECOMP_COOP_ZOOM=<factor>` | the camera zoom forced (`tools/coop_tests/zoom_audit.sh`) |
| `LARECOMP_COOP_TRACE=<name prefix>[,<another>...]` | `[trace]` lines: each actor so named, its position every 30 frames (a part's: its owner's), and the player it goes for whenever that changes (alone too); an enemy and its parts: `EnemyZolRed,ZolRed:` |
| `LARECOMP_COOP_TRACE_EVERY=<n>` | with `LARECOMP_COOP_TRACE`: every n frames instead |
| `LARECOMP_NO_ZOOM_STRETCH=1` | camera-attached effects not stretched by the zoom (to see what it fixes) |
| `LARECOMP_TRACK_RESPAWN=1` | `[respawn]` lines: a rock or grass actor made again under its name (came back), where, Player 1's distance and SEEN when near a player or in view; with co-op, or alone with "Zoom out" Always |

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
- **Triggers:** Angler's Tunnel 08E up into the fight room 07E with 3 players,
  all at once and one at a time; Lanmola's pit with the others walking in
  first; a field warp hole and a dungeon warp hole with Player 2 on it, then
  Player 1.
- **Enemies:** spear Moblins changing targets only after their lock.
- **This round** (`tools/coop_tests/full_check.sh` runs the triggers, doors, A-button,
  side-view and HUD checks in turn):
  - Tail Cave loaded twice: one set of extras, no copies.
  - Players turned on at the start screen (`LARECOMP_COOP_AT`): every Link
    moves.
  - A real hit knocking Player 2 out, then a revive: he walks again.
  - Player 2 left behind in the open: back on Player 1 after 3 s; knocked out,
    revived and back after 30 s.
  - Stairs into a side-view room: the others arrive on Player 1, on the floor.
  - Each zoom setting, and the bubble pinned to the screen's edge.
  - Run again on commit 44 (2026-10-31): the same, but for the spear Moblins'
    random knock-outs at the seed's start and the hits on knocked-out players
    now logged ("knocked out, unhurt"); a knocked-out player hit with an
    enemy near stays where he is (no longer "put on Player 1"). The revive
    prompt is under the knocked-out player.
- **Saves and lost players** (2026-10-28):
  - `tools/coop_tests/savetest.sh` saves through the pause menu in a scratch save folder
    (`LARECOMP_SAVE_DIR`): with Player 2 away, the file holds Player 1's spot,
    and it loads with Link standing there.
  - A player in an area that isn't loaded falls and is put back on Player 1
    (one screen away, Field 12D from 11D; idle players left at a spawn).
  - Bring-back stopping in play: a falling player at Out 50% was "in view" for
    it; `lost_tick` takes him now.
  - Grass and rocks coming back beside Player 1 in play: not reproduced in
    scripts (with players near, far, falling, or teleported onto Player 1; the
    old creation order and the new). Both things that went wrong around it in
    the user's session (another player stuck falling far away, taken as "the
    player" by the pause menu) are fixed. `LARECOMP_TRACK_RESPAWN=1` logs any
    rock or grass that comes back. (Found later the same day, from the user's
    log: see "Cut grass and thrown rocks" and its test below.)

**ImGui checks.** ImGui's checks are upstream's own (`IM_ASSERT`, ImGui's
error recovery): the fork's app-wide change (failed checks logged and let
through, recoverable errors logged) was taken out on 2026-11-13, as upstream's
review asked. The one check that had failed - `IsNamedKey(key)` in
`GetKeyData`, in the Controls tab with co-op on - is Dear ImGui 1.91.9's own:
a controller's West button (Xbox X; `ImGuiKey_NavGamepadMenu`) starts its
window switching and sets `NavWindowingToggleLayer`; a keyboard key then makes
the keyboard the input source, and the Alt-toggle code asks
`IsKeyReleased(g.NavWindowingToggleKey)` with no toggle key
(`ImGuiKey_None`). Several players pressing buttons while one uses the
keyboard made it likely in co-op; upstream's build has it too. Dear ImGui
fixed it in 1.92.0 (#8525); `third_party/imgui/imgui.cpp` has the guard
(that code only for a toggle an Alt key started), marked "LAS Recomp". The
cause was found by reading ImGui's code; a scripted run (menu open, West held,
Enter) didn't make the check fail, so the fix isn't confirmed by one.
- **A-button actions** (`tools/coop_tests/interact_check.sh`): Player 2 lifting and
  throwing a rock and a pot, opening a chest (the seashell at Field 07M),
  reading a sign (Field 04P),
  Player 1 lifting with co-op on and off, two players pressing A together
  at one rock, and a player away from Player 1 pressing A as he lifts.
- **Pushing a block:** Player 2 pushes Tail Cave 05C's block a tile, in the
  game's push state (kind 6), as Player 1 does: walking into a block is each
  Link's own state code, nothing of "Link" (`+0x4b8`).
- **Talking and shops** (Mabe's shop, `Shop:Shop_01A`): Player 1 buys the
  bombs (lift, B at the shopkeeper, Buy: 10 rupees). Player 2's A on the
  goods does nothing. Player 2 talks to the shopkeeper, closes the talk with
  his own A, and stays where he stood.
- **Cut grass and thrown rocks** (a copy of the user's save, at Marin and
  Tarin's house): Player 2 throws the stairs' rock, Player 1 cuts the grass
  beside the stairs, walks up into the next screen and back. Both were made
  again twice and kept gone each time, and Player 1 walked back down the
  stairs (before, he stood against the rock). At 3x (`LARECOMP_COOP_ZOOM=3`)
  after a warp to Field_09E: nothing destroyed and nothing kept gone (before
  the unload fix, 155 tall grass and rocks were, in view).
- **The screens around Player 1 and the camera moved back** (Field_09E, Out
  60% with fit up to Out 200%):
  - Player 2 held 40 units east: the zoom stops at 2.5 while the grown box's
    screens are made, then goes on to 3.0; 60 fps, the game's thread 11 ms; the
    far ground drawn, Player 1 sharp, no missing grass in view.
  - Player 2 dropped 40 units north (a screen whose ground the game had
    switched off): he lands and stands; at 40 he's just past the 3x view, so
    bring-back takes him after 3 s. (With the widened view: 28 north, in view
    at 2.3, he stayed.)
  - Player 2 dropped 38 units east with bring-back off: he lands and stands
    (before, he fell through and was taken off the map).
  - The Mysterious Woods (Field_06C) at 2 and 3, Tal Tal (Field_02B) at 3: no
    fog edge; the sky and sea drawn.
  - The game's own boxes, measured: the ground pieces active with Player 1
    standing still, a 4 x 3 block; ×5 the third box, 7 x 4; the first two
    ×2, 13 x 13 screens made.
  - The widened view, before: 30 fps at 2.4 and 2.5 (box 2), the game's thread
    13 ms; moved back, 2.5 and 3 with the game's own box: 6.3 and 6.6 ms.
- **Enemy targets:** Player 2 six units from Player 1 among the graveyard
  zombies. Each zombie walks to the player it targets and Player 2 is hit by
  his. Player 2 17 units from Player 1 beside a Moblin and a red Zol: both
  take him and he is hit.
- **Player 1's talk:** Player 1 talks to the shopkeeper twice while Player 2
  stands across the shop. Player 2 stays where he is, and still comes in
  with Player 1 through the door.
- **Co-op off:** unchanged with "Zoom out" at With co-op (the default).
- **"Zoom out"** (`tools/coop_tests/zoom_solo.sh`, frame 2600 of the save seed's Tal Tal
  Heights and Marin and Tarin's house): alone at With co-op the game's own
  view, alone at Always the Field as co-op's Out 50%, two players at Off the
  game's own view, alone at Always with Indoor zoom Out 30% the room zoomed
  out (its backdrop stretched), and at With co-op the game's own room. Alone
  at Always and Out 60% (`tools/coop_tests/zoom_solo_grass.sh`), spin attacks in tall
  grass south of Mabe Village and a walk away and back: what he cut is noted,
  nothing came back in view.
- **The boulder carried past a screen's edge** (`tools/coop_tests/rock_run.sh`, the
  user's "picking up the boulder and running past slightly" 2026-10-29): at
  Tal Tal Heights' start, two players and alone at Always, Player 1 lifts the
  boulder, runs east past the edge and back: it isn't made again, and none of
  the 44 rocks and grass made again was near a player or in view (47 to 58
  units away, `[respawn]`). Not reproduced on this build; the user's report
  likely came from the build before the evening round's fix.
- **Upstream's controller tests** (`tools/test_active_pad.c`,
  `tools/test_rumble.c`: both include `runtime/hle/input.c`): build and pass at
  each of the commits, with co-op cases: two players and three pads, pads
  plugged in after the count changed (each with its own open: let go, it stays
  open), a quick tap on Player 2's pad, and player 2's rumble on his own pad
  (mounted only while he has one).
- **The evening round** (user playtest 2026-10-28):
  - Revive prompts (`tools/coop_tests/hud_pair.sh`): 4 players on one spot, Player 2
    knocked out, the others stepping round him, at the user's 3840x2071 window:
    three prompts stacked, presenting to the end. (The old loop's rounding was
    shown in a test program - at 3840x2071 it trips for about 7% of prompt
    positions below y 1024 - not landed on in a scripted run.) Over the warp
    hole's "Where to?" map neither bubble nor prompt shows.
  - Command memory: with a test build capping every buffer without a handler
    to 4 KB of the game's memory, 70 s at 3x: four buffers (64 KB to 4 MB) went
    on in host memory, no error, the picture as before, 60 fps.
  - Warp hole objects: Players 3 and 4 stepping onto the Martha's Bay warp
    hole: "only Player 1 jumps in", Player 1 not in an event; Player 1 stepping
    on it: the jump and "Where to?".
  - The lever in Angler's Tunnel (by 02E): Player 2 under its handle, A held,
    down: his state 7 (the pull), the lever drawn out with him, the blocks
    moving; Player 1 where he was.
  - Enemy parts (`LARECOMP_COOP_LOG`, each kind once): `ZolRed:` /
    `ZolGreen:` / `Ghini:` / `Armos:` / `AnaMonea:EnemyReactionCom` among those
    that go by the target, their owners the enemies.
  - Yarna Desert (Field_13P) at Out 50% and 3: no circle, no ghosting.
  - The zoom logged every frame (a test build), players spread at Ukuku
    Prairie (to 2.5) and at Martha's Bay, the map's edge (to 3): no snap in
    play, only as a level loads.
  - A copy of the user's File 3 loaded with 4 players: all stand, no
    allocation failure.
  - The follow-up checks, each also run on the build before the round
    (`EXE=` in `tools/coop_tests/run_test.sh`): Angler's Tunnel's spikes (above); the
    Angler Fish's room - Player 3 held 4 below a knocked-out Player 2: no
    prompt; beside him: the prompt; a side-view room with collision on,
    Player 2 dropped onto Player 1's head: through to the floor at once (the
    build before: on his head 1.5 s); the dungeon's door walked out of and in
    with 4 players at Out 50%, and the fight room's door with collision on: in
    both builds everyone through, nobody left on the door (the user's "stuck
    above the door frame" not reproduced); the Spiked Beetle's reaction part
    among those that follow the target (`Togezo:EnemyReactionCom`). A Gel
    (a split red Zol) wasn't brought about by a script.
  - (The pause menu's hold, which wrote the other Links' position copies, is
    gone: see "The save's restart point".)

## Not done or not checked

- **Only the screens around Player 1 are made** on the Field (see "The screens
  around Player 1"). Another player who walks past them falls through and is
  put back on Player 1 (`lost_tick`): he's out of view by then.
- **The camera moved back** was checked on the Field at Field_09E, Field_06C
  and Field_02B, and in play only by scripts: not yet in cutscenes on the
  Field or with the sound (the listener may be the camera). At 3x the shadows
  left a thin darker wedge at the window's far left edge (seen before the
  shadow map's box grew with the zoom, likely the same cause; not looked for
  there since). At the overworld's
  edges (Martha's Bay, Yarna Desert) far out, the view reaches past the map:
  black beyond it (the zoom isn't held back there; it didn't snap there).
- **Slower PCs:** 60 fps at 3x was on an i7-13700K (the game's thread is the
  limit: single-threaded game code).

- **Tunic colours on Metal** (macOS, iOS): the same call as Vulkan's
  (`renderer.m`), not run: checked on Windows (Vulkan) only.
- **Manbo's warps and falls to a lower floor** weren't examined. Manbo's warp
  points are the song's arrivals, with no touch of their own: another player
  playing the song with his ocarina may still warp everyone.
- **Instant kills:** none was found to try. The dead-state change was checked
  with `LARECOMP_COOP_DIE` for Player 2 (knocked out); sent that way Player 1
  doesn't die, so the game over itself wasn't seen.
- **A-button actions:** event actions other than chests, signs and talks are
  routed the same way (any type but the lift and the pull), unchecked.
- **Not seen in the game:** a Gel clinging (a split red Zol wasn't brought
  about by a script), the Spiked Beetle knocked back by another player's hit
  (its reaction part is now followed: seen in the log), the user's other
  Links "stuck above the door frame" (not reproduced at Angler's Tunnel's
  doors), the camera zooming in and out fast (not reproduced; no snap in
  play logged frame by frame).
- **Enemy kinds** besides zombies, Moblins and Zols weren't checked. One
  whose chasing is in a component not named `...React...` or `...PlayerComp`
  would still go to Player 1. Bosses other than the Armos Knight weren't
  checked, and the Armos Knight only in scripted fights
  (`tools/coop_tests/armos_fight.sh`). A Like Like's grab is checked (`tools/coop_tests/likelike.sh`)
  on the Field only (Eagle's Tower's weren't tried).
- **Other crumbling floors** (`AC_FLOOR`: Catfish's Maw's broken floor, a
  house's collapse tiles, the Face Shrine's falling floor) weren't checked in
  the game; the collapse tiles are (`tools/coop_tests/floor.sh`).
- **Controller rows** (`coop_pad_path_N`) have no test: whether a real pad's
  SDL path stays the same across restarts and reconnects wasn't checked.
- **Red Zols "jumping only for Player 1"** (user 2026-11-01, Catfish's Maw)
  wasn't reproduced: in the Mysterious Forest and in Catfish's Maw 06E (the
  user's Zols, still in the floor) the Zol by Player 2 went to him and stayed
  on him, on this build and the one before. Four reads of "Link" in the Zol's
  code (main+0x799ab4, 0x79ae3c, 0x79d96c, 0x79df00) never ran in those tests;
  they aren't hooked.
- **Grass back in Animal Village** (user 2026-11-01): the log has no cut grass
  made again there (only one kept gone in view); not reproduced.
- **Zols on a knocked-out player:** one that lands on him stays and hits him
  (nothing comes off); it isn't moved off.
- **The top of the map zoomed out:** the user saw "slight texture glitching"
  there (2026-10-31); not reproduced at Field_01K_a up to 3x, panning.
- **Holding A to revive** also does the game's A action (beside a rock, the
  rock is lifted). The revive goes on.
- **Shared hearts:** stacked Links hit by one attack each cost hearts.
- **The moved countdown** was checked by its bytes, not seen in a timed
  challenge.
- **Not played with extras:** minigames (the Trendy Game's crane, the Rapids
  Ride, fishing) and cutscenes.
- **Left as they are (2026-10-29 audit):**
  - A walk-in (kind 16) is told from a talk's only by `g_talking` (an A that
    started an event, or 60 frames): a talk an NPC starts would put the others
    on Player 1 each frame of its walk, and a door within 60 frames of an A that
    started nothing leaves them at the door (the room follow brings them).
  - Host command memory (an out-of-memory buffer with no handler) is freed 8
    frames after the buffer gets the game's memory again: a list recorded there
    and kept to be called or sent again later would read freed memory. The
    game's own memory is reused the same way, so none is expected.
  - The co-op `Game.arc` patch and `Playerlink.bfres` are chosen as the RomFS is
    indexed (boot, Quit to Start Screen); the countdown's patch follows co-op
    on and off after that only where the game reads the file again.
  - `COOP_SCROLL_SITE` is placed and does nothing (removing it renumbers the
    sites after it).
