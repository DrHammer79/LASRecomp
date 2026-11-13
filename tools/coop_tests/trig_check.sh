#!/bin/bash
# tools/coop_tests/trig_check.sh: co-op trigger checks - Angler's Tunnel 08E up into the fight room 07E with 3 players (at once,
# one at a time), Lanmola's pit with the others in first, a field warp hole with Player 2 then Player 1, Player 2 sent
# to the dead state. Frames in build/frames/<id>_frame_<n>.png, logs in build/run_<id>.log.
cd "$(git rev-parse --show-toplevel)" || exit 1
U="2450:LSTICK:4,2450:RSTICK:4,2500:STICK_UP:150"
tools/coop_tests/trig.sh fin3 3 55 Lv04AnglersTunnel:Lv04AnglersTunnel_08D "$U" "$U" "$U" 2530,2560,2620,2700 LARECOMP_PLACE=2300:67.5:85.15
tools/coop_tests/trig.sh fin1 3 65 Lv04AnglersTunnel:Lv04AnglersTunnel_08D "2450:LSTICK:4,2450:RSTICK:4,2750:STICK_UP:150" \
    "2450:LSTICK:4,2450:RSTICK:4,2500:STICK_UP:150" "2450:LSTICK:4,2450:RSTICK:4,3000:STICK_UP:100" 2600,2780,2830,2900 \
    LARECOMP_PLACE=2300:67.5:85.15
tools/coop_tests/trig.sh finlan 3 60 Field:Field_13P "2800:STICK_UP:40" "2420:LSTICK:4,2420:RSTICK:4,2500:STICK_UP:40" \
    "2420:LSTICK:4,2420:RSTICK:4,2510:STICK_UP:40" 2560,2620,2700 LARECOMP_PLACE=2300:217.5:157.5
tools/coop_tests/trig.sh finwh2 3 50 Field:Warp_UkukuPrairie "" "2600:STICK_UP:90" "" 2700,2780
tools/coop_tests/trig.sh finwh1 3 50 Field:Warp_UkukuPrairie "2600:STICK_UP:90" "" "" 2700,2780
tools/coop_tests/trig.sh findie 2 45 Field:Field_12D "" "" "" 2500 LARECOMP_COOP_DIE=2
for r in fin3 fin1 finlan finwh2 finwh1 findie; do
    echo "== $r: $(grep -c 'level-jump request' build/run_$r.log) level loads, events: $(grep -c 'Player 1 in an event' build/run_$r.log)"
    grep -h "event box\|warp hole\|dead state\|knocked out" build/run_$r.log | head -4
done
