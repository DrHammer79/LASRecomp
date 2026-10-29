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
while the extras are made (`COOP_DUP_SITE`). Player 1 has to be last: the
scene's camera is the last Link made's. Made first, he got the save right (see
"The pause menu" below) but the camera showed the ground somewhere else.

The creator can run again for one area: a dungeon loaded fresh, a door's
failed entry. The extras it made the first time were still alive, so every run
added another set: copies of a player standing about, moving with his
controller. Each Link the creator makes is remembered (`g_made`). A run takes
back a live one that no player holds before making a new one. One that is left
over anyway is put out of the way (under the map) and its update is skipped
(`COOP_ACTOR_EXEC_SITE`).

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

The game reads which controllers are connected when the npad's "style set
changed" event is signalled. A controller turned on after the game had read
them (at the start screen) gave a Link that never moved. Those events are kept
(`hid_bind_style_event`), and `input_style_changed()` signals them again
whenever the player count changes out of play.

**Colours.** `COOP_COLOR_SITE` picks each Link's tunic frame from the mod's
`Playerlink.bfres`: 24 colour frames, 3 per colour set. It is served from
`<data root>/coop/romfs/region_common/actor/Playerlink.bfres` when that file
exists (`coop_romfs_override`, `runtime/services/fs.c`). The repository can't
ship it: it is the mod's edit of a game file. Without it the game still plays,
and every Link wears Player 1's tunic. The file is picked up when the RomFS is
indexed, as the game boots (again after Quit to Start Screen). The Controls tab
says when it is missing, with the path it goes to (`coop_colours_path`), or
when co-op was turned on after the game started and it comes with Quit to
Start Screen (`coop_colours_state`). Before the game has started, nothing is
shown: the colours come as it starts.
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
  (`COOP_EXIT_LEAVE_*`).
- **New rooms.** When Player 1's camera scrolls to a new camera area indoors,
  or he moves more than 10 units in one frame (stairs into a side-view room),
  the others follow him (`room_follow_tick`). That waits for Player 1 to stand
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
  component is checked once per player, with `+0x4b8` pointing at him for the
  check; the frame's end runs once per player; and another player's own update
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
  itself (`DOACTION_TRIGGER`) and his Link stays as he is. Items from chests go
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
- `+0x4b8` is only moved for the components' checks. The camera, the HUD,
  events, saving and the camera areas still follow Player 1.

