#!/bin/bash
# tools/coop_tests/hud_pair.sh [exe]: 4 players on one spot, player 2 knocked out 2 s after arriving, nobody walks off - three
# players beside him, his one "(A) Revive" prompt (since 2026-10-31 under the knocked-out player; under each of the
# three, the prompts were stacked: their placement once looped forever, the game locked up, 2026-10-28). build/window.png (frame 2300). The log should go on presenting to the end ("[vk] t=" lines),
# with no "[hang]". The window: 3840x2071 (the user's; $WINDOW), where the old rounding tripped below y 1024. [exe]: build/win-release/lasrecomp.exe to run the release build (its own game code, beside it, copied in for it).
cd "$(git rev-parse --show-toplevel)" || exit 1
export PATH="/c/msys64/ucrt64/bin:$PATH"
EXE=${1:-build/lasrecomp.exe}
mkdir -p save/save_0 build/test_ini
TITLE=$(python -c "
s=[f'{f}:L:10,{f}:R:10' for f in range(400, 1400, 90)] + [f'{f}:A:8' for f in range(1400, 1800, 100)]
print(','.join(s))")
# (the three beside him step back and forth a little - in and out of each other's way, the camera moving: real play's
# changing positions, where the old placement's rounding tripped)
JIG=$(python -c "
dirs = ['LEFT', 'UP', 'RIGHT', 'DOWN']
for k in range(3):
    out, f, i = [], 2100 + k * 5, k
    while f < 3300:  # (a slow circle round him, steps of uneven length: a new spot nearly every frame)
        n = 2 + (i * 7 + k) % 4
        out.append(f'{f}:STICK_{dirs[i % 4]}:{n}')
        f += n + 1 + (i % 3)
        i += 1
    print(','.join(out))")
J3=$(echo "$JIG" | sed -n 2p); J4=$(echo "$JIG" | sed -n 3p)  # (Player 1 stays: the warp hole is beside him)
rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
cp build/save_seed/* save/save_0/
rm -f build/window.png
[ "$EXE" != build/lasrecomp.exe ] && cp "$(dirname "$EXE")/lasgame.dll" "$(dirname "$EXE")/lasgame.dll.id" extracted/
: > build/test_ini/hudpair.ini
env LARECOMP_NO_LAUNCHER=1 LARECOMP_SETTINGS=build/test_ini/hudpair.ini LARECOMP_NO_AUDIO=1 LARECOMP_COOP=4 LARECOMP_COOP_LOG=1 \
    LARECOMP_COOP_DOWN=2 LARECOMP_WINDOW=${WINDOW:-3840x2071} "LARECOMP_INPUT=$TITLE" "LARECOMP_INPUT_P3=$J3" "LARECOMP_INPUT_P4=$J4" LARECOMP_WINDOW_SHOT=2300 LARECOMP_HANG_DUMP=10 \
    "./$EXE" extracted --play > build/run_hudpair.log 2>&1 &
P=$!
for i in $(seq 1 60); do kill -0 $P 2>/dev/null || break; sleep 1; done
kill $P 2>/dev/null; sleep 1
ROOT=$(cygpath -w "$PWD")
powershell -NoProfile -Command "Get-Process lasrecomp -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$ROOT\*' } | Stop-Process -Force" >/dev/null 2>&1
[ "$EXE" != build/lasrecomp.exe ] && rm -f extracted/lasgame.dll extracted/lasgame.dll.id
grep -h "knocked out\|\[hang\] =====" build/run_hudpair.log | head -4
grep -a "^\[vk\] t=" build/run_hudpair.log | tail -1 | cut -c1-60
