#!/bin/bash
# tools/coop_tests/grave.sh <id> <players> [extra env...]: the user's save beside the ghost's grave (build/save_grave, 2026-10-30):
# Player 1 walks into the grave's event, A pressed through it, then walks down and right (did it end?).
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0 build/frames
ID=$1; N=$2; shift 2
IN=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
s+=[] + [f'{f}:A:6' for f in range(3000, 7400, 45)] + ['7600:STICK_DOWN:90', '7800:STICK_RIGHT:90']
print(','.join(s))")
rm -f build/frames/frame_*.png save/save_0/savedata*.bin save/save_0/screenshot*.bin
cp build/save_grave/save_0/* save/save_0/
tools/coop_tests/run_test.sh "$ID" ${SECS:-200} LARECOMP_COOP=$N LARECOMP_COOP_LOG=1 "LARECOMP_INPUT=$IN" \
    "LARECOMP_INPUT_P2=9999:A:1" LARECOMP_SHOT_AT=${SHOTS:-2600,3400,5000,6500,7500,7900} "$@" > /dev/null 2>&1
for f in build/frames/frame_*.png; do [ -f "$f" ] && mv "$f" "build/frames/${ID}_$(basename "$f")"; done
echo "== $ID: $(grep -c 'FATAL' build/run_$ID.log) errors"
grep -E "event box|in an event|out of an event|walk-in|level change|FATAL" build/run_$ID.log | head -20
grep -E "^\[coop\] frame (2[5-9]|[3-9])[0-9][0-9]1 " build/run_$ID.log | awk 'NR%6==1' | cut -c1-110
