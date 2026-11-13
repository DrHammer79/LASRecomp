#!/bin/bash
# tools/coop_tests/full_check.sh: every co-op regression script in turn
cd "$(git rev-parse --show-toplevel)" || exit 1
echo "### triggers"; bash tools/coop_tests/trig_check.sh
echo "### doors"; bash tools/coop_tests/door_check.sh
echo "### interaction"
X=tools/coop_tests/ic.sh
M() { python -c "print(','.join(f'{f}:A:4' for f in range($1,$2,$3)))"; }
TITLE_A="(1400,1480,1540)" SECS=45 $X p2rock 2 - "1850:STICK_LEFT:50" "1990:STICK_RIGHT:10,2050:A:8,2250:A:8" 2200
SECS=70 $X p2chest 2 Field:Field_07M "$(M 3300 3700 40)" "2400:STICK_LEFT:20,2440:STICK_UP:6,2470:A:8,3800:STICK_DOWN:30" 3000
grep -h "acts on\|can act on\|state kind 8" build/run_p2rock.log build/run_p2chest.log | head -4
echo "### side view"; SECS=55 $X sidefall 3 Lv02BottleGrotto:Lv02BottleGrotto_03F "2300:STICK_UP:40" "" 2600 > /dev/null
grep "\[coop\] frame 27[0-9][0-9]" build/run_sidefall.log | head -1 | cut -c1-100
echo "### hud and revive"; bash tools/coop_tests/hud_check.sh
