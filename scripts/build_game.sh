#!/usr/bin/env sh
#
# Configure and build the engine, plus one game.
#
#   scripts/build_game.sh [args]
#
# A stub over `substrate build-game`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/build_game.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" build-game "$@"
