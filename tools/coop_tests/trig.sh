#!/bin/bash
# tools/coop_tests/trig.sh <id> <players> <secs> <level:locator> <P1 input> <P2 input> <P3 input> [shots] [ENV=value...]:
# one co-op trigger test run from the save seed - title, file 1, the warp at frame 2000, then the inputs. Frames go
# to build/frames/<id>_frame_<n>.png.
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0 build/frames
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s))")
ID=$1; N=$2; T=$3; W=$4; I1=$5; I2=${6:-9999:A:1}; I3=${7:-9999:A:1}; SH=$8; shift 8
rm -f build/frames/frame_*.png
rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
cp build/save_seed/* save/save_0/
tools/coop_tests/run_test.sh "$ID" "$T" LARECOMP_COOP=$N LARECOMP_COOP_LOG=1 LARECOMP_WARP_AT=2000:$W \
    "LARECOMP_INPUT=$TITLE${I1:+,$I1}" "LARECOMP_INPUT_P2=$I2" "LARECOMP_INPUT_P3=$I3" ${SH:+LARECOMP_SHOT_AT=$SH} "$@" > /dev/null 2>&1
for f in build/frames/frame_*.png; do [ -f "$f" ] && mv "$f" "build/frames/${ID}_$(basename "$f")"; done
echo "== $ID: $(grep -c 'placed at' build/run_$ID.log) placements, $(grep -c 'level-jump request' build/run_$ID.log) level loads, $(grep -c 'FATAL' build/run_$ID.log) errors"
