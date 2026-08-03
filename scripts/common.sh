#!/usr/bin/env bash
#
# Shared helpers for the five root scripts. Sourced, never executed.
#
# build.sh, build_game.sh, build_release.sh, run.sh and test.sh are the only supported
# way to build and run this project, and each of them used to carry its own copy of
# everything below -- usage() in all five, list_games() in three, the sanitizer
# environment in two. build_game.sh already showed the alternative by delegating its
# configure line to build.sh rather than reimplementing it; this file is that same move
# applied to the parts that have no obvious owner among the five.
#
# Nothing here sets `set -euo pipefail`. That belongs to the script the user ran, and a
# sourced file quietly changing the caller's shell options is how a helper becomes a
# thing you have to read before you can trust the script that sourced it.

# Print the leading comment block of the *invoking* script, minus the shebang, stopping
# at the first line of actual code. Beats hard-coded line ranges, which drift.
#
# BASH_SOURCE[-1] is the bottom of the call stack -- the script the user actually ran --
# so this reads the right header without every caller having to pass its own path in.
# BASH_SOURCE[0] would name this file, which is the bug the obvious version has.
usage() {
    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "${BASH_SOURCE[-1]}"
}

# Every game/<name>/ in the tree, indented for printing under a "games:" heading.
list_games() {
    local found=0
    for dir in game/*/; do
        [ -f "$dir/CMakeLists.txt" ] || continue
        echo "  $(basename "$dir")"
        found=1
    done
    [ "$found" = 1 ] || echo "  (none -- game/<name>/CMakeLists.txt is what makes one)"
}

# A game is a directory under game/ with a CMakeLists.txt in it. That file, and not the
# directory, is what makes one -- the same condition list_games() prints against.
is_game() { [ -n "${1:-}" ] && [ -f "game/$1/CMakeLists.txt" ]; }

# The shared front half of `<script> <game> [...]`: --help, --list, no name at all, and a
# name that is not a game. Exits on every one of those; returns 0 only when $2 names a
# real game, so the caller can `shift` and carry on.
#
# Takes the usage line to print when nothing was named, because that string is the only
# part the two callers disagree about. Call it directly and never in a command
# substitution: the exits below have to end the script, and inside `$(...)` they would
# end a subshell and let the caller continue as though the game had been fine.
check_game() {
    local usage_line="$1" game="${2:-}"
    case "$game" in
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
    "")
        echo "error: no game named. Usage: $usage_line" >&2
        echo "games:" >&2
        list_games >&2
        exit 1
        ;;
    esac

    if ! is_game "$game"; then
        echo "error: game/$game/CMakeLists.txt does not exist" >&2
        echo "games:" >&2
        list_games >&2
        exit 1
    fi
}

# Reject an argument that is neither a configuration nor anything else the caller
# recognised. The point is that it *is* rejected: `./test.sh releas` used to build debug
# and pass `releas` on to the binary, and `./build_release.sh` explains the general case
# better than this comment can -- "a build that quietly dropped the flag would hand back
# something that looks like what was asked for". A run that silently ran the wrong
# configuration is indistinguishable from one that ran the right one.
die_unknown_config() {
    echo "error: unknown argument '$1' (want: debug|release|asan|tsan)" >&2
    echo "       Everything meant for the binary goes after '--'." >&2
    exit 1
}

# Serialise builds that share a build directory, on fd 9, for the life of the calling
# shell.
#
# Ninja takes no lock of its own, and two sessions in one checkout is the normal state of
# this project, so two `cmake --build build/release` runs write the same object files and
# link the same executable at the same time. The half of that which is not obvious is what
# it does to a *finished* build: the linker unlinks its output before it writes it --
# unlink(2) then open(O_TRUNC) -- so while one session is linking `demo`, the file does
# not exist for anybody. That is how a golden case failed with "still does not exist after
# building" one statement after its own build had returned zero, and why re-running it
# passed.
#
# SUBSTRATE_BUILD_LOCK carries the held directory to child processes so the lock is not
# re-entered: run.sh holds it across build_game.sh *and* the check for the binary, which
# is the whole window, and the build.sh underneath must not queue behind its own caller.
#
# A holder that execs must close fd 9 first -- see run.sh. An inherited lock held for the
# length of a 120-second capture would block every other build in the tree for it.
build_lock() {
    local dir="$1"
    if [ "${SUBSTRATE_BUILD_LOCK:-}" = "$dir" ]; then return 0; fi
    if ! command -v flock >/dev/null 2>&1; then return 0; fi
    mkdir -p "$dir"
    exec 9>"$dir/.build.lock"
    if ! flock -n 9; then
        echo "==> waiting for another build in $dir/"
        if ! flock -w 900 9; then
            echo "error: gave up waiting for the build lock on $dir/ after 15 minutes." >&2
            exit 1
        fi
    fi
    export SUBSTRATE_BUILD_LOCK="$dir"
}

# Export the sanitizer runtime options a configuration needs, and set LAUNCH to the
# wrapper its binary has to be started through -- empty for everything except TSan.
#
# Extracted at two occurrences rather than three, against this project's usual rule,
# because the two had already drifted apart in shape: run.sh built a LAUNCH array and
# test.sh exec'd setarch inline from inside its case, so the thing they shared was the
# reasoning and not the code. That reasoning is the part worth keeping in one place --
# without setarch -R, ThreadSanitizer aborts with "unexpected memory mapping" before
# main(), and that failure looks exactly like a clean run to anyone grepping for race
# warnings.
sanitizer_env() {
    LAUNCH=()
    case "${1:-}" in
    asan)
        # halt_on_error=0 so a run surfaces every finding, not just the first.
        export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=0:abort_on_error=0}"
        export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=0}"
        ;;
    tsan)
        export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=0:second_deadlock_stack=1}"
        if command -v setarch >/dev/null 2>&1; then
            LAUNCH=(setarch -R)
        else
            echo "warning: setarch not found; TSan will likely abort before main()" >&2
        fi
        ;;
    esac
}
