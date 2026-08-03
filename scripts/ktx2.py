#!/usr/bin/env python3
"""
Build the compressed texture cache for a glTF scene (4.6a).

    ./scripts/ktx2.py engine/assets/Sponza/glTF/Sponza.gltf
    ./scripts/ktx2.py --clean engine/assets/Sponza/glTF/Sponza.gltf

Writes `<image>.png.ktx2` beside every image the scene references, holding a BC7 mip
chain. The engine picks those up automatically: `GltfScene::load` looks for the sibling
and, where it finds one, never decodes the PNG at all. Nothing rewrites the glTF, and
deleting the cache restores the original behaviour exactly -- which is the property that
makes this a cache rather than a second asset format to keep in step.

## Why BC7 and why sRGB-ness is decided here

Block compression is not a property of an image, it is a property of an image *in a
slot*. A base-colour map is sRGB and a normal map is not, and the decode happens in
fixed-function hardware selected by the VkFormat -- so the cache file has to carry the
right one. This script therefore reads the glTF's materials to find out which images are
colour, exactly as the loader does, and emits BC7_SRGB or BC7_UNORM accordingly. Getting
it wrong is invisible in a thumbnail and obvious in a lit frame.

BC7 rather than BC1/BC3: it is 8 bits per texel against RGBA8's 32, the same ratio BC3
gives, and it does not have BC3's 4-bit-endpoint colour banding or its separate alpha
block. Sponza's foliage is alpha-masked, so alpha quality is not optional.

## Why the toolchain shells out

The KTX-Software `ktx` CLI does the encoding. Vendoring a BC7 encoder would be several
thousand lines of rate-distortion search to replace a tool that already exists and is
only ever run when assets change -- and the engine does not depend on it at all. The
route is `ktx create --encode uastc` then `ktx transcode --target bc7`, because `ktx
create` will not encode BC7 directly; the intermediate UASTC file is never written to
disk.
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


def find_ktx():
    exe = shutil.which("ktx")
    if exe is None:
        sys.exit("ktx not found on PATH. Install KTX-Software "
                 "(https://github.com/KhronosGroup/KTX-Software) to build the texture cache.")
    return exe


def srgb_images(doc):
    """
    Indices of images used in a colour slot.

    The same rule GltfScene::load applies: base colour and emissive are authored in sRGB,
    everything else (normal, metallic-roughness, occlusion) is data and must not be
    gamma-decoded on read.
    """
    textures = doc.get("textures", [])
    out = set()

    def mark(info):
        if not info:
            return
        tex = textures[info["index"]]
        if "source" in tex:
            out.add(tex["source"])

    for mat in doc.get("materials", []):
        mark(mat.get("pbrMetallicRoughness", {}).get("baseColorTexture"))
        mark(mat.get("emissiveTexture"))
    return out


def convert(ktx, source, dest, srgb, quiet):
    fmt = "R8G8B8A8_SRGB" if srgb else "R8G8B8A8_UNORM"
    with tempfile.TemporaryDirectory() as tmp:
        intermediate = pathlib.Path(tmp) / "u.ktx2"
        create = subprocess.run(
            [ktx, "create", "--format", fmt, "--encode", "uastc", "--generate-mipmap",
             "--assign-tf", "srgb" if srgb else "linear", str(source), str(intermediate)],
            capture_output=True, text=True)
        if create.returncode != 0:
            print(f"  FAILED {source.name}: {create.stderr.strip().splitlines()[-1:] or create.stdout.strip()}")
            return False

        transcode = subprocess.run([ktx, "transcode", "--target", "bc7", str(intermediate), str(dest)],
                                   capture_output=True, text=True)
        if transcode.returncode != 0:
            print(f"  FAILED {source.name}: {transcode.stderr.strip()}")
            return False

    if not quiet:
        before = source.stat().st_size
        after = dest.stat().st_size
        print(f"  {source.name}: {before // 1024} KiB -> {after // 1024} KiB "
              f"({'sRGB' if srgb else 'linear'})")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scene", type=pathlib.Path)
    ap.add_argument("--clean", action="store_true", help="delete the cache instead of building it")
    ap.add_argument("--force", action="store_true", help="rebuild entries that already exist")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not args.scene.is_file():
        sys.exit(f"{args.scene}: not found")

    doc = json.loads(args.scene.read_text())
    base = args.scene.parent
    colour = srgb_images(doc)

    images = doc.get("images", [])
    if not images:
        print("scene declares no images")
        return

    ktx = None if args.clean else find_ktx()

    built = skipped = failed = 0
    total_before = total_after = 0

    for i, image in enumerate(images):
        uri = image.get("uri")
        if uri is None:
            # Embedded in a buffer view. The loader names its cache after the scene, but
            # there is no file here to hand the encoder -- extracting it would mean
            # decoding the payload, which is the work the cache exists to avoid.
            if not args.quiet:
                print(f"  image {i}: embedded, skipped (no file to encode)")
            skipped += 1
            continue

        source = base / uri
        dest = base / (uri + ".ktx2")

        if args.clean:
            if dest.exists():
                dest.unlink()
                built += 1
            continue

        if not source.is_file():
            print(f"  image {i}: {source} missing")
            failed += 1
            continue

        # Rebuilt when the source is newer, which is what makes this safe to re-run
        # after editing one texture.
        if dest.exists() and not args.force and dest.stat().st_mtime >= source.stat().st_mtime:
            skipped += 1
            continue

        if convert(ktx, source, dest, i in colour, args.quiet):
            built += 1
            total_before += source.stat().st_size
            total_after += dest.stat().st_size
        else:
            failed += 1

    if args.clean:
        print(f"removed {built} cache entries")
        return

    print(f"built {built}, up to date {skipped}, failed {failed}")
    if total_before:
        print(f"source {total_before // (1024 * 1024)} MiB -> cache {total_after // (1024 * 1024)} MiB")


if __name__ == "__main__":
    main()
