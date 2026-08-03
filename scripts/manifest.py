#!/usr/bin/env python3
"""
Work out every file a packaged game needs, by following what the engine would load.

    ./scripts/manifest.py demo                 the file list, one `src -> dest` per line
    ./scripts/manifest.py demo --json          the same, for build_release.sh
    ./scripts/manifest.py demo --config x.json a different starting config
    ./scripts/manifest.py demo --strict        fail if anything in it cannot be redistributed

Exits non-zero naming every `res:/` reference that resolved to nothing, and which roots
were searched. That is the whole point: a package missing an asset should fail here, in
seconds and with the name of the thing, rather than build for ten minutes and ship a game
that opens on a black screen.

## Why this is a closure walk and not a grep

The obvious implementation -- grep the tree for `res:/` -- finds almost nothing in the
engine, because the call sites take runtime values: `Resources(...debugFont)` and
`Resources(s.file)` in Engine.cpp resolve strings that came out of the settings table and
out of the game's `GameSetup`. Everything else a scene needs is named *inside* the glTF and
resolved relative to the document by GltfScene, never through `res:/` at all.

The *game's* literals are a different matter since S1: a scene path is authored, so
`GameSetup::scene` in the game's own source is where the name lives now, and this script
requires it rather than treating it as a fallback.

So the literals are seeds. The set that matters is the transitive closure:

    source literals + substrate.json
        -> res:/ names, resolved against the two asset roots
            -> for each glTF: buffers[].uri, images[].uri, the .ktx2 sidecars beside
               them, and nodes[].extras.substrate_audio.file
                -> all document-relative, so relative to wherever the glTF resolved to

## The one coupling worth knowing about

Resolution here has to match `Resources::Resources` exactly -- game tree first, then
engine -- or this stages one file and the binary opens another. It is the same hazard the
comment above `kShaderTrees` in Renderer.cpp names for shader flags: two places encoding
one rule, and no compiler to make them agree. `tests/ResourcesTests.cpp` pins the engine
side; `tests/manifest_test.py` pins this side against the same cases.

## What it cannot see

A game that builds a `res:/` name at runtime from something other than the config is
invisible to this, and no amount of scanning fixes that. `game/<name>/package.txt` is the
escape hatch: one name per line, blank lines and `#` comments ignored.
"""

import argparse
import json
import pathlib
import re
import struct
import sys
import urllib.parse

# Matches a res:/ name in a source literal or a JSON string. The scheme is the anchor;
# Resources drops any number of leading slashes, so `res:/x` and `res://x` are one asset.
RES_RE = re.compile(r'res:/+([^"\'\s\\]+)')

# `Logger::warn(..., "res:/%.*s not found in ...")` in Resources.cpp is a format string,
# not an asset. Anything carrying a printf conversion is the engine talking about a name
# rather than naming one.
FORMAT_RE = re.compile(r"%[-+ #0-9.*]*[a-zA-Z]")

# Source trees worth scanning for literals, relative to the repo root. The game's own tree
# is added at runtime once the game is known.
ENGINE_SRC = "engine"

# Content this repository does not own and cannot redistribute. Sponza is (c) Crytek under
# the CryEngine Limited License Agreement, which is incompatible with Apache-2.0.
#
# Packaged by default and merely reported, because a local build is not distribution and
# the common case here is a developer making a runnable artifact for themselves. `--strict`
# turns every one of these into an error, and is what a build that actually goes out to
# other people should pass -- the point of tracking it at all is that the day the answer
# changes, the list already exists instead of having to be reconstructed.
RESTRICTED = ("Sponza",)


def restricted_part(name):
    """The first non-redistributable path component of `name`, or None."""
    for part in pathlib.PurePosixPath(name).parts:
        if part in RESTRICTED:
            return part
    return None


class Missing(Exception):
    """A res:/ name that neither tree held."""


