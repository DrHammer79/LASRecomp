#!/bin/bash
# tools/coop_tests/hud_check.sh: 5 players, player 3 knocked out 2 s after arriving, players 2 / 4 / 5 walk off, then Player 1
# holds A beside player 3 (a revive). build/window.png (frame 2300): the heart rows - 3 bottom left, 1 bottom right -
# and the "Zzz" bubble over player 3. The log shows the knock-out, the revive and "just revived, unhurt" for a hit
# within 2 s of it.
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s + ['2400:A:330']))")
rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
cp build/save_seed/* save/save_0/
rm -f build/window.png
tools/coop_tests/run_test.sh hud5 75 LARECOMP_COOP=5 LARECOMP_COOP_LOG=1 LARECOMP_COOP_DOWN=3 "LARECOMP_INPUT=$TITLE" \
    "LARECOMP_INPUT_P2=1900:STICK_RIGHT:40" "LARECOMP_INPUT_P4=1900:STICK_LEFT:40" "LARECOMP_INPUT_P5=1900:STICK_DOWN:30" \
    LARECOMP_WINDOW_SHOT=2300 > /dev/null 2>&1
grep -h "knocked out\|revived\|unhurt" build/run_hud5.log
ls -la build/window.png
