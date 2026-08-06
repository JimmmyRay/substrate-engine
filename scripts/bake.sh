#!/usr/bin/env sh
#
# Write a scene's .scene sidecar; no device, no window.
#
#   substrate bake [args]
#
# A stub over `substrate bake`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/bake.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" bake "$@"
