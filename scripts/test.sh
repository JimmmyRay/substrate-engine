#!/usr/bin/env sh
#
# Build and run the unit suite.
#
#   scripts/test.sh [args]
#
# A stub over `substrate test`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/test.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" test "$@"