def read_gltf(path):
    """The JSON of a .gltf or the JSON chunk of a .glb. None if it is neither."""
    data = path.read_bytes()
    if data[:4] == b"glTF":
        # 12-byte header, then chunks of (length, type, payload). The first chunk is JSON.
        if len(data) < 20:
            return None
        length, kind = struct.unpack_from("<II", data, 12)
        if kind != 0x4E4F534A:  # 'JSON'
            return None
        return json.loads(data[20:20 + length])
    try:
        return json.loads(data)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def gltf_references(doc, scene):
    """Every file a glTF names, resolved against the document holding it.

    Mirrors what the loader actually opens: fastgltf reads buffers and images relative to
    the document, GltfScene::ktx2CachePath looks for a sibling .ktx2, and
    parseSceneAudioSources reads substrate_audio.file the same way.
    """
    base = scene.parent
    out = []

    for kind in ("buffers", "images"):
        for i, item in enumerate(doc.get(kind, [])):
            uri = item.get("uri")
            if uri is None:
                # Embedded in a buffer view. Nothing to stage for the payload itself, but
                # the texture cache still gets a name -- ktx2CachePath falls back to
                # `<stem>.image<N>.ktx2` exactly so an embedded image can have one.
                if kind == "images":
                    out.append(base / f"{scene.stem}.image{i}.ktx2")
                continue
            if uri.startswith("data:"):
                continue
            target = base / unquote(uri)
            out.append(target)
            if kind == "images":
                out.append(base / (unquote(uri) + ".ktx2"))

    # C15's sidecar, beside the document and named for the whole of it. Absent is the
    # normal case in a source tree, exactly as a `.ktx2` is, and staged whenever it is
    # there -- `build_release.sh` bakes before taking the manifest so that it always is.
    out.append(scene.parent / (scene.name + ".scene"))

    # Audio lives in node extras, not in a glTF-standard array.
    for node in doc.get("nodes", []):
        audio = node.get("extras", {}).get("substrate_audio")
        if isinstance(audio, dict) and isinstance(audio.get("file"), str):
            out.append(base / unquote(audio["file"]))

    return out


def unquote(uri):
    """glTF URIs are percent-encoded. Spaces as %20 are the case that actually occurs."""
    return urllib.parse.unquote(uri)


