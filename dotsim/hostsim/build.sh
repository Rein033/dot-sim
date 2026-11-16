#!/bin/bash
# build.sh <app-dir> <out-dir>: build one Kode app for the sim.
# The out-dir may be reused between apps: only what changed is rebuilt.
set -o pipefail
APP=$(realpath "$1"); OUT=$(realpath -m "$2")
HERE=$(dirname "$(realpath "$0")")
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/opt/esp/tools}" IDF_COMPONENT_MANAGER=0 KODE_APP_DIR="$APP"
. "${IDF_PATH:-/opt/esp/esp-idf}/export.sh" >/dev/null 2>&1 || { echo "cannot load ESP-IDF"; exit 3; }
mkdir -p "$OUT"
if [ ! -f "$APP/main/CMakeLists.txt" ]; then
    echo "no main/CMakeLists.txt: this is not an ESP-IDF app"
    exit 4
fi
# The app's own settings, minus what the sim decides: flash layout, USB, and
# the radio (the sim has none yet).
grep -v -E 'PARTITION_TABLE|ESPTOOLPY|TINYUSB|KHAL_CONSOLE_DOCK|^CONFIG_KODEOS_(CONSOLE|RADIO|WIFI|NET|MQTT|BLE|NFC|MIRROR|C5_HOLD)|^CONFIG_(ESP_HOSTED|BT)_' \
    "$APP/sdkconfig.defaults" 2>/dev/null > "$OUT/app.sdkconfig.new" || true
if ! cmp -s "$OUT/app.sdkconfig.new" "$OUT/app.sdkconfig"; then
    mv "$OUT/app.sdkconfig.new" "$OUT/app.sdkconfig"
    rm -f "$OUT/sdkconfig"
else
    rm -f "$OUT/app.sdkconfig.new"
fi
echo "== configuring"
idf.py -C "$HERE" -B "$OUT" -DSDKCONFIG="$OUT/sdkconfig" reconfigure > "$OUT/configure.log" 2>&1 || { tail -40 "$OUT/configure.log"; exit 5; }
echo "== compiling"
ninja -C "$OUT" -k 0 ${NINJA_ARGS:-} 2>&1 | grep -v '^\[[0-9]*/[0-9]*\] \(Building\|Linking\|Generating\)' 
rc=${PIPESTATUS[0]}
[ -f "$OUT/kode_sim_app.elf" ] && [ "$rc" = 0 ] && echo "== built" || echo "== failed ($rc)"
exit $rc
