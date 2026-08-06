#!/usr/bin/env bash
#
# Cross-compile, stage and package a game for Windows. Runs *inside* the container built
# from docker/windows.Dockerfile; `scripts/build_release.sh <name> windows --docker` is the entry
# point, and running this on a host without the mingw toolchain will simply fail to
# configure.
#
#   scripts/release_windows.sh <game> <version> [--strict]
#
# Structured to mirror the Linux half of build_release.sh step for step -- manifest first,
# then build, then stage, then package -- because the two producing different layouts from
# the same manifest is the bug class that already cost one broken AppImage.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

GAME="${1:?game name}"
VERSION="${2:?version}"
STRICT="${3:-}"

BUILD_DIR="build/win-release"
STAGE="$BUILD_DIR/stage/$GAME"

# ------------------------------------------------------------------ manifest, first

echo "==> resolving what $GAME needs"
MANIFEST="$(mktemp)"
trap 'rm -f "$MANIFEST"' EXIT
if ! scripts/substrate.sh manifest "$GAME" $STRICT >"$MANIFEST"; then
    echo "error: the package is incomplete; nothing was built." >&2
    exit 1
fi
echo "==> $(wc -l <"$MANIFEST") file(s) to package"

# ------------------------------------------------------------------ build

echo "==> cross-compiling $GAME for windows"
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchains/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSUBSTRATE_GAME="$GAME" \
    -DSUBSTRATE_PORTABLE=ON
cmake --build "$BUILD_DIR"

# ------------------------------------------------------------------ verify

# Runs before packaging, because an installer around a broken .exe is a slower way to find
# out. It is a static check: nothing here executes the .exe, so what it proves is that the
# binary is linked correctly, not that it runs. See docs/architecture/limitations.md.
echo "==> checking the import table"
IMPORTS="$(x86_64-w64-mingw32-objdump -p "$BUILD_DIR/$GAME.exe" | awk '/DLL Name:/ {print tolower($3)}' | sort -u)"
BAD="$(echo "$IMPORTS" | grep -E 'libstdc\+\+|libgcc|libwinpthread|vulkan-1' || true)"
if [ -n "$BAD" ]; then
    echo "error: $GAME.exe imports DLLs it must not:" >&2
    echo "$BAD" | sed 's/^/  /' >&2
    echo "       libstdc++/libgcc/libwinpthread mean -static stopped applying, and the exe" >&2
    echo "       will not start on a machine without the MinGW runtime beside it." >&2
    echo "       vulkan-1 means volk is no longer loading the loader at runtime, and the exe" >&2
    echo "       will die in the Windows loader on a machine with no Vulkan rather than" >&2
    echo "       printing its own error." >&2
    exit 1
fi
echo "    imports: $(echo "$IMPORTS" | tr '\n' ' ')"

# ------------------------------------------------------------------ stage

rm -rf "$STAGE"
mkdir -p "$STAGE/shaders/game" "$STAGE/debug_frames"

cp "$BUILD_DIR/$GAME.exe" "$STAGE/"
cp "$BUILD_DIR"/shaders/*.spv "$STAGE/shaders/" 2>/dev/null || true
cp "$BUILD_DIR"/shaders/game/*.spv "$STAGE/shaders/game/" 2>/dev/null || true
cp substrate.json "$STAGE/substrate.json"

while IFS= read -r line; do
    src="${line%% -> *}"
    dest="${line##* -> }"
    mkdir -p "$STAGE/$(dirname "$dest")"
    cp "$src" "$STAGE/$dest"
done <"$MANIFEST"

echo "==> staged $(find "$STAGE" -type f | wc -l) file(s), $(du -sh "$STAGE" | cut -f1)"

# ------------------------------------------------------------------ package

mkdir -p releases
OUT="releases/$GAME-$VERSION-windows-x64-setup.exe"
rm -f "$OUT"

echo "==> building $OUT"
makensis -NOCD \
    "-DGAME=$GAME" \
    "-DVERSION=$VERSION" \
    "-DSTAGE=$REPO_ROOT/$STAGE" \
    "-DOUTFILE=$REPO_ROOT/$OUT" \
    scripts/installer.nsi >/dev/null

echo "==> done: $OUT ($(du -h "$OUT" | cut -f1))"

# The installer is produced but not run. Nothing in this image can execute a Windows binary,
# so "it installs, with the layout the manifest chose" is a claim that now rests on the
# staged tree above rather than on an observed install. What that leaves unchecked -- most
# sharply, that engine/assets and game/<name>/assets keep their depth, which the composite
# scenes' relative references depend on -- is recorded in docs/architecture/limitations.md.
