#!/bin/bash
# tools/coop_tests/ic.sh <id> <players> <warp level:locator or -> <P1 inputs> <P2 inputs> <frames to save> [extra env...]
# Boots into file 1 (the save seed; SEED=<folder>: that save folder instead), optionally warps at frame 2000, logs to build/run_<id>.log and saves game frames
# to build/frames/<id>_frame_<n>.png.
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0 build/frames
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in ${TITLE_A:-range(1400, 1800, 100)}]
print(','.join(s))")
ID=$1; N=$2; W=$3; I1=$4; I2=$5; SH=$6; shift 6
rm -f build/frames/frame_*.png
rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
rm -f save/save_0/savedata*.bin save/save_0/screenshot*.bin; cp ${SEED:-build/save_seed}/* save/save_0/
WARP=()
[ "$W" != "-" ] && WARP=(LARECOMP_WARP_AT=2000:$W)
tools/coop_tests/run_test.sh "$ID" ${SECS:-70} LARECOMP_COOP=$N LARECOMP_COOP_LOG=1 "${WARP[@]}" \
    "LARECOMP_INPUT=$TITLE${I1:+,$I1}" "LARECOMP_INPUT_P2=${I2:-9999:A:1}" LARECOMP_SHOT_AT=$SH "$@" > /dev/null 2>&1
for f in build/frames/frame_*.png; do [ -f "$f" ] && mv "$f" "build/frames/${ID}_$(basename "$f")"; done
echo "== $ID: $(grep -c 'level-jump request object' build/run_$ID.log) level loads, $(grep -c 'FATAL' build/run_$ID.log) errors"
