#!/bin/bash
# tools/coop_tests/revive_zol.sh: a hit stops a revive (user 2026-10-31), with the Red Zols of the user's File 3 (build/save_ll3)
# coming at the players. Three players, player 3 knocked out 2 s after arriving, a 10 s revive; Players 1 and 2 hold A
# beside him in bursts - 1 s held, 0.5 s let go - so it builds slowly: a Zol's hit on either logs "hit while reviving".
cd "$(git rev-parse --show-toplevel)" || exit 1
printf 'coop_revive_s = 10\n' > build/test_ini/revzol.ini
A=$(python -c "print(','.join(f'{f}:A:60' for f in range(2300, 7000, 90)))")
SEED=build/save_ll3 SECS=130 tools/coop_tests/ic.sh revzol 3 - "$A" "$A" 0 LARECOMP_COOP_DOWN=3 \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@"
awk '/^\[coop\] frame/ {f=$3} /knocked out|revived|reviving|hit for/ {print f": "$0}' build/run_revzol.log | head -40
