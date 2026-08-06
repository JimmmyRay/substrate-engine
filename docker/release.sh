#!/usr/bin/env bash
#
# Build a release inside a container. Called by `scripts/build_release.sh <name> ... --docker`;
# not meant to be run directly, which is why it takes positional arguments in a fixed
# order rather than parsing flags.
#
#   docker/release.sh <game> <linux|windows|all> <version> [--strict]
#
# The container is what makes a Windows build possible at all -- the cross-compiler only
# exists in the image -- and what makes a Linux build reproducible, by pinning the glibc
# the AppImage is built against rather than inheriting whatever the developer has.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

GAME="${1:?game name}"
TARGETS="${2:?linux|windows|all}"
VERSION="${3:?version}"
STRICT="${4:-}"

build_image() {
    local tag="$1" file="$2"
    echo "==> building image $tag"
    docker build -q -f "$file" -t "$tag" . >/dev/null
}

# The repository goes in read-write because the build writes to build/ and releases/, and
# --user keeps everything it writes owned by the caller rather than by root. A container
# that leaves root-owned build output behind is one that breaks the next native build with
# a permission error nobody connects to having run this.
#
# HOME has to be a directory the caller owns: --user makes the container's uid one with no
# passwd entry and no home, so any tool that wants to write a dotfile lands on a path it
# cannot create. Under build/ so it is already gitignored and `scripts/build.sh clean` takes it
# with everything else.
#
# Nothing may create a symlink out of the tree here. This directory sits inside the
# workspace, so anything under it that resolves outside -- a wine prefix's `dosdevices/z:`
# pointing at `/` was the case that happened -- turns every editor and indexer that walks
# the repository into one walking the whole filesystem, by way of /proc/<pid>/cwd, until it
# runs out of file descriptors or memory.
CONTAINER_HOME="$REPO_ROOT/build/.container-home"

run_in() {
    local image="$1"
    shift
    mkdir -p "$CONTAINER_HOME"
    docker run --rm \
        -v "$REPO_ROOT:/src" \
        -w /src \
        --user "$(id -u):$(id -g)" \
        -e HOME=/src/build/.container-home \
        -e SUBSTRATE_IN_CONTAINER=1 \
        "$image" bash -c "$*"
}

case "$TARGETS" in
linux | all)
    build_image substrate-linux docker/linux.Dockerfile
    echo "==> linux release in a container"
    run_in substrate-linux "scripts/build_release.sh '$GAME' linux $STRICT"
    ;;
esac

case "$TARGETS" in
windows | all)
    build_image substrate-windows docker/windows.Dockerfile
    echo "==> windows release in a container"
    run_in substrate-windows "scripts/release_windows.sh '$GAME' '$VERSION' $STRICT"
    ;;
esac

echo "==> artifacts:"
ls -la releases/
