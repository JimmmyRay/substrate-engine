#!/usr/bin/env bash
#
# Run Substrate with a sane environment.
#
#   ./run.sh                              the engine's test scene, debug
#   ./run.sh demo                         game/demo/, debug
#   ./run.sh demo release                 game/demo/, release
#   ./run.sh release                      the test scene, release
#   ./run.sh asan
#   ./run.sh tsan
#   ./run.sh release -- --msaa 8 --frames 900 --trace bench/8x.json
#   ./run.sh -- path/to/other.gltf
#
# The two leading arguments are a game name and a configuration, in either order, and
# both are optional. A name is a directory under `game/`; a configuration is one of
# debug, release, asan or tsan. `--list` names the games.
#
# Naming no game runs the engine's own test scene rather than a game's, which is what
# makes `./run.sh` on a fresh clone do something rather than explain itself. Naming one
# runs that game, and lets the scene its `configure` names decide what it opens.
#
# Everything after `--` is passed straight to the executable:
#   [scene.gltf] [--msaa 1|2|4|8] [--frames N] [--trace path]
#   [--record [seconds]] [--record-file path]
#
# `--record` writes debug_frames/session.mp4 -- the presented frames and the mixed
# audio, recorded while you play, keeping the last 30 seconds by default. It records
# what Substrate drew, not what is on the screen, so nothing else on the display can
# end up in it. It needs ffmpeg on PATH.
#
# `./build.sh` produces no runnable binary -- `game/<name>/` is what builds a program --
# so this script builds one before every run. Always: what you launch is what the tree
# currently says, sources and shaders alike, and never a binary left over from before an
# edit. An up-to-date build costs about 0.7s. Switching the configuration to a game it
# does not hold costs a reconfigure and a link, not a rebuild -- the engine's object
# files survive the toggle.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Asset and log paths in the app are relative to the working directory.
cd "$REPO_ROOT"

CONFIG=""
GAME=""

# Up to two leading tokens, in either order, so `./run.sh demo release` and
# `./run.sh release demo` are the same command. Order-independence is worth the loop:
# the alternative is remembering which slot a name goes in, and getting it wrong reads
# as "no such configuration" rather than as a swapped argument.
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    case "$1" in
    debug | release | asan | tsan)
        CONFIG="$1"
        ;;
    -h | --help | help)
        usage
        echo "games:"
        list_games
        exit 0
        ;;
    --list | list)
        list_games
        exit 0
        ;;
    *)
        if is_game "$1"; then
            GAME="$1"
        else
            echo "error: '$1' is neither a configuration nor a game." >&2
            echo "       configurations: debug release asan tsan" >&2
            echo "games:" >&2
            list_games >&2
            exit 1
        fi
        ;;
    esac
    shift
done
[ "${1:-}" = "--" ] && shift

# Whether a game was asked for by name, as opposed to inherited from the build
# directory. It is what decides which scene opens, so it has to be recorded before the
# fallbacks below fill GAME in.
NAMED_GAME=0
[ -n "$GAME" ] && NAMED_GAME=1

CONFIG="${CONFIG:-debug}"
BUILD_DIR="build/$CONFIG"

# Before the build, not after it. The engine refuses `--record` without ffmpeg too, but
# it does so once the window is open and several minutes of compiling have gone by, and
# a missing dependency is worth hearing about while you can still act on it.
for arg in "$@"; do
    if [ "$arg" = "--record" ] && ! command -v ffmpeg >/dev/null 2>&1; then
        echo "error: --record needs ffmpeg on PATH." >&2
        echo "       Debian/Ubuntu: sudo apt install ffmpeg" >&2
        echo "       Fedora:        sudo dnf install ffmpeg" >&2
        exit 1
    fi
done

cached_game() {
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || return 0
    awk -F= '/^SUBSTRATE_GAME:STRING=/ { print $2 }' "$BUILD_DIR/CMakeCache.txt"
}

