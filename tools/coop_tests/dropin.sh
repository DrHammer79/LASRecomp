#!/bin/bash
# tools/coop_tests/dropin.sh [id]: drop-in co-op through controllers, as players would - test controllers (virtual ones
# SDL lists like real ones: LARECOMP_HOST_INPUT vpad) press their buttons. Co-op on in the settings, Player 1 alone at
# the start; after the warp (frame 2000, Field_13P): controller 2's Plus held 2 s joins as player 2 (2400), controller
# 4's tapped joins nobody (2550), controller 3's held joins as player 3 (2650); controller 2 holds Minus and taps R
# (2900: his colour steps, he stays, no ring); controller 3 holds Minus alone 4 s (3100: from 0.6 s the ring fills,
# build/<id>_ring.png at 3250, he leaves at 3.1 s); its Plus held joins again (3450).
# The log's "[input]" / "[coop]" lines show each step.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=${1:-dropin}
mkdir -p save/save_0 build/test_ini
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s))")
rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin
cp build/save_seed/* save/save_0/
printf 'coop_players = 2\n' > build/test_ini/$ID.ini
# (each test controller connected - its first use - well before its Plus: a press in the frame it is attached can come
# before SDL has opened it as a gamepad, and is lost - a real controller is connected long before it is pressed)
H="2200:vpad:1/a:5,2200:vpad:2/y:2,2200:vpad:3/y:2,2200:vpad:4/y:2,2400:vpad:2/start:120,2550:vpad:4/start:30,2650:vpad:3/start:120,2900:vpad:2/back:60"
H="$H,2920:vpad:2/rightshoulder:6,2935:shot:${ID}_colour,3100:vpad:3/back:240,3250:shot:${ID}_ring"
H="$H,3450:vpad:3/start:120,3700:shot:${ID}_end"
tools/coop_tests/run_test.sh "$ID" 80 LARECOMP_HIDDEN=1 LARECOMP_COOP_LOG=1 LARECOMP_WARP_AT=2000:Field:Field_13P \
    "LARECOMP_INPUT=$TITLE" "LARECOMP_HOST_INPUT=$H" > /dev/null 2>&1
grep -E "\[input\] (test controller|Player [0-9]|gamepad)|\[coop\] (player [0-9] (joined|left|couldn)|[0-9] players)|exception|crashed" \
    build/run_$ID.log
ls build/${ID}_ring.png build/${ID}_end.png 2>&1
