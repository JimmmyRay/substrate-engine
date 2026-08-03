#!/usr/bin/env bash
#
# Build and run the unit suite (5.1).
#
#   ./test.sh                  debug build
#   ./test.sh release
#   ./test.sh asan             AddressSanitizer + UndefinedBehaviorSanitizer
#   ./test.sh tsan             ThreadSanitizer
#   ./test.sh -- --gtest_filter=Profiler*      args after -- go to the test binary
#
# The suite links only the hosted translation units -- no Vulkan, no window, no
# shaders -- which is the whole reason it exists in this shape: `./run.sh tsan` cannot
# run the renderer at all, because the proprietary NVIDIA driver segfaults inside
# vkCreateDevice under ThreadSanitizer. `./test.sh tsan` is where the profiler's and
# the logger's threading actually gets checked.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

CONFIG="debug"
if [ $# -gt 0 ] && [ "$1" != "--" ]; then
    case "$1" in
    debug | release | asan | tsan)
        CONFIG="$1"
        shift
        ;;
    -h | --help | help)
        usage
        exit 0
        ;;
    *)
        # The branch build.sh has had all along and this script did not. Without it
        # `./test.sh releas` built debug, passed `releas` to the test binary as a gtest
        # argument, and reported a green suite for a configuration nobody ran.
        die_unknown_config "$1"
        ;;
    esac
fi
[ "${1:-}" = "--" ] && shift

# Carried through rather than left unset, because build.sh clears the cache variable
# whenever the environment does not name a game -- and running the tests must not
# silently un-configure the game a build directory holds. This script builds one target
# and changes nothing else about the directory.
if [ -f "build/$CONFIG/CMakeCache.txt" ]; then
    SUBSTRATE_GAME="$(awk -F= '/^SUBSTRATE_GAME:STRING=/ { print $2 }' "build/$CONFIG/CMakeCache.txt")"
    export SUBSTRATE_GAME
fi

# Held across the build and the exec of what it produced, for the reason run.sh holds it:
# a concurrent link unlinks its output, so `substrate_tests` can cease to exist between
# being built and being run. See build_lock in scripts/common.sh.
build_lock "build/$CONFIG"
./build.sh "$CONFIG" --target substrate_tests

BIN="build/$CONFIG/substrate_tests"
if [ ! -x "$BIN" ]; then
    echo "error: $BIN does not exist after building." >&2
    exit 1
fi

sanitizer_env "$CONFIG"

# The python suites, before the C++ one and only when no gtest argument was given -- a
# `--gtest_filter` says the caller is after one C++ case and does not want a second suite's
# output in front of it.
#
# **They are here because they were nowhere.** `tests/manifest_test.py` unpacked three of the
# four values `manifest.build` returns and had been failing since the fourth was added; the
# only runner was the CI workflow, so a green `./test.sh` said nothing about it and nobody
# reading a local green had any reason to look. Cheap enough to be unconditional: three
# suites, about twenty milliseconds, no build.
#
# Skipped rather than failed when python3 is absent, for the same reason `fetch_assets.sh`
# skips its generators -- python is not a build dependency of the engine and refusing to run
# the C++ tests without it would make it one.
if [ "$#" -eq 0 ]; then
    if command -v python3 >/dev/null 2>&1; then
        for suite in tests/*_test.py; do
            [ -f "$suite" ] || continue
            printf '==> %s\n' "$suite"
            python3 "$suite" || exit 1
        done
    else
        echo "warning: python3 not found; skipping the python suites" >&2
    fi
fi

# The lock does not survive into the suite: a shell-opened descriptor outlives exec, and a
# TSan run holding it would block every build in the tree for its duration.
exec 9>&-

exec "${LAUNCH[@]}" "$BIN" "$@"
