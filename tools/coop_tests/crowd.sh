#!/bin/bash
# tools/coop_tests/crowd.sh: Player 1 alone with co-op on (the others' Links benched: made as the file loads, switched
# off until their players join) stands and walks as with co-op off. Two runs from the seed's spot (Field), Players Off
# and On (nobody joins): he stands still after the load, then the stick left, right and up for 120 frames each; his
# positions every 30 frames, side by side, should match (user 2026-11-16: "Main link keeps walking to the right
# weirdly": with co-op on he walked half a tile right after every load - a benched Link's camera answered a new scroll
# area, and the scroll event walked him on; docs/coop.md, Camera areas).
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p build/test_ini
printf 'coop_players = 1\n' > build/test_ini/crowd_off.ini
printf 'coop_players = 2\n' > build/test_ini/crowd_on.ini
for id in crowd_off crowd_on; do
    SECS=50 tools/coop_tests/ic.sh $id - - "2400:STICK_LEFT:120,2600:STICK_RIGHT:120,2800:STICK_UP:120" "" "" \
        LARECOMP_HIDDEN=1 &
done
wait
pos() { grep -E "\[coop\] frame (1[6-9][0-9]|2[0-9][0-9]|30[0-9])[0-9] P1" build/run_$1.log |
        sed -E 's/^\[coop\] frame ([0-9]+) P1 \(([^)]*)\).*/\1 \2/'; }
pos crowd_off > build/crowd_off.pos; pos crowd_on > build/crowd_on.pos
paste build/crowd_off.pos build/crowd_on.pos | awk '{ d = ($2 - $6) * ($2 - $6) + ($4 - $8) * ($4 - $8);
    printf "frame %s  off %7.2f %7.2f   on %7.2f %7.2f%s\n", $1, $2, $4, $6, $8, (d > 0.01 ? "   <- apart" : "") }'
grep -h "would scroll to a new camera area" build/run_crowd_on.log | head -3
