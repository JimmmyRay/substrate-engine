#!/usr/bin/env sh
#
# Bootstrap `substrate` and hand it the command line.
#
#   scripts/substrate.sh <command> [args]
#
# Something has to run cmake before any C++ tool exists, and this is that something. It is
# the only shell script the project needs and the only one it has: every other entry point --
# scripts/build.sh, scripts/run.sh and their .cmd siblings -- is three lines that come
# straight back here.
#
# tools/ configures on its own, without the root project. That is why a fresh clone reaches a
# working `substrate` in seconds: no submodule download, no simdjson, no Jolt, no GLFW.
#
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

CLI_DIR="build/.cli"
CLI="$CLI_DIR/substrate"

# rapidjson is the CLI's one dependency and it is a submodule, so the check that would
# otherwise be a compiler error three minutes in happens here instead.
if [ ! -f external/rapidjson/include/rapidjson/document.h ]; then
    echo "==> initialising submodules"
    git submodule update --init --recursive
fi

# Configure once, build every time. The build is a ninja no-op in the ordinary case and
# costs a fraction of a second; skipping it is how a stale tool outlives the edit that was
# supposed to change it.
# Output is held rather than discarded: a build that fails here fails with no other
# explanation, and `exec` on a binary that was never produced says only "no such file".
run_quietly() {
    if ! output=$("$@" 2>&1); then
        printf '%s\n' "$output" >&2
        echo "error: could not build the substrate CLI (see above)" >&2
        exit 1
    fi
}

[ -f "$CLI_DIR/build.ninja" ] ||
    run_quietly cmake -B "$CLI_DIR" -S tools -G Ninja -DCMAKE_BUILD_TYPE=Release
run_quietly cmake --build "$CLI_DIR" --target substrate-cli

exec "./$CLI" "$@"
