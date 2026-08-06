#!/usr/bin/env sh
#
# Configure and build the engine and the unit suite.
#
#   scripts/build.sh [args]
#
# A stub over `substrate build`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/build.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" build "$@"