**Events** (`p1_event_tick`). While Player 1 is in an event (walked by a
cutscene, an item held up, a talk), the others get no input
(`coop_input_blocked`, which also blocks the scripted test input). Link's
state kind tells it: slot `+0x4ed`, states at `+0x1340`, kind at the state's
`+0x28`. The event kinds are the ones the game's "can he go" check
(`main+0xd58d70`) holds apart: 18, 19, 22, 23, 24 and 26. Before, the others
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
into 07E). Only Player 1's contact fires a box (`COOP_EVENT_BOX_SITE`).

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
`Enemy...-<key>`), `+0x4b8` holds the Link it targets; it is Player 1's again
after. Its components update on their own (the owner at `+0x48`). The one it
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
ledge (Angler's Tunnel). A Gel (`EnemyMiniZol`) goes for the nearest player
throughout: one that jumped at its target and landed on another player clung
to him, then went over to its target. The rules:
- An enemy targets the nearest player who is up.
- Within `TARGET_ENGAGE` (6 units, about 4 tiles) it keeps him until he is
  knocked out, so an attack, a wind-up or a boss pattern plays out on one
  player.
- Otherwise it keeps him `TARGET_LOCK` (6 s), plus `TARGET_GRACE` (2 s) after
  a fight. After that it changes only to a clearly nearer player (1.5x).
- Enemies more than 30 units from everyone are left alone.

**Cut grass and thrown rocks** (`gone_check`, `gone_note`). On the Field the
game makes a level object again (grass, a rock) when Player 1 steps into its
screen and its actor is gone: the screens come back fresh, as on the Game Boy.
Alone, what Link cut or threw is in the screen he just left, out of view. With
co-op the others cut and throw in screens Player 1 isn't in, and the view
shows the screens around. Grass and rocks came back beside the players, and a
thrown rock came back on the stairs in Player 1's way (the user's log: the
stairs' rock, `ObjRock-4800A5005B88BEDC`, back as he stepped onto them).
- Each one destroyed is remembered by its actor's name, the level object's id
  (`ObjRock`, `ObjGrass`, `ObjGrass2x2`, `ObjLawn`, `ObjSkullStone`; its hash
  worked out once per actor, `actor_class`). Destroyed: actor `+0x40` bit 30
  set by its own update (cut, or thrown and broken), or set before it with a
  player within `GONE_TAKER` (4 units: lifted, cut by another player's sword).
  The screens the game unloads as Player 1 moves on kill theirs between
  updates, far from everyone: those come back whole. Counted as destroyed, the
  tall grass around was kept gone at the zoom's wide views, bare earth in its
  place.
- Only those destroyed take a slot (`GONE_SLOTS` 4096; when they're all taken
  the one destroyed longest ago makes way). The first version took one for
  every one seen: walking the Field filled the table, and from then on nothing
  was kept (the user's session: 2 kept, cut grass and a thrown boulder back).
- One made again while a player is within `GONE_NEAR` (10 units) of it or
  it's in view is killed before its first update (`ACTOR_KILL`, as its own
  break does): it isn't drawn and stays gone.
- One made again away from everyone is left: it grew back unseen, as alone.
- A level change forgets them all (through a door, everything is back, as
  alone). Not indoors: a room comes back whole as Player 1 walks in, with
  everyone.
- There is no "destroyed" record in the game to change: the thrown rock's
  actor stays as a dead stub, its level entry never changes, and the spawner
  makes a new actor for an entry with none. Found by watching the rock's id
  in memory, the calls on its actor through its break, and the backtrace of
  its new actor's set-up (`main+0xc44a00`).

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
  1.6, height too: in a side view a player swimming above him had the prompt)
  for `coop_revive_s` seconds revives him with 30% of the maximum and 2 s
  of no damage (`REVIVE_SAFE`). An "(A) Revive" prompt shows under a player
  who can revive someone (`coop_revive_prompt`); two on one spot are stacked,
  each moved below the one it overlaps at most once (moved while it was
  within `h` of it, `y + h` could still test within it by a rounding: the HUD
  looped forever and the game locked up with two players by a knocked-out
  one).
- **Knocked out in spikes:** each spike hit knocked him back (kind 11), the
  knockback ended on the spikes and the next began - he never lay still. A
  knocked-out player isn't knocked back again (`dead_state`: a change to a
  state of kind 11 stays undone; the hit that knocked him out plays out), and
  two such hits within 3 s (`DOWN_HITS`) put him on Player 1's spot
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
  - Up to 3 rows go in the chosen bottom corner, the rest in the other one.
  - Top right is a single column below the X/Y items, the rupee count and the
    compass.
  - Bottom right moves up above the "Now saving" note and the run timer.
- **Zzz bubbles:** projected from Player 1's camera (`host+0x318`, else the
  one seen at `COOP_CAMERA_SITE`), with the zoom's field of view. A player
  out of view has his bubble pinned to the screen's edge on his side, with a
  rim in his colour (`player_screen`). The revive bar is green on a dark
  track, so it shows on the white player's bubble.
- **Hidden** on the pause menu's Items and Map pages.

**Camera zoom** (`coop_zoom`, `coop_camera_fit`, `coop_zoom_fit`, `zoom_tick`).
The zoom `g_zoom` is how many times further the view reaches. The base is 1.0
to 1.6 in 0.1 steps (Camera zoom, Normal to Out 60%; default Out 50%). With
"fit everyone" on, the camera zooms out further until every player who is up
is inside 70% of the view (`back_need`), up to "Fit up to" (Out 10% to Out
200%: 1.1 to 3.0; default 3.0), eased in and out, and never closer than the
base. Out of play it is 1. The sky and sea margin (`rt_skysea_margin`) grows
with it (`coop_camera_reach()`).
- **On with co-op, always, or off** ("Zoom out", `coop_zoom_mode`, `zoom_on`):
  With co-op (the default) zooms with 2 players or more, as before, so a game
  alone keeps the game's own camera. Always zooms playing alone too: of the
  co-op hooks only the zoom's run then (`zoom_hook`: `COOP_CAMERA_SITE`, the
  three boxes of screens, and on the Field the actors' updates for cut grass
  and thrown rocks only - zoomed out, the screen Link just left is still in
  view, so what he cut there is kept gone as with co-op). With no other player
  fit adds nothing (its rows show only with co-op). Off: 1 with co-op too. The
  section is shown without co-op ("Camera", under Players); hidden on 1.0.0.

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
own value plus the move, the game's own again at 1):
- the far plane (`+0x56c`): at 3x the far ground was cut off, black;
- the shadows' reach (`+0x560`, `+0x350`, `+0x734`, 24 each): past it a wedge
  of the ground was all in shadow;
