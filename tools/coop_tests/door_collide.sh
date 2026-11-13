#!/bin/bash
# tools/coop_tests/door_collide.sh: doors with player collision on and 4 players - the stacked Links pushing apart as they arrive
# must not walk Player 1 (or, with any player's exits, anyone) back out. Prints the levels each run went through.
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0 build/test_ini
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s))")
run() {  # run <id> <host exits 0/1> <level:locator to warp to at frame 2000> <Player 1's inputs>
    rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
    cp build/save_seed/* save/save_0/
    printf 'coop_collision = 1\ncoop_host_exits = %s\n' "$2" > build/test_ini/$1.ini
    tools/coop_tests/run_test.sh "$1" 75 LARECOMP_COOP=4 LARECOMP_COOP_LOG=1 LARECOMP_WARP_AT=2000:$3 "LARECOMP_INPUT=$TITLE,$4" \
        > /dev/null 2>&1
    echo "== $1: $(grep -o 'level [A-Za-z0-9_]*' build/run_$1.log | grep -v 'level change\|level load' | uniq | \
        awk '{print $2}' | tr '\n' ' ')| level loads: $(grep -c 'level-jump request' build/run_$1.log) | exit holds: $(grep -c 'at an exit' build/run_$1.log) | errors: $(grep -c 'FATAL' build/run_$1.log)"
}
run c_trendy 1 Field:Field_12D "3000:STICK_UP:150"
run c_house 1 MarinTarinHouse:MarinTarinHouse_01A "2500:STICK_DOWN:200,3400:STICK_UP:150"
run c_idle 1 MarinTarinHouse:MarinTarinHouse_01A "9999:A:1"
run c_trendy_any 0 Field:Field_12D "3000:STICK_UP:150"
run c_house_any 0 MarinTarinHouse:MarinTarinHouse_01A "2500:STICK_DOWN:200,3400:STICK_UP:150"
