#!/bin/bash
# tools/coop_tests/zoom_solo.sh: the camera zoom's "Zoom out" (coop_zoom_mode) - alone and with co-op, on the Field and indoors.
# Each run boots file 1 of the save seed (Tal Tal Heights), optionally warps at frame 2000, and saves frame 2600 to
# build/frames/<id>_frame_02600.png. Compare: zsolo_def / zcoop_off the game's own view, zsolo_on / zcoop_def out 50%,
# zsolo_in out 30% indoors, zsolo_in_def the game's own.
cd "$(git rev-parse --show-toplevel)" || exit 1
run() {
    local id=$1 n=$2 w=$3; shift 3
    mkdir -p build/test_ini
    : > build/test_ini/$id.ini
    for kv in "$@"; do echo "${kv%%=*} = ${kv#*=}" >> build/test_ini/$id.ini; done
    SECS=45 tools/coop_tests/ic.sh $id $n $w "" "" 2600
}
run zsolo_def 1 -
run zsolo_on 1 - coop_zoom_mode=1
run zcoop_off 2 - coop_zoom_mode=2
run zcoop_def 2 -
run zsolo_in 1 MarinTarinHouse:MarinTarinHouse_01A coop_zoom_mode=1 coop_zoom_indoor=3
run zsolo_in_def 1 MarinTarinHouse:MarinTarinHouse_01A coop_zoom_indoor=3
ls -l build/frames/z*_frame_02600.png
