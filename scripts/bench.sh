#!/usr/bin/env sh
#
# Per-pass GPU and CPU cost, read from the trace.
#
#   scripts/bench.sh [args]
#
# A stub over `substrate bench`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/bench.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" bench "$@"
