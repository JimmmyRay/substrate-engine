#!/usr/bin/env sh
#
# One command for a fresh clone: submodules, the commit hook, and the sample assets.
#
#   scripts/setup.sh              submodules, a dependency check, and the sample assets
#   scripts/setup.sh --no-assets  skip the download
#
# It deliberately does not build. Which configuration, and whether a game comes with it, are
# the first real decisions, and a setup script that guessed them would be the last place
# anyone looked when the wrong thing got built.
#
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

ASSETS=1
for arg in "$@"; do
    case "$arg" in
    --no-assets) ASSETS=0 ;;
    -h | --help | help)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "error: unknown argument '$arg' (want: --no-assets)" >&2
        exit 1
        ;;
    esac
done

echo "==> submodules"
git submodule update --init --recursive

missing=0
require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "  missing: $1 -- $2" >&2
        missing=1
    fi
}
echo "==> dependencies"
require cmake "the build system"
require ninja "the generator every build directory uses"
require glslangValidator "compiles engine/shaders/ to SPIR-V"
require git "submodules and the asset fetch"
require python3 "the scene generators"
[ "$missing" = 0 ] || echo "  install those, then re-run" >&2

echo "==> commit hook"
scripts/substrate.sh install-hooks

if [ "$ASSETS" = 1 ]; then
    echo "==> assets"
    scripts/substrate.sh fetch-assets
fi

cat <<'NEXT'

next:
  scripts/build.sh                 engine and unit suite -- no runnable binary
  scripts/build_game.sh demo       the demo, and a program to run
  scripts/run.sh                   what the build directory holds
  scripts/test.sh                  the unit suite
  scripts/new_game.sh mygame       start your own
NEXT
