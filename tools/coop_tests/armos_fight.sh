#!/bin/bash
# tools/coop_tests/armos_fight.sh <id> [extra env...]: the Armos Knight with his first target running off: after the wake-up
# (everyone stacked on Player 1: he's the target) Player 1 runs to the top-left corner and stays; players 2-4 stay at the
# knight swinging their swords (B). User 2026-10-30: "walking to the corner with 4 players fighting him and chain jumping,
# not attacking a target". [trace]: where he goes; "-> P" lines: his target switches. Infinite hearts.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; shift
A=$(python -c "print(','.join(f'{f}:A:6' for f in range(2700, 3250, 60)))")
B=$(python -c "print(','.join(f'{f}:B:8' for f in range(3300, 7600, 40)))")
echo "cheat_hearts = 1" > build/test_ini/$ID.ini
SECS=${SECS:-135} tools/coop_tests/ic.sh $ID 4 ArmosShrine:ArmosShrine_03A "2500:STICK_UP:160,$A,3240:STICK_135:200" \
    "2700:STICK_UP:110,$B" ${SHOTS:-3600,4400,5200,6000,6800} "LARECOMP_INPUT_P3=2700:STICK_UP:110,$B" \
    "LARECOMP_INPUT_P4=2700:STICK_UP:90,$B" LARECOMP_COOP_TRACE=EnemyDeguArmos SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 \
    SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 SDL_JOYSTICK_GAMEINPUT=0 "$@" > /dev/null
grep -F -e "[trace]" -e "DeguArmos-" build/run_$ID.log | grep -v "DeguArmos:" | awk 'NR%5==0 || /->/' | cut -c1-110 | head -40
grep -E "^\[coop\] frame (4|6)[0-9]{3}1 " build/run_$ID.log | awk 'NR%15==1' | cut -c1-110