# With no game named, `game/viewer` -- which opens a scene and composes nothing.
#
# It used to be `cached_game()`, whichever game the build directory happened to hold, and
# that made `./run.sh` and `scripts/golden.sh` mean different things on different days: a
# game builds its own world in `init` now, so a suite that named no game got that world in
# its baselines. Every game in the tree composes unconditionally because this line does not
# hand them the harness's job.
VIEWER_GAME="viewer"
if [ -z "$GAME" ]; then
    if [ ! -f "game/$VIEWER_GAME/CMakeLists.txt" ]; then
        echo "error: game/$VIEWER_GAME is what ./run.sh opens when no game is named, and it" >&2
        echo "       is not in the tree. Name one:" >&2
        list_games >&2
        exit 1
    fi
    GAME="$VIEWER_GAME"
fi

BIN="$BUILD_DIR/$GAME"

# Always build, and never conditionally. This used to run only when the cache named a
# different game or the binary was missing -- which is precisely the set of conditions a
# *source edit* does not change. Editing a shader or a .cpp and running launched the
# previous binary and said nothing, so the run looked like the change had no effect,
# which is indistinguishable from the change being wrong. That has now cost two debugging
# sessions chasing a fix that was never in the binary under test.
#
# A no-op is a guarded submodule check, a cmake configure that re-reads its cache and a
# ninja that builds nothing: 0.7s. A silently stale binary costs a session. There is no
# version of this trade worth the conditional, so there is no conditional.
#
# It also means the shaders are current: they are build targets, so the same no-op
# recompiles any that changed and a stale .spv cannot outlive its source either.
if [ "$(cached_game)" != "$GAME" ]; then
    echo "==> $BUILD_DIR does not hold $GAME; switching it"
fi

# Taken here rather than inside build.sh, and released only after the check below,
# because the check is inside the window it protects: a concurrent link unlinks $BIN
# before rewriting it, so a lock that ended when the build did would leave exactly the
# gap this guard used to fire in. See build_lock in scripts/common.sh.
build_lock "$BUILD_DIR"
./build_game.sh "$GAME" "$CONFIG"

if [ ! -x "$BIN" ]; then
    echo "error: $BIN still does not exist after building." >&2
    exit 1
fi

# ------------------------------------------------------------------------ scene

# The engine's own test scene, used when no game was named. Sponza rather than one of
# the small generated scenes because it is what the golden suite pins and what the
# renderer is tuned against, so `./run.sh` and `scripts/golden.sh` open the same thing.
ENGINE_SCENE="engine/assets/Sponza/glTF/Sponza.gltf"

# A scene named after `--` wins over both. Recognised by extension rather than by
# position, because the flags after `--` are order-free and golden.sh puts its scene
# last. Without this, a case that names its own scene would get two.
has_scene=0
for arg in "$@"; do
    case "$arg" in
    *.gltf | *.glb | res:/*) has_scene=1 ;;
    esac
done

SCENE_ARG=()
if [ "$NAMED_GAME" = 0 ] && [ "$has_scene" = 0 ]; then
    # Checked here rather than unconditionally above, which is where it used to sit.
    # A named game opens whatever its `GameSetup::scene` names, and a scene given after
    # `--` opens itself; neither reads this file. Aborting on it regardless
    # meant the smallest game guides/making-a-game.md documents could not be run without
    # first fetching an asset it never loads.
    if [ ! -f "$ENGINE_SCENE" ]; then
        echo "error: Sponza is missing, and it is what ./run.sh opens when no game is" >&2
        echo "       named. Run: scripts/fetch_assets.sh" >&2
        echo "       Or name a game (./run.sh --list) or a scene (./run.sh -- x.gltf)." >&2
        exit 1
    fi
    SCENE_ARG=("$ENGINE_SCENE")
fi

# ---------------------------------------------------------------- environment

# VK_LAYER_PATH is deliberately left alone. VulkanContext::init detects the case
# where it hides the system layers and appends the standard directories itself, so
# validation works no matter how the binary is launched — not only through here.

sanitizer_env "$CONFIG"

mkdir -p debug_frames

# Before the exec, and it has to be explicit: a shell-opened descriptor survives exec, so
# the build lock would otherwise be held for the whole run -- a 120-second golden capture
# blocking every build in the tree behind it.
exec 9>&-

echo "==> ${LAUNCH[*]:-} $BIN ${SCENE_ARG[*]:-} $*"
exec "${LAUNCH[@]}" "$BIN" "${SCENE_ARG[@]}" "$@"
