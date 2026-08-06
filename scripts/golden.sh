#!/usr/bin/env sh
#
# Golden-image regression.
#
#   scripts/golden.sh [snap|check] [config]
#
# A stub over `substrate golden`. Exit 1 is an image difference, 2 is a harness failure.
#
exec "$(dirname -- "$0")/substrate.sh" golden "$@"
