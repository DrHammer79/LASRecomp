#!/bin/bash
# tools/coop_tests/zoom_audit.sh [zoom]: the co-op camera zoomed out (default 2.0, LARECOMP_COOP_ZOOM) across the world - overworld
# areas, dungeons, interiors - one game frame each (build/frames/za_<n>_<level>.png), for camera-attached effects
# (fog, darkness, backdrops) showing an edge, and interiors showing past their walls.
cd "$(git rev-parse --show-toplevel)" || exit 1
Z=${1:-2.0}
PLACES="Field:Field_02B Field:Field_01G Field:Field_03N Field:Field_04P Field:Field_05J Field:Field_06C Field:Field_08F
Field:Field_09E Field:Field_10N Field:Field_11L Field:Field_12D Field:Field_13G Field:Field_13P Field:Field_14N
Field:Field_15D Field:Field_16J
Lv01TailCave:Lv01TailCave_08D Lv02BottleGrotto:Lv02BottleGrotto_03F Lv03KeyCavern:Lv03KeyCavern_01F
Lv05CatfishsMaw:Lv05CatfishsMaw_01C Lv06FaceShrine:Lv06FaceShrine_01A Lv07EagleTower:Lv07EagleTower_01B
Lv08TurtleRock:Lv08TurtleRock_01A Lv09WindFishsEgg:Lv09WindFishsEgg_01D Lv10ClothesDungeon:Lv10ClothesDungeon_04B_Warp
Lv04AnglersTunnel:Lv04AnglersTunnel_08D
MarinTarinHouse:MarinTarinHouse_01A Library:Library_01A MadamHouse:MadamHouse_01A Shop:Shop_01A
DanpeiHouse:DanpeiHouse_01A_a RichardHouse:RichardHouse_01A_a CraneGame:CraneGame_01A FishingPond:FishingPond_01A
RapidsRideTicket:RapidsRideTicket_01A GhostHouse:GhostHouse_01A DreamShrine:DreamShrine_02A
MysteriousWoodsCave01:MysteriousWoodsCave01_01A MamboCave:MamboCave_01A UlriraHouse:UlriraHouse_01A"
n=0
for p in $PLACES; do
    n=$((n + 1)); id=za; name=$(printf '%02d_%s' $n "${p%%:*}")
    rm -f save/save_0/savedata2.bin save/save_0/screenshot2.bin  # (the seed has no slot 3: none left from another run)
    cp build/save_seed/* save/save_0/
    SECS=45 tools/coop_tests/ic.sh $id 2 "$p" "" "" 2600 LARECOMP_COOP_ZOOM=$Z > /dev/null 2>&1
    if [ -f build/frames/za_frame_02600.png ]; then mv build/frames/za_frame_02600.png "build/frames/za_${name}.png"; r=ok; else r="no frame"; fi
    echo "$name ${p#*:}: $r, $(grep -c 'FATAL' build/run_za.log) errors"
done
