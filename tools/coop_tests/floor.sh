#!/bin/bash
# tools/coop_tests/floor.sh <id> [extra env...]: Player 2 on a collapse tile (user 2026-10-30: "the crumbling stone we are by
# doesn't crumble for other players. Only p1", a Tal Tal Heights cave, Tamaranch04). Catfish's Maw, warped to room
# 02E: Player 1 put in room 03G (x 90-105, z 24-36) away from its tiles (P1X=, P1Z=; 96.75/33.75 is a pit), Player 2
# put on the collapse tile at (92.25, 27.75) (its middle at y -1) at frame 2500. The tile drops under him within a
# second (he falls, and comes back up at the pit's edge); before 2026-11-01's fix (COOP_TILE_PLAYER_SITE) he stood on
# it. P1=1: Player 1 put on the tile instead (the game's own: it drops). (Not on the room's edge: put on z 24.75,
# Player 1 walked into the room.) [trace]: the tile every EVERY frames (10). In the user's cave: tools/coop_tests/ic.sh <id> 2
# Tamaranch04:Tamaranch04_02E - - 0 LARECOMP_COOP_MOVE=2400:2:68.25:0:21.75 (Player 1 arrives at 64.5, 21.65).
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; shift
if [ -n "$P1" ]; then AT=(LARECOMP_PLACE=2300:92.25:27.75); else
    AT=(LARECOMP_PLACE=2300:${P1X:-92.0}:${P1Z:-33.0} LARECOMP_COOP_MOVE=2500:2:92.25:0:27.75); fi
SECS=${SECS:-60} tools/coop_tests/ic.sh $ID 2 Lv05CatfishsMaw:Lv05CatfishsMaw_02E - - ${SHOTS:-0} "${AT[@]}" \
    LARECOMP_COOP_TRACE=ObjDungeonCo-990008 LARECOMP_COOP_TRACE_EVERY=${EVERY:-10} \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@" > /dev/null
grep -a "test: player\|collapse tile\|its \"Link\"\|\[coop\] frame \(2[4-9]\|3[0-1]\)[0-9]1 " build/run_$ID.log | cut -c1-100
