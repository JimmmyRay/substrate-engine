#!/usr/bin/env sh
#
# Scaffold game/<name>/ from the template.
#
#   scripts/new_game.sh [args]
#
# A stub over `substrate new-game`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/new_game.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" new-game "$@"
