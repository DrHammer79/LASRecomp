#!/bin/bash
# tools/coop_tests/armos.sh <id> <players> [extra env...]: the Armos Knight (EnemyDeguArmos, the Southern Face Shrine's first room;
# user 2026-10-30: "walking to the corner with 4 players fighting him and chain jumping, not attacking a target and
# chasing"). Warp to the entrance, Player 1 walks up into the room (the knight wakes), the others spread out - 2 left,
# 3 right, 4 back down - and everyone stands; A presses through the wake-up text. [trace] lines: his position.
# Targeting off: build/test_ini/<id>.ini with coop_targets = 0.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; N=$2; shift 2
P1="2500:STICK_UP:160,$(python -c "print(','.join(f'{f}:A:6' for f in range(2700, 3250, 60)))"),3300:STICK_RIGHT:45,3345:STICK_DOWN:10"
SDLX=(SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0
      SDL_JOYSTICK_GAMEINPUT=0)
[ -f build/test_ini/$ID.ini ] || echo "cheat_hearts = 1" > build/test_ini/$ID.ini
SECS=${SECS:-110} tools/coop_tests/ic.sh $ID $N ArmosShrine:ArmosShrine_03A "$P1" "2700:STICK_UP:110,3240:STICK_135:200" ${SHOTS:-3400,4000,4600,5200,5800} \
    "LARECOMP_INPUT_P3=2700:STICK_UP:110,3240:STICK_45:200" "LARECOMP_INPUT_P4=2700:STICK_UP:90,3240:STICK_LEFT:90" LARECOMP_COOP_TRACE=EnemyDeguArmos \
    "${SDLX[@]}" "$@"
grep -E "\[trace\]|DeguArmos.*->|knocked|level change|game over" build/run_$ID.log | cut -c1-130 | awk 'NR<=3 || NR%3==0' | head -60
