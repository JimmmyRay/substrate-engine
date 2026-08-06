#!/usr/bin/env sh
#
# Fetch Sponza and generate the derived assets.
#
#   substrate fetch-assets [args]
#
# A stub over `substrate fetch-assets`. The logic is in tools/cli/; this exists because something
# has to build that before it can run. scripts/fetch_assets.cmd is the same thing on Windows.
#
exec "$(dirname -- "$0")/substrate.sh" fetch-assets "$@"