def string_literals(text):
    """Every double-quoted literal in a C++ translation unit, comments excluded.

    Both halves matter and neither is optional. Scanning raw text finds the examples in
    Resources.h's own file comment -- `Resources("res:/showcase.gltf")` is prose about how
    the scheme works, and treating it as a dependency had this script demanding a scene the
    package never asks for. Scanning quotes without skipping comments finds them too,
    because the examples are quoted. Skipping comments without tracking strings would cut a
    literal containing `//` in half.

    A single pass that knows which of the three states it is in gets all of it right, and
    is shorter than any two of the approximations.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                if text[j] == "\n":  # unterminated; not our problem to diagnose
                    break
                buf.append(text[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
        elif text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                break
        elif text.startswith("/*", i):
            i = text.find("*/", i)
            if i < 0:
                break
            i += 2
        elif c == "'":
            i += 3 if not text.startswith("\\", i + 1) else 4
        else:
            i += 1
    return out


def scan_literals(roots):
    """`res:/` names appearing in string literals in source files under `roots`."""
    names = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in (".cpp", ".h", ".hpp", ".c"):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for literal in string_literals(text):
                names.update(n for n in RES_RE.findall(literal) if not FORMAT_RE.search(n))
    return names


def scan_json(value):
    """Every `res:/` name anywhere in a decoded JSON document.

    Walks the whole tree rather than reading scene.path, render.debugFont and
    audio.sources[].file by name: those are simply the fields that use the scheme today,
    and a config that grows a fourth should not need this script edited to notice.
    """
    names = set()
    if isinstance(value, str):
        names.update(RES_RE.findall(value))
    elif isinstance(value, dict):
        for v in value.values():
            names |= scan_json(v)
    elif isinstance(value, list):
        for v in value:
            names |= scan_json(v)
    return names


class Resolver:
    """`res:/` lookup, in the order Resources::Resources uses: game tree, then engine.

    Also knows where each tree lands in a package. Those destinations mirror the source
    tree's own shape rather than being flattened, because a glTF's references are relative
    to the document and the composite scenes reach across into the other tree; the two have
    to stay the same distance apart in the package as they are here. See SUBSTRATE_PORTABLE
    in CMakeLists.txt, which sets the matching runtime roots.
    """

    def __init__(self, game_root, engine_root, game_prefix="game/demo/assets", engine_prefix="engine/assets"):
        self.game_root = game_root
        self.engine_root = engine_root
        self.game_prefix = pathlib.PurePosixPath(game_prefix)
        self.engine_prefix = pathlib.PurePosixPath(engine_prefix)

    def prefix(self, tree):
        return self.game_prefix if tree == "game" else self.engine_prefix

    def resolve(self, name):
        """(absolute path, which tree). Raises Missing if neither tree holds it."""
        if self.game_root is not None:
            candidate = self.game_root / name
            if candidate.exists():
                return candidate, "game"
        candidate = self.engine_root / name
        if candidate.exists():
            return candidate, "engine"
        raise Missing(name)

    def roots_description(self):
        game = str(self.game_root) if self.game_root is not None else "<no game>"
        return f'"{game}" or "{self.engine_root}"'

    def relocate(self, path):
        """(tree, destination within it) for a file on disk, or None if it is under neither.

        A document-relative reference is not confined to the tree its document was found
        in -- the composite scenes graft props onto glTFs in the *other* tree, so they
        reach across with `../../../engine/assets/...`. The loader follows that happily;
        a package has to work out which root the file ends up under so it can be staged
        somewhere the same relative walk still lands on it.
        """
        resolved = path.resolve()
        for tree, root in (("game", self.game_root), ("engine", self.engine_root)):
            if root is None:
                continue
            try:
                rel = resolved.relative_to(root.resolve())
            except ValueError:
                continue
            return tree, pathlib.PurePosixPath(rel.as_posix())
        return None


def build(resolver, required, optional, strict=False, require_cache=False):
    """Close over every seed, returning (staged, missing, restricted, cold).

    `required` are names the packaged config asks for; one that is missing fails the build.
    `optional` are source-literal defaults the packaged config overrides, so a missing one
    is not an error -- the package never reaches it.

    `cold` collects images whose `.ktx2` sidecar was never built. Empty unless
    `require_cache`, because an absent sidecar is the normal case in a source tree and an
    error only in a release: shipping one means shipping the decode path to someone who
    cannot rebuild the cache.

    `restricted` collects package-relative destinations of content this repository cannot
    redistribute. Staged anyway unless `strict`, in which case the caller stops.

    `staged` maps an absolute source path to its package-relative destination. The two
    asset trees stay separate in the package -- assets/game and assets/engine -- so that
    "the game's copy of a name wins" means the same thing at runtime in a package as it
    does in the source tree.
    """
    staged = {}
    missing = []
    restricted = []
    cold = []
    seen = set()

    queue = []
    for name in sorted(required):
        try:
            path, tree = resolver.resolve(name)
        except Missing:
            missing.append(name)
            continue
        queue.append((path, tree, pathlib.PurePosixPath(name)))

    for name in sorted(optional):
        try:
            path, tree = resolver.resolve(name)
        except Missing:
            # A compiled-in default the packaged config overrides. Not being there is the
            # normal case on a machine that never fetched it.
            continue
        queue.append((path, tree, pathlib.PurePosixPath(name)))

    while queue:
        path, tree, dest = queue.pop()
        key = (str(path), tree)
        if key in seen:
            continue
        seen.add(key)

        # Normalised, because a reference that reached across trees got here through
        # `../../../` and the raw form is unreadable in build output -- and because two
        # spellings of one file would otherwise stage it twice.
        packaged = resolver.prefix(tree) / dest
        staged[path.resolve()] = packaged
        if restricted_part(str(dest)):
            restricted.append(str(packaged))

        if path.suffix.lower() not in (".gltf", ".glb"):
            continue
        doc = read_gltf(path)
        if doc is None:
            continue

        for ref in gltf_references(doc, path):
            if not ref.exists():
                # A .ktx2 sidecar that was never built is the normal case in a source
                # tree, not an error: the loader looks for one and decodes the source
                # image when there is none. In a release it is the decode path shipped to
                # someone who cannot rebuild the cache, which is what --require-cache is.
                if require_cache and ref.suffix.lower() == ".ktx2":
                    cold.append(f"{ref.name} (referenced by {path.name})")
                continue

            placed = resolver.relocate(ref)
            if placed is None:
                # Under neither asset root, so there is nowhere in the package it could go
                # and still be found by the same relative walk.
                missing.append(f"{ref} (referenced by {path.name}, outside both asset trees)")
                continue

            ref_tree, ref_dest = placed
            queue.append((ref, ref_tree, ref_dest))

    return staged, missing, sorted(set(restricted)), sorted(set(cold))


def read_extra_names(path):
    if not path.is_file():
        return []
    names = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.append(RES_RE.sub(r"\1", line))
    return names


def main():
    repo = pathlib.Path(__file__).resolve().parent.parent

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", help="the game/<name>/ to package")
    ap.add_argument("--config", default=None,
                    help="config to seed from (default: substrate.json at the repo root)")
    ap.add_argument("--json", action="store_true", help="emit JSON for build_release.sh")
    ap.add_argument("--strict", action="store_true",
                    help="fail if the package would contain content that cannot be redistributed")
    ap.add_argument("--require-cache", action="store_true",
                    help="fail if any packaged image has no .ktx2 sidecar (bake before staging)")
    args = ap.parse_args()

    game_dir = repo / "game" / args.game
    if not (game_dir / "CMakeLists.txt").is_file():
        sys.exit(f"error: game/{args.game}/CMakeLists.txt does not exist")

    game_root = game_dir / "assets"
    engine_root = repo / "engine" / "assets"

    # Checked before anything else, and separately, because "the tree is not there" and
    # "the tree is there and is missing one file" want different messages. The test scenes
    # in both trees are committed, so a tree that is missing entirely means the fetched
    # content never arrived -- which is one script for either of them.
    for label, root in (("engine", engine_root), (args.game, game_root)):
        if not root.is_dir():
            sys.exit(f"error: {root} does not exist, so there is nothing to package.\n"
                     f"       The fetched assets are not committed. Run: scripts/fetch_assets.sh")

    config_path = pathlib.Path(args.config) if args.config else repo / "substrate.json"
    if not config_path.is_file():
        sys.exit(f"error: {config_path} does not exist")
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        sys.exit(f"error: {config_path}: {e}")

    # Three seeds, and which of them is allowed to be missing changed with S1.
    #
    # The **game's own source literals are required**, because since the scene path moved
    # out of `substrate.json` and into `GameSetup` that is where a game names what it
    # loads. Treating them as optional -- which was right while the config named the scene
    # and the literal was only a fallback -- would now let a package build with no scene in
    # it and open on a black screen, which is the exact failure this script exists to make
    # impossible.
    #
    # The **engine's literals stay optional**: those are defaults for a game that names
    # nothing, and not being there is the normal case on a machine that never fetched them.
    #
    # `package.txt` is required too -- someone wrote the name down precisely because
    # nothing else would find it.
    required = (scan_json(config)
                | set(read_extra_names(game_dir / "package.txt"))
                | scan_literals([game_dir]))
    optional = scan_literals([repo / ENGINE_SRC]) - required

    resolver = Resolver(game_root, engine_root, game_prefix=f"game/{args.game}/assets")
    staged, missing, restricted, cold = build(resolver, required, optional, strict=args.strict,
                                              require_cache=args.require_cache)

    if restricted:
        where = "\n".join(f"  {d}" for d in restricted[:8])
        more = f"\n  ... and {len(restricted) - 8} more" if len(restricted) > 8 else ""
        verb = "error" if args.strict else "note"
        print(f"{verb}: {len(restricted)} packaged file(s) cannot be redistributed:\n{where}{more}\n"
              "       Sponza is (c) Crytek under the CryEngine Limited License Agreement,\n"
              "       which is incompatible with this repository's Apache-2.0. Fine for a\n"
              "       local build; not for one that goes to other people.", file=sys.stderr)
        if args.strict:
            return 1

    if cold:
        where = "\n".join(f"  {d}" for d in cold[:8])
        more = f"\n  ... and {len(cold) - 8} more" if len(cold) > 8 else ""
        print(f"error: {len(cold)} packaged image(s) have no .ktx2 sidecar:\n{where}{more}\n"
              "       A cold cache is fine in a source tree and wrong in a package -- it\n"
              "       ships the decode path to someone who cannot rebuild it.\n"
              "       Run: scripts/ktx2.py", file=sys.stderr)
        return 1

    if missing:
        print(f"error: {len(missing)} reference(s) resolved to nothing.", file=sys.stderr)
        print(f"       searched {resolver.roots_description()}", file=sys.stderr)
        for name in sorted(missing):
            print(f"  res:/{name}", file=sys.stderr)
        print("\n       Run scripts/fetch_assets.sh if the tree is only missing what is\n"
              f"       fetched, or add the name to game/{args.game}/package.txt if it is\n"
              "       built another way.", file=sys.stderr)
        return 1

    if args.json:
        json.dump({"game": args.game,
                   "files": [{"src": str(src), "dest": str(dest)}
                             for src, dest in sorted(staged.items())]},
                  sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        for src, dest in sorted(staged.items()):
            print(f"{src} -> {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
