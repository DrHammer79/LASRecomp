#!/bin/bash
# tools/coop_tests/zoom_solo_grass.sh: Player 1 alone with the zoom on ("Zoom out" Always, Out 60%) in the tall grass south of
# Mabe Village (Field_09E's warp, then put at the ObjLawn at 71.25, 90.75): two spin attacks, then five seconds north
# and back. Zoomed out the grass he cut stays in view, so made again it's killed (gone_check, "kept gone" lines). Then
# the same alone with the game's own camera (zoom_hook off: no gone lines at all).
cd "$(git rev-parse --show-toplevel)" || exit 1
P1="2450:B:80,2600:B:80,2800:STICK_UP:300,3150:STICK_DOWN:300"
SH=2750,3100,3300,3480
mkdir -p build/test_ini
printf 'coop_zoom_mode = 1\ncoop_zoom = 6\n' > build/test_ini/zgsolo.ini
: > build/test_ini/zgvan.ini
for id in ${IDS:-zgsolo zgvan}; do
    tools/coop_tests/trig.sh $id 1 75 Field:Field_09E "$P1" "" "" $SH LARECOMP_PLACE=2300:71.25:90.75 LARECOMP_PLACE_Y=12.0 \
        LARECOMP_COOP_LOG=1
    echo "== $id: $(grep -c 'FATAL' build/run_$id.log) errors, $(grep -c ' destroyed' build/run_$id.log) destroyed," \
         "$(grep -c 'kept gone' build/run_$id.log) kept gone, $(grep -c 'nobody sees it' build/run_$id.log) back unseen"
    grep -h "\[place\]\|kept gone\|nobody sees" build/run_$id.log | cut -c1-120 | head -8
done
