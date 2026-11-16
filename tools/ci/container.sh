#!/bin/bash
# Runs a command in the Roundie CI image (tools/ci/Containerfile) with this
# checkout at /work, as the calling user rather than root, without network,
# without new privileges and without any host socket (docs/ci.md). Native REP
# test applications are untrusted code; this is where they run.
#
#   tools/ci/container.sh [--network] COMMAND [ARGUMENT]...
#
# ROUNDIE_CI_IMAGE (default roundie-ci) and ROUNDIE_CI_ENGINE (default podman,
# else docker) select the image and the container engine. --network is for
# `npm ci` only; tests run without it.
set -euo pipefail

image=${ROUNDIE_CI_IMAGE:-roundie-ci}
engine=${ROUNDIE_CI_ENGINE:-}
if [ -z "$engine" ]; then
    if command -v podman > /dev/null; then
        engine=podman
    else
        engine=docker
    fi
fi
root=$(cd "$(dirname "$0")/../.." && pwd)

network=(--network=none)
if [ "${1:-}" = "--network" ]; then
    network=()
    shift
fi
if [ "$#" -eq 0 ]; then
    echo "usage: $0 [--network] COMMAND [ARGUMENT]..." >&2
    exit 2
fi

# Rootless podman maps the caller to root unless told to keep the user id.
if [ "$engine" = podman ]; then
    user=(--userns=keep-id --group-add keep-groups)
else
    user=(--user "$(id -u):$(id -g)")
fi

# gamescope allocates its buffers as dma-bufs; Mesa lavapipe can export them
# only through /dev/udmabuf (memfd-backed, no GPU). Without the device,
# gamescope aborts on lavapipe and the graphics tests fail.
devices=()
if [ -r /dev/udmabuf ] && [ -w /dev/udmabuf ]; then
    devices=(--device /dev/udmabuf)
fi

exec "$engine" run --rm "${network[@]}" "${user[@]}" "${devices[@]}" \
    --security-opt no-new-privileges \
    --shm-size 1g \
    --volume "$root:/work" --workdir /work \
    --env HOME=/tmp/home --env XDG_RUNTIME_DIR=/tmp/runtime \
    --env CI="${CI:-}" \
    "$image" \
    bash -c 'mkdir -p "$HOME" && mkdir -m 0700 -p "$XDG_RUNTIME_DIR" && exec "$@"' bash "$@"
