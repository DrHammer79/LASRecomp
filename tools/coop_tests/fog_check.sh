#!/bin/bash
# tools/coop_tests/fog_check.sh: the Mysterious Woods' fog seen from Mabe Village zoomed out (user 2026-10-30: "fog border
# jacked" - a hard-edged blue band over the woods at the view's top, P1 about (23, 10.5, 110), zoom 2.6, a 32:9-ish
# window). fogA: as it is; fogB: the woods' fog not stretched (LARECOMP_NO_ZOOM_STRETCH). Shots build/frames/fog?_*.
cd "$(git rev-parse --show-toplevel)" || exit 1
for v in A B; do
    : > build/test_ini/fog$v.ini
    X=(); [ $v = B ] && X=(LARECOMP_NO_ZOOM_STRETCH=1)
    SECS=80 tools/coop_tests/ic.sh fog$v 2 Field:Field_10B "" "" 2600,2700 LARECOMP_PLACE=2300:23:110 LARECOMP_PLACE_Y=10.5 \
        LARECOMP_COOP_ZOOM=2.6 LARECOMP_WINDOW=1920x1035 "${X[@]}" SDL_XINPUT_ENABLED=0 SDL_JOYSTICK_RAWINPUT=0 \
        SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_WGI=0 SDL_JOYSTICK_DIRECTINPUT=0 SDL_JOYSTICK_GAMEINPUT=0
done
ls build/frames | grep "^fog"
