#!/usr/bin/env bash
#
# Package a game into a distributable artifact.
#
#   ./build_release.sh demo                  Linux AppImage into releases/
#   ./build_release.sh demo linux
#   ./build_release.sh demo --docker         build in a container instead of on this machine
#   ./build_release.sh demo --strict         fail if the package holds anything unshippable
#   ./build_release.sh --list                names every game/<name>/ in the tree
#
# `./build_game.sh <name>` produces a program that runs out of the build tree it was built
# in: the shader and asset roots are absolute paths baked in at compile time, which is what
# lets it be launched from any working directory. This script produces the other thing -- a
# directory that carries everything it needs and can be moved to a machine that has never
# seen this source tree.
#
# The difference is one CMake flag. `SUBSTRATE_PORTABLE=ON` swaps all four runtime roots
# for relative names, which `executableDir()` then anchors to the binary, and turns shader
# hot reload off because its paths are this machine's shader sources and its glslang.
#
# What goes in the package is not a list kept here. `scripts/manifest.py` works it out by
# following what the engine would actually load -- the config, then every glTF it reaches,
# then their buffers, images, texture caches and audio -- and fails naming anything it
# cannot find. That check runs *before* the build, so a missing asset costs seconds rather
# than a full compile.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

GAME="${1:-}"
check_game "./build_release.sh <name> [linux|windows|all]" "$GAME"
shift

TARGETS="linux"
USE_DOCKER=0
STRICT=""

while [ $# -gt 0 ]; do
    case "$1" in
    linux | windows | all)
        TARGETS="$1"
        ;;
    --docker)
        USE_DOCKER=1
        ;;
    --strict)
        STRICT="--strict"
        ;;
    # The sanitizers are development configurations and there is no such thing as a
    # sanitized release. Rejected by name rather than ignored, because a build that
    # quietly dropped the flag would hand back something that looks like what was asked
    # for -- the failure mode CLAUDE.md's note about `run.sh tsan` is about.
    asan | tsan | debug)
        echo "error: '$1' is a development configuration; a release is always Release." >&2
        exit 1
        ;;
    *)
        echo "error: unknown argument '$1'" >&2
        usage >&2
        exit 1
        ;;
    esac
    shift
done

if [ "$TARGETS" != "linux" ] && [ "$USE_DOCKER" = 0 ]; then
    echo "error: '$TARGETS' needs the cross-compiler, which only exists in the container." >&2
    echo "       Add --docker." >&2
    exit 1
fi

VERSION="$(awk -F'[ )]+' '/^project\(Substrate VERSION/ { print $3; exit }' CMakeLists.txt)"
if [ -z "$VERSION" ]; then
    echo "error: could not read the version out of CMakeLists.txt" >&2
    exit 1
fi

if [ "$USE_DOCKER" = 1 ]; then
    echo "==> building in a container"
    exec docker/release.sh "$GAME" "$TARGETS" "$VERSION" $STRICT
fi

# ------------------------------------------------------------------ manifest, first

echo "==> resolving what $GAME needs"
MANIFEST="$(mktemp)"
trap 'rm -f "$MANIFEST"' EXIT

# The texture cache is built before the manifest is taken, not after, and the ordering is
# the whole of C14's package half. `manifest.py` treats a missing `.ktx2` as normal --
# correct in a source tree, where the loader simply decodes the source image -- so a
# release taken without this step ships the decode path to someone who has no `ktx` on
# their PATH and no way to rebuild the cache. Baking first and then demanding the result
# with --require-cache turns "quietly slower for the player" into "loud here".
#
# Which scenes to bake is not a list kept here either: the same resolver decides it, run
# once without the requirement to find out what would be staged.
echo "==> baking the texture cache"
SCENES="$(./scripts/manifest.py "$GAME" | awk '{ print $1 }' | grep -Ei '\.(gltf|glb)$' || true)"
if [ -z "$SCENES" ]; then
    echo "error: no scene resolved for $GAME, so there is nothing to bake or package." >&2
    exit 1
fi
for scene in $SCENES; do
    if ! ./scripts/ktx2.py "$scene" --quiet; then
        echo "error: the texture cache is incomplete; nothing was built." >&2
        exit 1
    fi
done

# C15, and it runs *after* the textures for a reason that is not ordering pedantry: a
# scene whose embedded images have no `.ktx2` is refused a sidecar, because the sidecar
# could not be read back without the document it exists to avoid opening. Baking the
# textures first is what makes that case not arise.
#
# One invocation, every scene, and no renderer (D9). This used to be one launch of the
# game per scene -- `--headless --locked --frames 3 --bake-scene` -- which stood up a
# Vulkan device, a swapchain and every texture upload to produce a CPU-side artifact that
# touches none of them, so the one step of a package with no use for a GPU was the step
# that could not run without one. `substrate-bake` links neither Vulkan nor a window.
#
# Still C++ and still the engine's own structs, which is C13's rule: the writer shares
# `Vertex` and `Primitive` with the reader, so a layout change is a compile error rather
# than a corrupt scene, and a Python writer would be a second encoding of the vertex
# layout with no compiler to make the two agree.
echo "==> baking the scene cache"
# shellcheck disable=SC2086
if ! ./scripts/bake.sh release $SCENES; then
    echo "warning: not every scene could be baked; the package will parse those at launch" >&2