- the tilt-shift's focus (`qol.c` `rt_tilt_focus_add`: the post-process
  object's `+0x40` and `+0x80`, 26.5): Player 1 was blurred and the band shown
  sharp sat near the screen's bottom.

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
  once, shrunk after 5 s of wanting less.
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
Checked at Out 50% and 3: no circle, no ghosting.

**Indoors** (any level but the Field: houses, caves, dungeons) the zoom is
"Indoor zoom" (`coop_zoom_indoor`, Normal by default), without fit. The game
draws only the room its camera frames, one screen: zoomed out, every dungeon
room and house sat small in black, most houses with their backdrop's glow a hard
circle (`build/zoom_audit.sh`, 2.0 at 40 places). The interior backdrop
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
the game puts him back after a fall).

Where the ground is solid (the active screens) was found later the same day:
see "The screens around Player 1". It explains what was seen before: on Ukuku
Prairie another player stood 30 units east and 20 west of Player 1, while at
Tal Tal Heights idle players fell a few steps from him as he crossed into the
next screen.

**The pause menu** (`hold_on_p1`). The pause menu takes "the player's" spot as
it opens, for its map and for a save's restart point (file `0x559c`), from the
first Link made: another player's. Not through the actors' name map (its
"Player" entry repointed at Player 1 didn't change the file), nor from a pointer
kept anywhere scanned, nor at save time (every copy of his position on Player
1's while paused didn't change it either). A file saved with him far away,
fallen through the ground, loaded with Link falling forever at the map's origin
(the user's two files, 2026-10-28: restart point y −7). So when Player 1
presses + or −, every copy of the other Links' positions that their components
keep (`k_pos_copies`, found by value) is set to Player 1's for the pause, their
updates skipped meanwhile, and put back once Player 1's update runs again after
the pause (or 8 frames on, if no menu came). Checked with `build/savetest.sh`:
saved with Player 2 9 units away, the file holds Player 1's spot, and after the
menu closes Player 2 is back on his own spot, walking.

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
| COOP_CAMERA_SITE | 0x7000010908 | ldr x8, [x23, #0x2a8] | camera for the bubbles; moved back on the Field (17) |
| COOP_HEAL_SITE_0 / _1 | 0x70008d0a5c / 0x70008a39d4 | strh w8, [x9, #0x84] | hearts gained (18, 19) |
| COOP_EXIT_TOUCH_SITE | 0x7000076f5c | ldrb w9, [x23, #0x43] | door touch (20) |
| COOP_STAIRS_TOUCH / LEAVE_SITE | 0x70000617e0 / 1640 | ldrb w8, [x8, #0x43] | stairs (21, 22) |
| COOP_EXIT_LEAVE_SITE / _CLEAR | 0x7000076aa0 / acc | stp x20, x19 / bl main+0x76b20 | door contact ends (23, 24) |
| COOP_CAMERA_SCROLL_SITE | 0x70000bbdcc | ldrb w8, [x19, #0x515] | only Player 1's camera scrolls (25) |
| COOP_DOACTION_SITE | 0x70000ea240 | sub sp, sp, #0x60 | a DoAction component checked for each player (26) |
| COOP_DOACTION_FRAME_SITE | 0x70000e8be0 | stp x20, x19, [sp, #-0x20]! | each player's current DoAction (27) |
| COOP_DOACTION_GET_SITE | 0x70000e8d90 | stp x20, x19, [sp, #-0x20]! | one taker per object (28) |
| COOP_EVENT_BOX_SITE | 0x7000067210 | str x19, [sp, #-0x20]! | an event box fires for Player 1 only (29) |
| COOP_WARP_HOLE_TOUCH / LEAVE_SITE | 0x700008c5ac / c410 | ldrb w8, [x8, #0x43] | warp holes (30, 31) |
| COOP_DEAD_STATE_SITE | 0x7000d34660 | sub sp, sp, #0x30 | another player's death is a knock-out (32) |
| COOP_LOAD_BOX_SITE | 0x700085c430 | adrp x23 | the box whose screens are made, grown with the zoom (33) |
| COOP_KEEP_BOX_SITE | 0x700085c628 | ld1 {v0.s}[1], [x23] | the box whose screens are kept, the same (34) |
| COOP_ACTIVE_BOX_SITE | 0x700085c848 | fsub s8, s0, s3 | the active screens' box takes in every player (35) |
| COOP_WARP_OBJ_TOUCH / LEAVE_SITE | 0x7000cbd42c / d290 | ldrb w8, [x8, #0x43] | the overworld's warp hole objects (36, 37) |

Runtime constants: `PLAYER_VTABLE`, `PLAYER_MAX_HEALTH`, `PLAY_ANIMATION`,
`DOACTION_CURRENT` / `_PENDING` / `_COOLDOWN` (the slots), `DOACTION_TAKE_RET` /
`_CHECK_RET` (who asks), `DOACTION_TRIGGER` (an action started)
(`runtime_v1_0_1.h`). The `case` numbers follow the order of the `COOP_*`
entries in the `HOOK_SITES` list in `runtime/hle/hooks.c`. Add new sites at its
end and in both `patches_v1_0_*.h` tables, in the same order.

## Settings (Controls tab)

Right under Inputs (`coop_options` in `runtime/ui/controls_ui.mm`): with co-op
off, one row. On, four headings:

| Heading | Row | Key | Default | |
|---|---|---|---|---|
| Co-op | Players | coop_players | 1 | 1 = off, 2..8 Links (from the next file loaded); greyed out on 1.0.0 |
| | Tunic colors | | | shown only when the file is missing or comes with Quit to Start Screen |
| | Player N color | coop_colours | 0,1,2,... | each player's colour set (Green, Red, Blue, Yellow, Purple, Pink, Black, White); its description names his controller; red when he has none or shares the colour |
| Co-op hearts | Hearts | coop_health | 1 | 0 shared, 1 each player's own |
| | Out of hearts | coop_knockout | 0 | 0 knocked out until revived, 1 game over for all (own hearts) |
| | Revive time | coop_revive_s | 4 | seconds of holding A to revive (knocked out) |
| | Other players' hearts | coop_hud | 0 | 0 bottom left, 1 bottom right, 2 top right (own hearts) |
| | Hearts in player colors | coop_heart_colours | 1 | hearts in each player's colour, Player 1's too (own hearts) |
| Camera | Zoom out | coop_zoom_mode | 0 | 0 with co-op (2+ players), 1 always (alone too), 2 off; shown without co-op |
| | Camera zoom | coop_zoom | 5 | the least zoom, 10% steps out: 0 normal .. 6 out 60% |
| | Zoom out to fit everyone | coop_camera_fit | 1 | further out to keep every player in view (shown with co-op) |
| | Fit up to | coop_zoom_fit | 20 | the furthest fit goes, 10% steps: 1 out 10% .. 20 out 200% (3x as far) |
| | Indoor zoom | coop_zoom_indoor | 0 | houses, caves, dungeons: 0 normal .. 6 out 60%, no fit |
| Co-op rules | Bring lost players back | coop_leash | 1 | players off screen are brought back to Player 1 |
| | Only Player 1 triggers exits | coop_host_exits | 1 | only Player 1 leads everyone out |
| | Enemies target the nearest player | coop_targets | 1 | enemies go after the nearest player |
| | Players block each other | coop_collision | 0 | the Links block each other |

The tunic colours and the countdown move are fixed when the game boots (the
RomFS index): after turning co-op on or off, Quit to Start Screen and play
again (checked: the colours load).

## Tests

Address tables: `python tools/test_game_addresses.py`, and with game files
`--game v1_0_1=<extracted>` checks every hook's instruction word.

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
| `LARECOMP_COOP_ZOOM=<factor>` | the camera zoom forced (`build/zoom_audit.sh`) |
| `LARECOMP_NO_ZOOM_STRETCH=1` | camera-attached effects not stretched by the zoom (to see what it fixes) |
| `LARECOMP_TRACK_RESPAWN=1` | `[respawn]` lines: a rock or grass actor made again under its name (came back) |

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
- **This round** (`build/full_check.sh` runs the triggers, doors, A-button,
  side-view and HUD checks in turn):
  - Tail Cave loaded twice: one set of extras, no copies.
  - Players turned on at the start screen (`LARECOMP_COOP_AT`): every Link
    moves.
  - A real hit knocking Player 2 out, then a revive: he walks again.
  - Player 2 left behind in the open: back on Player 1 after 3 s; knocked out,
    revived and back after 30 s.
  - Stairs into a side-view room: the others arrive on Player 1, on the floor.
  - Each zoom setting, and the bubble pinned to the screen's edge.
- **Saves and lost players** (2026-10-28):
  - `build/savetest.sh` saves through the pause menu in a scratch save folder
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

**ImGui checks.** `IM_ASSERT` (`third_party/imgui/imconfig.h`) calls
`imgui_check_failed` (`runtime/ui/menu.mm`), which logs the failed check and a
backtrace once per place instead of stopping the game with a dialog. One was
seen in play with co-op on and its cause isn't known yet: the log line names it.
- **A-button actions** (`build/interact_check.sh`): Player 2 lifting and
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
- **Co-op off:** unchanged.
- **The evening round** (user playtest 2026-10-28):
  - Revive prompts (`build/hud_pair.sh`): 4 players on one spot, Player 2
    knocked out, the others stepping round him, at the user's 3840x2071 window:
    three prompts stacked, presenting to the end. (The old loop's rounding was
    shown in a test program - at 3840x2071 it trips for about 7% of prompt
    positions below y 1024 - not landed on in a scripted run.) Over the warp
    hole's "Where to?" map neither bubble nor prompt shows.
  - Command memory: every buffer without a handler capped to 4 KB of the
    game's memory (a temporary test switch), 70 s at 3x: four buffers (64 KB to
    4 MB) went on in host memory, no error, the picture as before, 60 fps.
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
  - The zoom logged every frame (a temporary switch), players spread at Ukuku
    Prairie (to 2.5) and at Martha's Bay, the map's edge (to 3): no snap in
    play, only as a level loads.
  - A copy of the user's File 3 loaded with 4 players: all stand, no
    allocation failure.
  - The follow-up checks, each also run on the build before the round
    (`EXE=` in `build/run_test.sh`): Angler's Tunnel's spikes (above); the
    Angler Fish's room - Player 3 held 4 below a knocked-out Player 2: no
    prompt; beside him: the prompt; a side-view room with collision on,
    Player 2 dropped onto Player 1's head: through to the floor at once (the
    build before: on his head 1.5 s); the dungeon's door walked out of and in
    with 4 players at Out 50%, and the fight room's door with collision on: in
    both builds everyone through, nobody left on the door (the user's "stuck
    above the door frame" not reproduced); the Spiked Beetle's reaction part
    among those that follow the target (`Togezo:EnemyReactionCom`). A Gel
    (a split red Zol) wasn't brought about by a script.
  - The pause menu's hold writes every position copy of the other Links
    (`k_pos_copies`, found on a standing Link) whatever their state - written
    for good, as the spikes' first move did, one of them knocked back held a
    pointer and the game crashed. Held: 16 pauses with Player 2 bouncing in
    the spikes (kinds 11 and 3), 16 with him knocked out asleep - no crash
    (his update doesn't run while held, and every copy is put back).

## Not done or not checked

- **Only the screens around Player 1 are made** on the Field (see "The screens
  around Player 1"). Another player who walks past them falls through and is
  put back on Player 1 (`lost_tick`): he's out of view by then.
- **The camera moved back** was checked on the Field at Field_09E, Field_06C
  and Field_02B, and in play only by scripts: not yet in cutscenes on the
  Field or with the sound (the listener may be the camera). At 3x the shadows
  leave a thin darker wedge at the window's far left edge. At the overworld's
  edges (Martha's Bay, Yarna Desert) far out, the view reaches past the map:
  black beyond it (the zoom isn't held back there; it didn't snap there).
- **Slower PCs:** 60 fps at 3x was on an i7-13700K (the game's thread is the
  limit: single-threaded game code).

- **Tunic colours** need the mod's `Playerlink.bfres`. Recolouring the tunic
  at run time (as the cosmetics do) would remove the file and the restart.
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
  would still go to Player 1. Bosses weren't checked.
- **Holding A to revive** also does the game's A action (beside a rock, the
  rock is lifted). The revive goes on.
- **Shared hearts:** stacked Links hit by one attack each cost hearts.
- **The moved countdown** was checked by its bytes, not seen in a timed
  challenge.
- **Not played with extras:** minigames (the Trendy Game's crane, the Rapids
  Ride, fishing) and cutscenes.
