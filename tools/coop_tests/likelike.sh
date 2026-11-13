#!/bin/bash
# tools/coop_tests/likelike.sh <id> [extra env...]: a Like Like catching Player 2 (user 2026-10-30: one caught the player it went
# for by Player 1's "Link"). The seed save warped to Goponga Swamp's phone booth (Field_04B, Player 1 arrives at 24.0,
# 44.25; Like Likes around it): Player 1 put 12 units east at frame 2120, Player 2 put a unit from the Like Like
# EnemyLikeLike-6A0357 (17.75, 43.0) at 2250 (it wanders at random: put further off, it may not reach him). Since
# 2026-11-01's fix (COOP_LIKELIKE_SITE_0..8) it hits him, is on him by ~2280, catches him (player 2 state kind 23) and
# spits him out; then the game's "Did...did that thing eat it?!" (Player 1 in an event). Before, within 15 frames of
# the hit it flew onto Player 1 (18.75 -> 36.0) and Player 2 wasn't caught. [trace]: the Like Like every EVERY frames
# (10), and whom it goes for.
cd "$(git rev-parse --show-toplevel)" || exit 1
ID=$1; shift
SECS=${SECS:-55} tools/coop_tests/ic.sh $ID 2 Field:Field_04B - - ${SHOTS:-0} LARECOMP_COOP_TRACE=EnemyLikeLike-6A0357 \
    LARECOMP_COOP_TRACE_EVERY=${EVERY:-10} LARECOMP_PLACE=2120:36.0:44.25 LARECOMP_COOP_MOVE=2250:2:17.75:13.5:43.0 \
    SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 \
    SDL_JOYSTICK_GAMEINPUT=0 "$@" > /dev/null
awk '/^\[coop\] frame [0-9]+ / {f=$3; p=$4" "$5" "$7" "$8" "$9" "$11} /state kind|hit for|in an event|goes for/ {print f": "$0}
    /^\[trace\] frame [0-9]+ EnemyLike/ && $4 !~ /goes/ && $3 % 30 == 0 && $3 >= 2200 && $3 <= 2800 {print $3": "p" LL "$5" "$7}' \
    build/run_$ID.log | cut -c1-100
