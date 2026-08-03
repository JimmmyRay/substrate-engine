#!/usr/bin/env bash
#
# Bake the scene sidecar a game reads at load time (D9).
#
#   scripts/bake.sh engine/assets/Sponza/glTF/Sponza.gltf
#   scripts/bake.sh debug some.gltf other.glb
#   scripts/bake.sh --quiet "$(scripts/manifest.py demo | awk '{print $1}' | grep '\.gltf$')"
#
# Writes `<scene>.scene` beside each document: the parsed scene and C17's LOD chains, in
# the form `scene::readSceneCache` takes. Nothing rewrites the glTF, and deleting a
# sidecar restores the original behaviour exactly -- the load falls back to the document,
# which is not an error and is not logged.
#
# `substrate-bake` links neither Vulkan nor a window, so this needs no GPU and no display.
# It used to: the bake was `--bake-scene` inside the engine, and producing a CPU-side
# artifact meant standing up a device, a swapchain and every texture upload first.
#
# The configuration is optional and defaults to release, because the bake is minutes of
# mesh simplification and a debug build spends most of them in glm.
#
# Who runs this: `build_release.sh`, once per scene the manifest resolves. Not
# `build.sh` -- a development build parses the document on purpose, which is what makes a
# stale sidecar impossible to hide behind.
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
    -h | --help | help)
        usage
        exit 0
        ;;
    esac
fi

if [ $# -eq 0 ]; then
    usage
    exit 1
fi

BUILD_DIR="build/$CONFIG"

# Passed back in rather than left to default, because `./build.sh` clears SUBSTRATE_GAME
# from the cache on every configure -- that is what makes it mean "engine and tests only".
# Baking a scene is not a reason to make the next `./run.sh` reconfigure and relink.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    SUBSTRATE_GAME="$(awk -F= '/^SUBSTRATE_GAME:STRING=/ { print $2 }' "$BUILD_DIR/CMakeCache.txt")"
    export SUBSTRATE_GAME
fi

# Always, and for run.sh's reason: what you bake with is what the tree currently says.
# A sidecar written by a stale binary is the exact failure C15's format version and layout
# digest exist to catch, and catching it at the next launch is later than catching it here.
./build.sh "$CONFIG" --target substrate-bake >/dev/null

exec "$BUILD_DIR/substrate-bake" "$@"
