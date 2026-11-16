#!/bin/bash
# tools/coop_tests/hook_join.sh <id> [join|start] [extra env...]: Player 2's hookshot at an Octorok on open grass
# (Field_12D, Mabe Village's south: the Octorok at 20.25, 153.75). Player 1 is put east of it (LARECOMP_PLACE),
# Player 2 west of it facing it (LARECOMP_COOP_MOVE), and fires every 30 frames from 2480. "join": Player 2 leaves
# at 2120 and joins again in play at 2300 (his Link benched and woken); "bench": co-op on in the settings, Player 1
# alone as the file loads, Player 2 joins in play at 2300 (drop-in: his Link benched at the load, woken);
# "start" (default): in from the start.
# Prints the frames the Octorok is in line (its trace) - look at build/frames/<id>_frame_<f>.png for those: stars or
# sparks on it, a hit; the hook past it, none (user 2026-11-15: a player who joined in play shot through the enemies).
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; MODE=${2:-start}; shift; shift
IN=$(python -c "print(','.join(['2465:STICK_RIGHT:3']+[f'{f}:Y:3' for f in range(2480,3100,30)]))")
SH=$(python -c "print(','.join(str(f+10) for f in range(2480,3100,30)))")
J=()
N=2
[ "$MODE" = join ] && J=(LARECOMP_COOP_LEAVE=2120:2 LARECOMP_COOP_JOIN=2300:2)
: > build/test_ini/$ID.ini
[ "$MODE" = bench ] && J=(LARECOMP_COOP_JOIN=2300:2) && N=- && printf 'coop_players = 2\n' > build/test_ini/$ID.ini
SECS=${SECS:-70} tools/coop_tests/ic.sh $ID $N Field:Field_12D "" "$IN" "$SH" LARECOMP_HIDDEN=1 "${J[@]}" \
    LARECOMP_PLACE=2150:23.25:153.75 LARECOMP_COOP_MOVE=2420:2:16.25:9.38:153.75:hold \
    LARECOMP_COOP_TRACE=EnemyOctorok-48000E LARECOMP_COOP_TRACE_EVERY=2 "$@" > /dev/null
grep -E "player 2 (joined|left)|exception|crashed" build/run_$ID.log | cut -c1-90
for f in $(python -c "print(' '.join(str(f+10) for f in range(2480,3100,30)))"); do
    grep -E "trace\] frame $f Enemy" build/run_$ID.log | grep -v "goes for" |
        awk -v f=$f '{x=substr($5,2)+0; z=$7+0; if (z>153.2 && z<154.3 && x>17 && x<22.5) print f}'
done | tr '\n' ' '; echo
