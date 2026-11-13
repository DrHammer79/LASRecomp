#!/bin/bash
# tools/coop_tests/revive_hit.sh: revives (user 2026-10-31). Three players at the seed's start (spear Moblins hit the idle ones),
# player 3 knocked out 2 s after arriving, a 10 s revive.
# - build/window.png (frame 2250): the "(A) Revive" prompt below player 3, the knocked-out one (Players 1 and 2 beside
#   him, neither holding A).
# - Player 2 holds A beside him for 5 s (2300-2600) and pushes his stick right meanwhile (2400, 2450, 2500): he stays
#   put (the [coop] frame lines' P2). He pushes it again at 2700 and 2750, A let go: he moves.
# - Then he holds A for 50 s (2900-5900): a hit then logs "hit while reviving" and no revive follows until he presses
#   A again (6000).
cd "$(git rev-parse --show-toplevel)" || exit 1
printf 'coop_revive_s = 10\n' > build/test_ini/revhit.ini
rm -f build/window.png
SECS=120 tools/coop_tests/ic.sh revhit 3 - "" \
    "2300:A:300,2400:STICK_RIGHT:20,2450:STICK_RIGHT:20,2500:STICK_RIGHT:20,2700:STICK_RIGHT:20,2750:STICK_LEFT:20,2900:A:3000,6000:A:900" \
    0 LARECOMP_COOP_DOWN=3 LARECOMP_WINDOW_SHOT=2250 \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@"
grep -E "^\[coop\] frame (22[0-9]1|2[3-9][0-9]1) " build/run_revhit.log | awk '{f=$3+0; if (f%90==61 || f>=2690 && f<=2820) print}' | cut -c1-90
grep -nE "knocked out|revived|reviving|player 2 hit" build/run_revhit.log | head -30
grep -nE "^\[coop\] frame (29[0-9]1|[3-6][0-9]{3}) " build/run_revhit.log | awk -F: '{split($2,a," "); f=a[3]+0; if (f%600==1) print}' | cut -c1-60
ls -la build/window.png
