#!/usr/bin/env bash
#
# Configure and build Substrate.
#
#   ./build.sh                 debug build (default)
#   ./build.sh release
#   ./build.sh asan            AddressSanitizer + UndefinedBehaviorSanitizer
#   ./build.sh tsan            ThreadSanitizer
#   ./build.sh clean           remove every build directory
#   ./build.sh debug --target shaders     extra args pass through to cmake --build
#
# This builds the *engine* and the unit suite, and produces no runnable binary: the
# engine has to build, test and sanitize with nothing under game/ in the tree, which is
# what makes a dependency leaking from a game into engine/ a link error. `./build_game.sh
# <name>` is what produces a program; it calls this script with SUBSTRATE_GAME set.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

CONFIG="${1:-debug}"
[ $# -gt 0 ] && shift

case "$CONFIG" in
clean)
    rm -rf build compile_commands.json
    echo "removed build/ and the compile_commands.json symlink"
    exit 0
    ;;
debug)
    BUILD_DIR="build/debug"
    CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Debug)
    ;;
release)
    BUILD_DIR="build/release"
    CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)
    ;;
asan)
    BUILD_DIR="build/asan"
    CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Debug
                -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
                -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined")
    ;;
tsan)
    BUILD_DIR="build/tsan"
    CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Debug
                -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
                -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread")
    ;;
-h | --help | help)
    usage
    exit 0
    ;;
*)
    echo "error: unknown config '$CONFIG' (want: debug|release|asan|tsan|clean)" >&2
    exit 1
    ;;
esac

# Submodules must exist before CMake runs; the guard in CMakeLists fires otherwise,
# but catching it here gives a shorter path to the fix.
if [ ! -f external/glfw/CMakeLists.txt ] || [ ! -f external/fastgltf/CMakeLists.txt ] ||
    [ ! -f external/meshoptimizer/CMakeLists.txt ]; then
    echo "==> initialising submodules"
    git submodule update --init --recursive
fi

# Passed on every configure, including when it is empty: that is what clears a game a
# previous `./build_game.sh` recorded, so `./build.sh` always means "engine and tests
# only". The engine's objects survive the toggle either way.
CMAKE_ARGS+=(-DSUBSTRATE_GAME="${SUBSTRATE_GAME:-}")

build_lock "$BUILD_DIR"

echo "==> configuring $CONFIG in $BUILD_DIR/"
# The first configure downloads the simdjson amalgamation for fastgltf and needs
# network access; it is cached in the build directory afterwards.
cmake -B "$BUILD_DIR" -G Ninja "${CMAKE_ARGS[@]}"

echo "==> building"
cmake --build "$BUILD_DIR" "$@"

# clangd and other tooling look for this at the repo root.
if [ -f "$BUILD_DIR/compile_commands.json" ] && [ "$CONFIG" = "debug" ]; then
    ln -sf "$BUILD_DIR/compile_commands.json" compile_commands.json
fi

# Asserted, not announced. This line used to print unconditionally, so it claimed a file
# existed without anyone having looked -- and a golden case then failed on the very next
# statement in run.sh because it did not. A "done" that is not a check is worse than no
# line at all: it is evidence pointing away from the thing that went wrong.
#
# Only for a default build. `./build.sh debug --target shaders` is asked to produce
# something else and asserting the library there would fail a build that did what it was
# told.
if [ $# -gt 0 ]; then
    echo "==> done: $BUILD_DIR ($*)"
else
    if [ -n "${SUBSTRATE_GAME:-}" ]; then
        ARTIFACT="$BUILD_DIR/$SUBSTRATE_GAME"
        SUFFIX=""
    else
        ARTIFACT="$BUILD_DIR/libsubstrate.a"
        SUFFIX=" (no game configured; ./build_game.sh <name>)"
    fi
    if [ ! -e "$ARTIFACT" ]; then
        echo "error: cmake --build reported success but $ARTIFACT does not exist." >&2
        exit 1
    fi
    echo "==> done: $ARTIFACT$SUFFIX"
fi
