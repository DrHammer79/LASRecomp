#!/bin/bash
# tools/coop_tests/rock_run.sh: the user's "picking up the boulder and running past slightly will almost always trigger the boulder
# and grass respawn" (2026-10-29). At the double rocks east of Mabe Village (Field_10E):
# Player 1 lifts the northern rock, runs east past the screen's edge (x 75) carrying it, runs back, throws it.
# rrcoop: two players (zoom Out 50%, the default); rrsolo: alone, "Zoom out" Always; rrvan: alone, the game's camera.
# [respawn] lines (co-op, or alone with the zoom on; rrvan has none) and the gone lines (destroyed / kept gone / nobody
# sees it) say what was made again, where, and whether in view (SEEN).
cd "$(git rev-parse --show-toplevel)" || exit 1
P1="2500:STICK_DOWN:4,2560:A:8,2620:STICK_RIGHT:100,2760:STICK_LEFT:100,2900:A:8"
SH=2600,2720,2780,2860,3000,3200
# (Tal Tal Heights, no warp: the save seed's file 1 starts beside ObjRock-6A004A005CF13A61 at 128.25, 35.25 - west of
# Link. Lift it facing west, run east past the screen's edge (x 135) carrying it, back west, throw.)
T1="2150:STICK_LEFT:3,2180:A:8,2240:STICK_RIGHT:130,2390:STICK_LEFT:130,2540:A:8"
TSH=2220,2330,2390,2470,2560,2750
mkdir -p build/test_ini
: > build/test_ini/rrcoop.ini
printf 'coop_zoom_mode = 1\n' > build/test_ini/rrsolo.ini
: > build/test_ini/rrvan.ini
: > build/test_ini/ttcoop.ini
printf 'coop_zoom_mode = 1\n' > build/test_ini/ttsolo.ini
: > build/test_ini/ttvan.ini
run() {
    local id=$1 n=$2
    if [ "${id#tt}" != "$id" ]; then  # (tt...: Tal Tal Heights, no warp)
        SECS=60 tools/coop_tests/ic.sh $id $n - "$T1" "" $TSH LARECOMP_TRACK_RESPAWN=1 > /dev/null
    else
        tools/coop_tests/trig.sh $id $n 70 Field:Field_12D "$P1" "" "" $SH LARECOMP_PLACE=2300:68.25:108.6 LARECOMP_COOP_LOG=1 \
            LARECOMP_TRACK_RESPAWN=1
    fi
    echo "== $id: $(grep -c 'FATAL' build/run_$id.log) errors"
    grep -h "\[place\]\|\[respawn\]\|destroyed\|kept gone\|nobody sees\|level change" build/run_$id.log | grep -v 'frame 2000' |
        cut -c1-150 | head -14
}
# (arguments: the runs as id:players - the Mabe ones rr..., the Tal Tal ones tt...)
for r in ${@:-rrcoop:2 rrsolo:1 rrvan:1}; do run ${r%%:*} ${r##*:}; done
