#!/usr/bin/env sh
#
# Present a known PNG and compare the swapchain bit-exactly against it.
#
#   scripts/readback.sh [config]
#
# A stub over `substrate readback`.
#
exec "$(dirname -- "$0")/substrate.sh" readback "$@"
