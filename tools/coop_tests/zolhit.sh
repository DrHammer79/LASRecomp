#!/bin/bash
# tools/coop_tests/zolhit.sh <id> [extra env...]: Red Zols hitting Player 2 for real (user 2026-10-31: "Zols still always going on
# p1 when hitting p2" - tools/coop_tests/zol.sh has infinite hearts, so no hit of theirs lands). The user's File 3
# (build/save_ll3), two players: Player 1 stands, Player 2 is put 4.5 units west of him, on the Zols, and stands - their
# hits knock him back (MOVE=1600:2:37.0:12.75:56.25:hold pins him there: no knock-back). [trace]: each Red Zol and its
# reaction part (ZolRed:) every EVERY frames (10), and whom each goes for - before 2026-10-31's fix the parts went for
# Player 1 after the knock-backs.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; shift
: > build/test_ini/$ID.ini
SEED=build/save_ll3 SECS=${SECS:-80} tools/coop_tests/ic.sh $ID 2 - "${P1:-}" "${P2:-}" ${SHOTS:-0} "LARECOMP_COOP_TRACE=EnemyZolRed,ZolRed:" \
    LARECOMP_COOP_TRACE_EVERY=${EVERY:-10} LARECOMP_COOP_MOVE=${MOVE:-1600:2:37.0:12.75:56.25} \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@" > /dev/null
awk '/^\[coop\] frame/ {f=$3} /ZolRed.*->|goes for|player [12] hit|knocked out/ {print f": "$0}' build/run_$ID.log |
    grep -v "8D00\|6272B7" | cut -c1-120 | head -50
