#!/bin/bash
# tools/coop_tests/revive_hit2.sh: a hit stops a revive (user 2026-10-31). Three players at the seed's start, player 2 knocked
# out, a 10 s revive; player 3 (the side the spear Moblins come at) holds A in bursts - 1 s held, 0.5 s let go - so the
# revive builds slowly (about 30 s) - Player 1 too - and a Moblin's hit comes in it: "hit while reviving ... the revive stops".
cd "$(git rev-parse --show-toplevel)" || exit 1
printf 'coop_revive_s = 10\n' > build/test_ini/revhit2.ini
P3=$(python -c "print(','.join(f'{f}:A:60' for f in range(2300, 7000, 90)))")
SECS=130 tools/coop_tests/ic.sh revhit2 3 - "$P3" "" 0 LARECOMP_COOP_DOWN=2 "LARECOMP_INPUT_P3=$P3" \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@"
grep -nE "knocked out|revived|reviving|player 3 hit" build/run_revhit2.log | head -30
