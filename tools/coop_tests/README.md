# Co-op test runs

Scripted runs of the dev build for local co-op (`runtime/hle/coop.c`, [docs/coop.md](../../docs/coop.md)). Each boots
the game straight into a save with `LARECOMP_NO_LAUNCHER=1`, drives every player with the test switches
(`LARECOMP_INPUT`, `LARECOMP_INPUT_P2..P8`, `LARECOMP_WARP_AT`, `LARECOMP_COOP_*`: docs/coop.md, "Tests"), then
reads the log. They are how the co-op fixes in `docs/coop.md` were checked; most say in their header which report
they answer and what to expect.

They are Windows-only as they stand (MSYS2 UCRT64 shell: `run_test.sh` stops the game with PowerShell and `cygpath`),
and run the checkout's own build (`make gen`, `make`). Outputs go to `build/` (git-ignored): the log of a run as
`build/run_<id>.log`, game frames in `build/frames/`, each run's settings file in `build/test_ini/<id>.ini` (made
empty, so the defaults, when missing). Your own settings and saves aren't touched.

## Setup

- **A save seed** in `build/save_seed/` (git-ignored: never commit a save): a save folder whose file 1 is past the
  intro, with the Power Bracelet for the lifting runs. The runs copy it into the checkout's `save/save_0/` first,
  removing every `savedata*.bin` there (a file 3 left from another run could be loaded instead). Most runs were
  written for a seed standing at Tal Tal Heights' start, beside a boulder, with spear Moblins near: from another
  spot, the ones that walk or warp from where the seed stands won't land where their header says.
- **Real controllers:** the runs read any controller plugged into the PC. Hide them first:

  ```
  export SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 \
         SDL_JOYSTICK_DIRECTINPUT=0 SDL_JOYSTICK_GAMEINPUT=0
  ```

- **Time:** the inputs are on game frames, not seconds. Shader compilation on a first run, or a busy GPU, slows the
  game: give a run more seconds (`SECS=` for `ic.sh`) rather than moving its frames.
- `grave.sh`, `zol.sh`, `zolhit.sh` and `revive_zol.sh` replay copies of a playtester's own saves (`build/save_grave`,
  `build/save_ll3`), which aren't in the repository: they need a save at the same place (beside the ghost's grave;
  near Red Zols) to run.

## The runs

| Script | What it checks |
|---|---|
| `run_test.sh <id> <seconds> [ENV=...]` | the runner the others use: one boot, its own settings file, the log; `EXE=` runs another build (a release, the build before a change) with its own game code |
| `ic.sh <id> <players> <warp or -> <P1 inputs> <P2 inputs> <frames>` | one or two players' inputs from the seed, an optional warp at frame 2000, frames saved; `SEED=` another save folder, `SECS=` the run's length |
| `trig.sh` | the same for event-box runs (three players' inputs, a warp) |
| `full_check.sh` | the regression set in turn (about 20 minutes): `trig_check.sh`, `door_check.sh`, two A-button runs, a side-view room, `hud_check.sh` |
| `trig_check.sh` | a fight room that locks (Angler's Tunnel 07E) with 3 players, Lanmola's start, a warp hole with Player 2 then Player 1, another player's death made a knock-out |
| `door_check.sh` | doors and exits: the Trendy Game, Marin & Tarin's house, Player 2 held at a door |
| `door_collide.sh` | doors with player collision on, 4 players |
| `interact_check.sh` | A-button actions: Player 2 lifting, throwing, opening a chest, reading a sign; two players at one rock |
| `play_check.sh` | 3 players in Mabe Village, Tail Cave and a side-view room, frames saved |
| `hud_check.sh` | 5 players, one knocked out and revived; the HUD frame |
| `hud_pair.sh [exe]` | 4 players on one spot, one knocked out: the revive prompts stacked without hanging |
| `revive_hit.sh`, `revive_hit2.sh` | revives: the prompt's place, the reviver standing still, a hit stopping a revive |
| `savetest.sh` | a save through the pause menu with Player 2 away keeps Player 1's spot |
| `ground_reach.sh` | how far from Player 1 the Field's ground holds another player |
| `rock_run.sh`, `rock_flash.sh` | cut grass and thrown rocks not made again where they'd be seen |
| `zoom_solo.sh`, `zoom_solo_grass.sh` | the camera zoom alone ("Zoom out": Always) and with co-op, outdoors and in |
| `zoom_audit.sh [zoom]` | one frame at a forced zoom in 40 places, for effects showing an edge |
| `fog_check.sh` | the Mysterious Woods' fog seen from Mabe Village zoomed out |
| `floor.sh` | a collapse tile dropping under Player 2 (Catfish's Maw) |
| `likelike.sh` | a Like Like catching Player 2 |
| `armos.sh`, `armos_fight.sh` | the Armos Knight's target, traced |
| `grave.sh`, `zol.sh`, `zolhit.sh`, `revive_zol.sh` | the ghost's grave's event, Red Zols' targets and hits (need the saves above) |
