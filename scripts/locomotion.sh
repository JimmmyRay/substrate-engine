#!/usr/bin/env sh
#
# A stub over `substrate locomotion`. The harness is tools/cli/; this exists so the command has a
# name on both platforms.
#
exec "$(dirname -- "$0")/substrate.sh" locomotion "$@"
