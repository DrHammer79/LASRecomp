#!/bin/bash
# tools/coop_tests/play_check.sh: 3 players in a few places - Mabe Village (Player 1 into the Trendy Game while the others walk
# off), Tail Cave (Player 1 into the next room, then Player 2 after him), a side-view room (the others walk and jump).
# Game frames go to build/frames/<id>_frame_<n>.png; check the [coop] frame lines in build/run_<id>.log for positions.
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0 build/frames
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s))")
run() {  # run <id> <level:locator> <Player 1 moves> <Player 2 moves> <Player 3 moves> <frames to save>
    rm -f build/frames/frame_*.png
    rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
    cp build/save_seed/* save/save_0/
    tools/coop_tests/run_test.sh "$1" 80 LARECOMP_COOP=3 LARECOMP_COOP_LOG=1 LARECOMP_WARP_AT=2000:$2 \
        "LARECOMP_INPUT=$TITLE${3:+,$3}" "LARECOMP_INPUT_P2=$4" "LARECOMP_INPUT_P3=$5" LARECOMP_SHOT_AT=$6 > /dev/null 2>&1
    for f in build/frames/frame_*.png; do [ -f "$f" ] && mv "$f" "build/frames/$1_$(basename "$f")"; done
    echo "== $1: $(grep -c 'placed at' build/run_$1.log) placements, $(grep -c 'FATAL' build/run_$1.log) errors"
}
run mabe Field:Field_12D "3000:STICK_UP:150" "2300:STICK_LEFT:60,2600:STICK_DOWN:40" "2300:STICK_RIGHT:60" 2250,2800,3400,4000
run tail Lv01TailCave:Lv01TailCave_08D "2500:STICK_UP:240" "2300:STICK_LEFT:40,3400:STICK_UP:240" "2300:STICK_RIGHT:40" 2250,2700,3100,3900
run side Lv01TailCave:Lv01TailCave_01F "2500:STICK_RIGHT:60" "2300:STICK_LEFT:60,2600:A:10" "2300:STICK_RIGHT:30,2400:B:10" 2250,2450,2800,3300
