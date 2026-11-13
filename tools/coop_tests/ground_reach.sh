#!/bin/bash
# tools/coop_tests/ground_reach.sh: how far from Player 1 the overworld's ground holds another player. Player 1 stands at Ukuku
# Prairie (Field_09E start, 74.25 9.75 105.75); Player 2 is dropped (once) at offsets from him onto open grass and
# watched: "stands" or "falls" (his height 1 s and 3 s later).
cd "$(git rev-parse --show-toplevel)" || exit 1
printf 'coop_leash = 0\n' > build/test_ini/gr.ini
OFFS="${*:-4:0 10:0 16:0 22:0 0:8 0:14 0:-14}"
for off in $OFFS; do
    dx=${off%%:*}; dz=${off##*:}
    x=$(python -c "print(74.25 + $dx)"); z=$(python -c "print(105.75 + $dz)")
    rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
    cp build/save_seed/* save/save_0/
    SECS=45 tools/coop_tests/ic.sh gr 2 Field:Field_09E "" "" 9999 LARECOMP_COOP_MOVE=2300:2:$x:${DROP_Y:-11}:$z > /dev/null 2>&1
    y1=$(grep "^\[coop\] frame 23[67]1 " build/run_gr.log | head -1 | sed -E 's/.*P2 \(([-0-9.]+) ([-0-9.]+) ([-0-9.]+)\).*/\2/')
    y3=$(grep "^\[coop\] frame 24[89]1 " build/run_gr.log | head -1 | sed -E 's/.*P2 \(([-0-9.]+) ([-0-9.]+) ([-0-9.]+)\).*/\2/')
    lost=$(grep -c "fell through the world" build/run_gr.log)
    echo "offset x $dx z $dz -> P2 at ($x, $z): height after 1 s $y1, after 3 s $y3, rescues $lost"
done
