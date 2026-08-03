#!/usr/bin/env bash
#
# Everything a fresh clone needs before the first build.
#
#   ./setup.sh            submodules, a dependency check, and the sample assets
#   ./setup.sh --no-assets   skip the download
#
# A thin wrapper over three commands the README already listed, and it exists for one
# reason: the first instruction a new developer reads should be one line rather than four.
# Every one of the three is safe to run again, so this is too.
#
# What it deliberately does not do is build. Which configuration, and whether a game comes
# with it, are the first real decisions -- and a setup script that guessed them would be
# the last place anyone looked when the wrong thing got built.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

# shellcheck source=scripts/common.sh
source "$REPO_ROOT/scripts/common.sh"

ASSETS=1
case "${1:-}" in
-h | --help | help)
    usage
    exit 0
    ;;
--no-assets)
    ASSETS=0
    ;;
"") ;;
*)
    echo "error: unknown argument '$1' (want: --no-assets)" >&2
    exit 1
    ;;
esac

echo "==> submodules"
git submodule update --init --recursive

# .git/hooks is not cloned, so a hook nobody installs is a check nobody runs.
scripts/install_hooks.sh

# Named, not guessed at. A missing glslangValidator surfaces as a shader that fails to
# compile several minutes into a build, and a missing Vulkan driver as a device that
# cannot be created -- both a long way from the sentence that would have explained them.
echo "==> dependencies"
missing=0
require() {
    if command -v "$1" >/dev/null 2>&1; then
        echo "  ok       $1"
    else
        echo "  MISSING  $1 -- $2"
        missing=1
    fi
}
require cmake "cmake >= 3.20"
require ninja "ninja-build"
require glslangValidator "glslang-tools"
require git "git"
require python3 "python3, for scripts/baseline.py and the scene generators"

if [ "$missing" = 1 ]; then
    echo
    echo "Install what is missing and run this again."
    echo "docs/guides/building.md carries the exact apt line and a verified-versions table."
    exit 1
fi

if [ "$ASSETS" = 1 ]; then
    echo "==> assets"
    scripts/fetch_assets.sh
else
    echo "==> assets skipped"
fi

echo
echo "ready. next:"
echo "  ./build.sh                 engine and unit suite -- no runnable binary"
echo "  ./build_game.sh demo       the demo, and a program to run"
echo "  ./run.sh                   what the build directory holds"
echo
echo "  ./new_game.sh mygame       start your own"
