#!/usr/bin/env bash
#
# Fetches the Khronos Sponza sample asset into engine/assets/.
#
# Sponza is (c) Crytek and distributed under the CryEngine Limited License
# Agreement. It is NOT compatible with this repository's Apache-2.0 license and
# must never be committed. Both asset trees are gitignored for exactly this reason.
#
# It lands in the engine's tree rather than the demo's because the golden suite is what
# depends on it: eight of eleven cases render Sponza, and they are the engine's regression
# check rather than the demo's. The demo reaches it through a relative URI inside a
# generated scene, which is a game using an engine asset -- allowed, and the reason
# make_composite_scene.py computes those URIs rather than writing them.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSET_DIR="$REPO_ROOT/engine/assets"
STAGING="$ASSET_DIR/.gltf-sample-assets"
TARGET="$ASSET_DIR/Sponza"

UPSTREAM="https://github.com/KhronosGroup/glTF-Sample-Assets"
MODEL_PATH="Models/Sponza"

# ---------------------------------------------------------------- generated scenes
# The test scenes are committed. Only the two *grafted* ones are generated, because they
# are the two that cannot be committed: `character.gltf` stages the Mixamo rig and
# `reflect.gltf` puts a mirror in Sponza, and each is a deep copy of an input under a
# license this repository cannot carry. Generating them belongs here, in the script whose
# job is "make the assets ready", rather than being a step a fresh clone discovers by a
# scene failing to load.
#
# The generator is idempotent and skips whatever source it cannot find, so this is safe to
# re-run and safe on a checkout that has only some of the inputs.
generate_scenes() {
    if ! command -v python3 >/dev/null 2>&1; then
        echo "warning: python3 not found; skipping scene generation" >&2
        echo "         run scripts/make_composite_scene.py by hand" >&2
        return
    fi
    echo "Generating grafted scenes ..."
    python3 "$(dirname "$0")/make_composite_scene.py"
}

# ---------------------------------------------------------------- fire crackle
# Cut a looping fire bed out of the recording beside it, for the braziers G9 builds.
#
# This transcodes rather than invents, and the engine is why it has to: miniaudio decodes
# WAV, FLAC and MP3, and the recording is AAC in an MP4 container, which is presumably why
# it had sat unread since the day it arrived. An earlier version synthesised a bed instead
# and measured 10,451 zero crossings a second against the recording's 1,065 -- broadband
# hiss wearing an envelope.
#
# Everything about the cut is stated rather than defaulted. Eight seconds from ten seconds
# in, because the opening is quieter than the body; mono, because four braziers place it in
# space and a stereo image would fight the panning; `volume` rather than `loudnorm`, because
# single-pass loudnorm on a source this quiet (mean -41 dB) returns silence.
#
# A missing ffmpeg or recording deletes any stale output rather than leaving it: the demo
# tests `found()` before building the source, so the braziers simply burn silently. A demo
# missing a sound is right; one that plays static is broken.
transcode_fire_crackle() {
    local dir="$REPO_ROOT/game/demo/assets/audio"
    local source="$dir/fire_crackle.m4a"
    local dest="$dir/fire_crackle.wav"

    if ! command -v ffmpeg >/dev/null 2>&1 || [ ! -f "$source" ]; then
        rm -f "$dest"
        echo "skipped $dest (needs ffmpeg and fire_crackle.m4a)"
        return
    fi

    ffmpeg -hide_banner -loglevel error -y \
        -ss 10 -t 8 -i "$source" \
        -ac 1 -ar 48000 \
        -af "volume=8dB,afade=t=in:st=0:d=0.25,afade=t=out:st=7.75:d=0.25" \
        -c:a pcm_s16le -fflags +bitexact -flags:a +bitexact "$dest"
    echo "wrote $dest"
}

if [ -f "$TARGET/glTF/Sponza.gltf" ]; then
    echo "Sponza already present at $TARGET"
    generate_scenes
    transcode_fire_crackle
    exit 0
fi

mkdir -p "$ASSET_DIR"

# Blobless + sparse: fetch only Models/Sponza (~41 MB) instead of the whole
# multi-gigabyte sample-asset repository.
echo "Fetching Sponza from $UPSTREAM ..."
rm -rf "$STAGING"
git clone --depth 1 --filter=blob:none --sparse "$UPSTREAM" "$STAGING"
git -C "$STAGING" sparse-checkout set "$MODEL_PATH"

if [ ! -f "$STAGING/$MODEL_PATH/glTF/Sponza.gltf" ]; then
    echo "error: expected $MODEL_PATH/glTF/Sponza.gltf in the upstream clone" >&2
    exit 1
fi

mv "$STAGING/$MODEL_PATH" "$TARGET"
rm -rf "$STAGING"

echo "Sponza ready:"
echo "  $TARGET/glTF/Sponza.gltf"
echo "  $(find "$TARGET" -type f | wc -l) files, $(du -sh "$TARGET" | cut -f1)"

generate_scenes
transcode_fire_crackle
