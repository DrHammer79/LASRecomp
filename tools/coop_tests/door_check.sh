#!/bin/bash
# tools/coop_tests/door_check.sh: door regressions - the Trendy Game in (2 and 3 players) and out-and-back-in, Marin & Tarin's
# house out and in, Player 2 held at the house's exit, co-op off. Prints the levels each run went through, how often
# another player was held at an exit and any fatal error. Needs build/save_seed/ (a save whose file 1 is past the
# intro); expected results in AI_BRIEF.md.
cd "$(git rev-parse --show-toplevel)" || exit 1
mkdir -p save/save_0
# boot -> title -> file 1 (L+R through the intro, then A)
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s))")
run() {  # run <id> <players> <level:locator to warp to at frame 2000> <Player 1's inputs> [Player 2's inputs]
    rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
    cp build/save_seed/* save/save_0/
    tools/coop_tests/run_test.sh "$1" 75 LARECOMP_COOP=$2 LARECOMP_COOP_LOG=1 LARECOMP_WARP_AT=2000:$3 "LARECOMP_INPUT=$TITLE,$4" \
        "LARECOMP_INPUT_P2=${5:-9999:A:1}" > /dev/null 2>&1
    echo "== $1: $(grep -o 'level [A-Za-z0-9_]*' build/run_$1.log | grep -v 'level change\|level load' | uniq | \
        awk '{print $2}' | tr '\n' ' ')| exit holds: $(grep -c 'at an exit' build/run_$1.log) | errors: $(grep -c 'FATAL' build/run_$1.log)"
}
run trendy2 2 Field:Field_12D "3000:STICK_UP:150"
run trendy3 3 Field:Field_12D "3000:STICK_UP:150"
run craneback 2 CraneGame:CraneGame_01A "2500:STICK_DOWN:200,3500:STICK_UP:150"
run house 2 MarinTarinHouse:MarinTarinHouse_01A "2500:STICK_DOWN:200,3400:STICK_UP:150"
run p2exit 2 MarinTarinHouse:MarinTarinHouse_01A "9999:A:1" "2500:STICK_DOWN:200"
run solo 1 Field:Field_12D "3000:STICK_UP:150"
