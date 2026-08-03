#!/usr/bin/env bash
#
# Build the engine plus one game.
#
#   ./build_game.sh demo                 debug build of game/demo/
#   ./build_game.sh demo release
#   ./build_game.sh mygame asan
#   ./build_game.sh --list               names every game/<name>/ in the tree
#
# `./build.sh` builds the engine and the unit suite and produces no runnable binary --
# that is the split, and it is what makes a dependency leaking from game/ into engine/ a
# link error rather than a code review. This is the script that produces a program.
#
# Which game a build directory holds is a property of the build directory: the name is
# recorded in the CMake cache, so `./run.sh`, `scripts/golden.sh` and
# `scripts/baseline.py` all keep the signatures they have and need no --game flag. The
# engine's object files survive the toggle, so alternating between two games -- or back
# to `./build.sh` -- costs a reconfigure rather than a rebuild.
#
# Everything after the configuration is passed on to `cmake --build`, exactly as
# build.sh does with it.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

GAME="${1:-}"
check_game "./build_game.sh <name> [debug|release|asan|tsan]" "$GAME"
shift

CONFIG="debug"
if [ $# -gt 0 ]; then
    case "$1" in
    debug | release | asan | tsan)
        CONFIG="$1"
        shift
        ;;
    esac
fi

# build.sh owns the configure line, the submodule check and the sanitizer flags, and it
# reads SUBSTRATE_GAME from the environment. Reimplementing any of that here would be a
# second copy that drifts the first time one of them changes.
SUBSTRATE_GAME="$GAME" ./build.sh "$CONFIG" "$@"