fi

# Deliberately not inside `$( )`: the whole point of running this first is that its
# diagnostics reach the terminal, and a command substitution would swallow them.
if ! ./scripts/manifest.py "$GAME" $STRICT --require-cache >"$MANIFEST"; then
    echo "error: the package is incomplete; nothing was built." >&2
    exit 1
fi
echo "==> $(wc -l <"$MANIFEST") file(s) to package"

# ------------------------------------------------------------------ build

# A separate build directory inside the container, because a CMake cache records the
# absolute path it was generated for. The repository is /src in the container and
# /home/... outside it, so sharing one directory makes the second configure fail with
# "The current CMakeCache.txt directory ... is different than the directory ...", which
# reads like a corrupted cache rather than like two views of one tree.
BUILD_DIR="build/release-portable${SUBSTRATE_IN_CONTAINER:+-docker}"
echo "==> configuring $GAME in $BUILD_DIR/"
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSUBSTRATE_GAME="$GAME" \
    -DSUBSTRATE_PORTABLE=ON

echo "==> building"
cmake --build "$BUILD_DIR"

# ------------------------------------------------------------------ stage

STAGE="$BUILD_DIR/stage/$GAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/shaders/game" "$STAGE/assets"

cp "$BUILD_DIR/$GAME" "$STAGE/$GAME"
cp "$BUILD_DIR"/shaders/*.spv "$STAGE/shaders/" 2>/dev/null || true
cp "$BUILD_DIR"/shaders/game/*.spv "$STAGE/shaders/game/" 2>/dev/null || true
cp substrate.json "$STAGE/substrate.json"

while IFS= read -r line; do
    src="${line%% -> *}"
    dest="${line##* -> }"
    mkdir -p "$STAGE/$(dirname "$dest")"
    cp "$src" "$STAGE/$dest"
done <"$MANIFEST"

# The engine resolves relative log and trace paths against the working directory, not the
# executable, so that scripts/golden.sh and scripts/baseline.py keep finding debug_frames/
# at the repo root. A packaged game is launched with a working directory nobody chose, so
# the launcher below is what makes that directory writable and predictable.
mkdir -p "$STAGE/debug_frames"

echo "==> staged $(find "$STAGE" -type f | wc -l) file(s), $(du -sh "$STAGE" | cut -f1)"

# ------------------------------------------------------------------ package

mkdir -p releases

case "$TARGETS" in
linux | all)
    APPDIR="$BUILD_DIR/AppDir"
    rm -rf "$APPDIR"
    mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

    cp -r "$STAGE"/. "$APPDIR/usr/bin/"

    # AppRun rather than a symlink to the binary, because the working directory is the
    # whole problem. An AppImage is mounted read-only, so a game that wrote its log beside
    # itself would fail; this puts the process somewhere writable and standard first.
    cat >"$APPDIR/AppRun" <<APPRUN
#!/bin/sh
HERE="\$(dirname "\$(readlink -f "\$0")")"
DATA="\${XDG_DATA_HOME:-\$HOME/.local/share}/$GAME"
mkdir -p "\$DATA"
cd "\$DATA" || exit 1
exec "\$HERE/usr/bin/$GAME" "\$@"
APPRUN
    chmod +x "$APPDIR/AppRun"

    cat >"$APPDIR/$GAME.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$GAME
Exec=$GAME
Icon=$GAME
Categories=Game;
Terminal=false
DESKTOP
    cp "$APPDIR/$GAME.desktop" "$APPDIR/usr/share/applications/"

    # appimagetool refuses to build without an icon, and a generated one beats a missing
    # dependency on an artist. 256x256 PNG, written by hand because ImageMagick is not a
    # thing this repository otherwise needs.
    python3 scripts/make_icon.py "$APPDIR/$GAME.png" "$GAME"
    cp "$APPDIR/$GAME.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/"

    OUT="releases/$GAME-$VERSION-x86_64.AppImage"
    rm -f "$OUT"
    echo "==> building $OUT"
    ARCH=x86_64 appimagetool --no-appstream "$APPDIR" "$OUT" >/dev/null 2>&1 ||
        ARCH=x86_64 appimagetool --no-appstream "$APPDIR" "$OUT"
    chmod +x "$OUT"
    echo "==> done: $OUT ($(du -h "$OUT" | cut -f1))"
    ;;
esac

if [ -n "$STRICT" ]; then
    echo "==> --strict: every packaged file is redistributable"
fi
