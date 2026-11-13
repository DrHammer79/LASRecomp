#!/bin/bash
# tools/coop_tests/run_test.sh <id> <seconds> [ENV=value...]: boots the dev build straight into the game (no start screen, its
# own settings file build/test_ini/<id>.ini - made empty, so the defaults, when missing - no audio) with the extra
# environment, logs to build/run_<id>.log and stops it after <seconds>. Only LAS Recomp processes started from this
# checkout are stopped. Run from an MSYS2 UCRT64 shell; the checkout's own build (make) must be there.
# EXE=<path to another lasrecomp.exe> runs that build instead (a release, the one before a change): its own game code,
# the lasgame.dll(.id) beside it, is put in extracted/ for the run and taken out after.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; T=$2; shift 2
export PATH="/c/msys64/ucrt64/bin:$PATH"
EXE=${EXE:-build/lasrecomp.exe}
mkdir -p build/test_ini
[ -f build/test_ini/$ID.ini ] || : > build/test_ini/$ID.ini
[ "$EXE" != build/lasrecomp.exe ] && cp "$(dirname "$EXE")/lasgame.dll" "$(dirname "$EXE")/lasgame.dll.id" extracted/
env LARECOMP_NO_LAUNCHER=1 LARECOMP_SETTINGS=build/test_ini/$ID.ini LARECOMP_NO_AUDIO=1 SHOT_ID=$ID "$@" \
    "./$EXE" extracted --play > build/run_$ID.log 2>&1 &
P=$!
for i in $(seq 1 $T); do kill -0 $P 2>/dev/null || break; sleep 1; done
kill $P 2>/dev/null; sleep 1
ROOT=$(cygpath -w "$PWD")
powershell -NoProfile -Command "Get-Process lasrecomp -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$ROOT\\*' } | Stop-Process -Force" >/dev/null 2>&1
[ "$EXE" != build/lasrecomp.exe ] && rm -f extracted/lasgame.dll extracted/lasgame.dll.id
echo "$ID: stopped after ${i}s (build/run_$ID.log)"
