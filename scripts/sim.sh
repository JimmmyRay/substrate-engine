#!/usr/bin/env bash
#
# Step a scene with no device (C27).
#
#   scripts/sim.sh engine/assets/Sponza/glTF/Sponza.gltf
#   scripts/sim.sh debug some.gltf --steps 10000
#   scripts/sim.sh --quiet some.gltf
#
# `substrate-sim` links neither Vulkan nor a window, so this needs no GPU and no display --
# which is the difference between it and `--headless`, and the whole point of the row. A
# headless run unmaps the window and still creates a real surface against a real driver; this
# creates neither, so it runs in a container, on a build machine, and under ThreadSanitizer.
#
# What it advances is `scene::Simulation::step`, which is the same call the drawn engine
# makes. A second copy of the call order living here is exactly what the row existed to stop.
#
# The configuration defaults to release: a batch job or a long soak wants the optimised
# solver, and the debug build spends most of a long run in glm. Pass `debug` for a run you
# intend to assert against under a sanitizer.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CONFIG="release"
if [ $# -gt 0 ]; then
    case "$1" in
    debug | release | asan | tsan)
        CONFIG="$1"
        shift
        ;;
    esac
fi

if [ $# -eq 0 ]; then
    "$0" --help 2>/dev/null || true
    echo "usage: scripts/sim.sh [debug|release|asan|tsan] <scene.gltf> [--steps N] [--gravity G] [--quiet]" >&2
    exit 1
fi

BUILD_DIR="build/$CONFIG"

# Passed back in rather than left to default, for bake.sh's reason: `./build.sh` clears
# SUBSTRATE_GAME on every configure, and stepping a scene is not a reason to make the next
# `./run.sh` reconfigure and relink the game.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    SUBSTRATE_GAME="$(awk -F= '/^SUBSTRATE_GAME:STRING=/ { print $2 }' "$BUILD_DIR/CMakeCache.txt")"
    export SUBSTRATE_GAME
fi

# Always, and for run.sh's reason: what you step with is what the tree currently says.
./build.sh "$CONFIG" --target substrate-sim >/dev/null

exec "$BUILD_DIR/substrate-sim" "$@"
