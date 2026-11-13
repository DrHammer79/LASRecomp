#!/bin/bash
# tools/coop_tests/interact_check.sh: A-button actions with co-op (lift, throw, open), one taker per object, co-op off.
# Frames go to build/frames/<id>_frame_<n>.png, logs to build/run_<id>.log.
cd "$(git rev-parse --show-toplevel)" || exit 1
X=tools/coop_tests/ic.sh
M() { python -c "print(','.join(f'{f}:A:4' for f in range($1,$2,$3)))"; }
# Player 2 lifts the Tal Tal rock (Player 1 walked away) and throws it
TITLE_A="(1400,1480,1540)" SECS=45 $X p2rock 2 - "1850:STICK_LEFT:50" "1990:STICK_RIGHT:10,2050:A:8,2250:A:8" 1960,2120,2200,2260,2280,2300,2400
# Player 1 lifts it (Player 2 beside him), co-op on and off
TITLE_A="(1400,1480,1540)" SECS=40 $X p1rock 2 - "1900:A:8,2100:A:8" "" 1880,1960,2050,2110,2140
TITLE_A="(1400,1480,1540)" SECS=40 $X p1solo 1 - "1900:A:8,2100:A:8" "" 1880,1960,2050,2110,2140
# Player 2 lifts and throws a pot in Bottle Grotto
SECS=50 $X p2pot 2 Lv02BottleGrotto:Lv02BottleGrotto_03F "" "2400:STICK_RIGHT:4,2450:A:8,2600:A:8" 2440,2520,2600,2620,2650,2700
# Player 2 opens the chest at Field 07M; Player 1 closes the text; Player 2 walks off after
SECS=70 $X p2chest 2 Field:Field_07M "$(M 3300 3700 40)" "2400:STICK_LEFT:20,2440:STICK_UP:6,2470:A:8,3800:STICK_DOWN:30" 2490,2560,3000,3300,3750,3900
# Players 1 and 2 standing together press A in the same frame at the boulders (Field 08F): one lifts
SECS=50 $X same 4 Field:Field_08F "2400:STICK_LEFT:4,2450:A:8,2650:STICK_LEFT:40" "2400:STICK_LEFT:4,2450:A:8" 2440,2480,2520,2700 "LARECOMP_INPUT_P3=2452:A:8"
# Player 2 away from Player 1 presses A just as Player 1 lifts (before: he went into the same lift)
TITLE_A="(1400,1480,1540)" SECS=45 $X away 2 - "2100:A:8" "1700:STICK_DOWN:60,1780:STICK_LEFT:80,$(M 2090 2140 4)" 2110,2160,2200
# Boulder next to boulder: 4 players, Player 1 lifts, walks out and throws while the others mash A
SECS=55 $X mash4 4 Field:Field_08F "2400:STICK_LEFT:4,2450:A:8,2650:STICK_LEFT:40,2750:A:8" "$(M 2400 2900 20)" 2470,2520,2600,2700,2800,2900 "LARECOMP_INPUT_P3=$(M 2407 2900 23)" "LARECOMP_INPUT_P4=$(M 2413 2900 17)"
# Player 2 reads the sign at Field 04P; Player 1 closes the text
SECS=60 $X p2sign 2 Field:Field_04P "$(M 3000 3400 40)" "2400:STICK_DOWN:10,2440:STICK_RIGHT:4,2480:A:8,3500:STICK_LEFT:30" 2470,2560,2800,3450,3600
