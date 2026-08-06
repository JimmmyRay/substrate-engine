#!/usr/bin/env sh
#
# Step a scene with no Vulkan device and no window.
#
#   substrate sim [args]
#
# A stub over `substrate sim`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/sim.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" sim "$@"
