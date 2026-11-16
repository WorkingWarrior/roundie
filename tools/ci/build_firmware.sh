#!/bin/bash
# Builds one firmware configuration as CI does (docs/ci.md), with the
# README's idf.py command and the board and profile options:
#
#   tools/ci/build_firmware.sh BOARD PROFILE
#
# BOARD: waveshare_s3_touch_lcd_1_85b or waveshare_s3_touch_amoled_1_75c;
# PROFILE: legacy or rep. Needs the ESP-IDF v6.1 environment (export.sh, or
# the espressif/idf image). The build directory is build-ci/BOARD-PROFILE with
# its sdkconfig inside, so no configuration in the checkout is touched.
#
# Reports in ${ROUNDIE_CI_REPORTS:-ci-reports}: firmware-BOARD-PROFILE.log
# (the full build), .size.txt (idf.py size) and .md (summary). The build fails
# on a compiler or linker warning: all four configurations build without one.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 BOARD PROFILE" >&2
    exit 2
fi
board=$1
profile=$2
name=$board-$profile
build=build-ci/$name
reports=${ROUNDIE_CI_REPORTS:-ci-reports}
mkdir -p "$reports"
log=$reports/firmware-$name.log
summary=$reports/firmware-$name.md

status=0
idf.py -B "$build" -DROUNDIE_BOARD="$board" -DROUNDIE_PROFILE="$profile" \
    -DSDKCONFIG="$build/sdkconfig" build > "$log" 2>&1 || status=$?
warnings=$(grep -c 'warning:' "$log" || true)

image=""
if [ "$status" -eq 0 ]; then
    idf.py -B "$build" -DROUNDIE_BOARD="$board" -DROUNDIE_PROFILE="$profile" \
        -DSDKCONFIG="$build/sdkconfig" size > "$reports/firmware-$name.size.txt" 2>&1 || status=$?
    image=$(grep -o 'Roundie layout: .*' "$log" | head -1 || true)
fi

verdict=PASS
if [ "$status" -ne 0 ] || [ "$warnings" -ne 0 ]; then
    verdict=FAIL
fi
{
    echo "### $verdict — firmware $board, $profile"
    echo
    echo "idf.py exit $status, $warnings warning(s). ${image}"
    echo
    if [ -f "$reports/firmware-$name.size.txt" ]; then
        echo '```'
        grep -E 'binary size|Flash Code|Flash Data|^│ (DIRAM|IRAM|RTC SLOW|RTC FAST) |Total image size' \
            "$reports/firmware-$name.size.txt" | head -20 || true
        echo '```'
    fi
    if [ "$warnings" -ne 0 ]; then
        echo
        echo '```'
        grep 'warning:' "$log" | head -50
        echo '```'
    fi
    echo
} > "$summary"
cat "$summary"

if [ "$verdict" = FAIL ]; then
    if [ "$status" -ne 0 ]; then
        tail -60 "$log" >&2
    fi
    exit 1
fi
