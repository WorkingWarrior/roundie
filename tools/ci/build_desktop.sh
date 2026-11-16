#!/bin/bash
# Builds Roundie Desktop as docs/roundie-core.md describes (meson setup,
# ninja), in build-ci/desktop, as CI does (docs/ci.md). LVGL comes from
# managed_components (a firmware build or idf.py reconfigure fetches it).
#
#   tools/ci/build_desktop.sh
#
# Reports in ${ROUNDIE_CI_REPORTS:-ci-reports}: desktop-build.log and .md.
# Roundie's sources build with -Werror; LVGL builds with -w, as in the
# firmware. A warning in the log fails the build.
set -euo pipefail

build=build-ci/desktop
reports=${ROUNDIE_CI_REPORTS:-ci-reports}
mkdir -p "$reports"
log=$reports/desktop-build.log
rm -rf "$build"

status=0
{
    meson setup "$build" -Ddesktop=enabled && ninja -C "$build"
} > "$log" 2>&1 || status=$?
warnings=$(grep -c 'warning:' "$log" || true)
versions=$(pkg-config --modversion sdl3 vulkan tfpsacrypto 2> /dev/null | paste -sd ' ' || true)

verdict=PASS
if [ "$status" -ne 0 ] || [ "$warnings" -ne 0 ]; then
    verdict=FAIL
fi
{
    echo "### $verdict — desktop build (meson, SDL3, Vulkan)"
    echo
    echo "meson/ninja exit $status, $warnings warning(s); $(meson --version 2> /dev/null || true) meson,"\
         "SDL3/Vulkan/TF-PSA-Crypto $versions, $(cc --version | head -1)."
    if [ "$warnings" -ne 0 ]; then
        echo
        echo '```'
        grep 'warning:' "$log" | head -50
        echo '```'
    fi
    echo
} > "$reports/desktop-build.md"
cat "$reports/desktop-build.md"

if [ "$verdict" = FAIL ]; then
    tail -60 "$log" >&2
    exit 1
fi
