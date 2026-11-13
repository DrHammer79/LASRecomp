#!/bin/bash
# tools/coop_tests/zol.sh <id> "<P2 inputs>" [extra env...]: Red Zols (EnemyZolRed) latching on (user 2026-10-30: "the sticky dudes
# that latch on still go to p1"). The user's File 3 (build/save_ll3, near Red Zols), two players, infinite hearts:
# Player 1 stands; Player 2 goes as told. [trace]: each Red Zol every half second - one on a player's spot is on him.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; P2=$2; shift 2
echo "cheat_hearts = 1" > build/test_ini/$ID.ini
SEED=build/save_ll3 SECS=${SECS:-90} tools/coop_tests/ic.sh $ID 2 - "${P1:-}" "$P2" ${SHOTS:-2400,3000,3600} LARECOMP_COOP_TRACE=EnemyZolRed \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@" > /dev/null
grep -E "^\[coop\] frame" build/run_$ID.log | awk '{f=$3+0; if (f>=1700 && f%300==1) print}' | cut -c1-80
grep -F "[trace]" build/run_$ID.log | awk '{f=$3+0; if (f%300==0) print}' | cut -c1-100 | head -60
grep -E "ZolRed.*->|player 2 (hit|state kind)" build/run_$ID.log | uniq -c | head -20
