#!/bin/bash
# tools/coop_tests/stray.sh [id]: a player who leaves leaves no Link behind. Co-op on in the settings, Player 1 alone
# at the start (the seed's spot, Field); player 3 joins at 2300 (player 2 never does), leaves at 2600; Player 1 walks
# left from 2700, so a Link left standing where they were shows apart from him (build/frames/<id>_frame_2800.png);
# player 3 joins again at 2900 (build/frames/<id>_frame_3000.png: beside Player 1). Prints the Links updating each 60
# frames (the trace: Player 1's and, while he is in, player 3's - a benched one doesn't update) - (user 2026-11-16:
# "Still leaving old links that never joined on leaving with select": the one who left, drawn where his model was).
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=${1:-stray}
mkdir -p build/test_ini
printf 'coop_players = 2\n' > build/test_ini/$ID.ini
SECS=${SECS:-60} tools/coop_tests/ic.sh "$ID" - - "2700:STICK_LEFT:90" "" "2690,2800,3000" LARECOMP_HIDDEN=1 \
    LARECOMP_COOP_JOIN=2300:3,2900:3 LARECOMP_COOP_LEAVE=2600:3 LARECOMP_COOP_TRACE=Player LARECOMP_COOP_TRACE_EVERY=60
grep -E "\[coop\] (player 3 (joined|left)|player 3's Link .* benched)|exception|crashed" build/run_$ID.log | cut -c1-90
grep -E "\[trace\] frame (2[5-9]|3[0-9])[0-9]{2} Player[0-9]+ \(" build/run_$ID.log | cut -c1-80
