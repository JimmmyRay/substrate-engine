#!/usr/bin/env sh
#
# Build a game and launch it.
#
#   scripts/run.sh [args]
#
# A stub over `substrate run`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/run.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" run "$@"
