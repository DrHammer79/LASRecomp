#!/bin/bash
# tools/coop_tests/savetest.sh <id> <players> <secs> <save frame> <P1 inputs> <P2 inputs> [ENV...]: loads the test save in a scratch
# save folder (build/sv_<id>), plays the inputs, then saves to File 1 through the pause menu at <save frame>
# (Plus, R to System, A Save, A File 1, Right, A Save). Prints what the file holds at the restart point after.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; N=$2; T=$3; S=$4; I1=$5; I2=$6; shift 6
rm -rf build/sv_$ID; mkdir -p build/sv_$ID/save_0; cp build/save_seed/* build/sv_$ID/save_0/
TITLE=$(python -c "print(','.join([f'{f}:L:10,{f}:R:10' for f in range(400,1400,90)]+[f'{f}:A:8' for f in range(1400,1800,100)]))")
SV="$S:PLUS:6,$((S+150)):R:6,$((S+300)):A:6,$((S+450)):A:6,$((S+550)):RIGHT:6,$((S+600)):A:6,$((S+800)):B:6,$((S+900)):B:6,$((S+1000)):B:6"
tools/coop_tests/run_test.sh "$ID" $T LARECOMP_COOP=$N LARECOMP_COOP_LOG=1 LARECOMP_SAVE_DIR=build/sv_$ID LARECOMP_WATCH_RP=1 \
    "LARECOMP_INPUT=$TITLE${I1:+,$I1},$SV" "LARECOMP_INPUT_P2=${I2:-9999:A:1}" "$@" > /dev/null 2>&1
grep -c "committed build/sv_$ID/save_0/savedata0.bin\|committed .*savedata0.bin" build/run_$ID.log | sed 's/^/saves: /'
build/rando-venv/Scripts/python.exe -c "
import struct; b=open('build/sv_$ID/save_0/savedata0.bin','rb').read()
print('file +5584 %.2f %.2f %.2f | +5590 %.2f %.2f %.2f | +559c %.2f %.2f %.2f' % (struct.unpack_from('<3f',b,0x5584)+struct.unpack_from('<3f',b,0x5590)+struct.unpack_from('<3f',b,0x559c)))"
