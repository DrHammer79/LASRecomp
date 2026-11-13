#!/bin/bash
# tools/coop_tests/rock_flash.sh [id:players ...]: the user's "boulders flashing with a lag spike as if respawning, then gone"
# (2026-10-30: standing on a screen's edge beside rocks the players had broken). Tal Tal Heights (the save seed's file
# 1 starts beside ObjRock-6A004A005CF13A61, west of Link): lift it, throw it west (broken), then step east past the
# screen's edge (x 135) and back, twice - each step back into the screen has the game make it again. Before: "made
# again in view: kept gone" each time (made, drawn a frame, killed); now: "not made again".
cd "$(git rev-parse --show-toplevel)" || exit 1
T1="2150:STICK_LEFT:3,2180:A:8,2240:A:8,2330:STICK_RIGHT:130,2480:STICK_LEFT:130,2630:STICK_RIGHT:130,2780:STICK_LEFT:130"
for r in ${@:-rf2:2}; do
    id=${r%%:*} n=${r##*:}
    : > build/test_ini/$id.ini
    SECS=65 tools/coop_tests/ic.sh $id $n - "$T1" "" 2250,2470,2620,2770,2920 LARECOMP_TRACK_RESPAWN=1 \
        SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
        SDL_JOYSTICK_GAMEINPUT=0 > /dev/null
    echo "== $id: $(grep -c 'FATAL' build/run_$id.log) errors"
    grep -h "destroyed\|kept gone\|not made\|nobody sees\|\[respawn\]" build/run_$id.log | cut -c1-150 | head -14
done
