#!/usr/bin/env python3
"""
Check that an exported glTF carries the cloth pins the engine will look for.

    ./scripts/check_pins.py game/demo/assets/curtain.glb
    ./scripts/check_pins.py --strict game/demo/assets/*.gltf

A mesh whose name begins `FABRIC_` becomes a soft body at load, and its per-vertex
`_PIN_WEIGHT` attribute becomes the inverse masses that hold it up. Both strings are
C19's convention, not this script's -- this reads them, it does not own them. What this
owns is the one question the convention cannot answer for itself: *did the export
actually come out carrying them?*

## Why this checks the export rather than the .blend

The obvious place for this is a Blender add-on with a pre-export validator, and the card
that opened this row asked for one. It is the wrong side of the export for the failure
that matters.

**A Blender vertex group does not survive glTF export.** Groups leave only as `JOINTS_0`
and `WEIGHTS_0`, and only when an armature is present; a bare group on a curtain is
silently dropped. Weight-painting a group is the familiar gesture, so it is the first
thing an artist reaches for, and the export succeeds without a word. A validator running
*inside* Blender sees a painted group and a happy scene. Nothing before the export can
see the thing that is missing after it.

The same holds for the other three ways this goes wrong, all of them invisible upstream:
the exporter's **Data > Mesh > Attributes** checkbox left off (the attribute is simply not
written), the object renamed but not its **mesh data-block** (glTF's `mesh.name` comes
from the data-block, and that is the name the loader compares), and an attribute on the
wrong domain or type (Blender will convert or drop it without saying which).

So this reads what the engine reads: the bytes of the exported file. It needs no `bpy`,
runs on a machine with no Blender installed, and is covered by `tests/check_pins_test.py`
-- none of which is true of an add-on this repository would ship and could not test.
`docs/guides/making-a-game.md` carries the half a person does by hand.

## What it refuses, and what it merely says

Refusals -- exit 1, one line each, naming the file, the mesh and the primitive:

  * a `FABRIC_` mesh with no `_PIN_WEIGHT` at all;
  * a `_PIN_WEIGHT` that is not a non-normalized `SCALAR` of `FLOAT`, or is sparse;
  * a `_PIN_WEIGHT` whose count disagrees with `POSITION`, or whose values leave [0, 1];
  * a `FABRIC_` mesh with no vertex pinned -- cloth pinned nowhere falls through the
    floor on frame one and reads as an engine bug;
  * a node named `FABRIC_` whose mesh is not. The loader compares `meshes[i].name`, and
    Blender takes that from the mesh data-block while the node name comes from the
    object -- so renaming the object alone produces a curtain the engine never notices.

A warning -- printed, exit 0, and an error only under `--strict`: a mesh carrying
`_PIN_WEIGHT` *without* the prefix. It is dead payload rather than a broken export, and
the export is still correct for the engine. Tethered's exporter wrote the attribute onto
all twenty-nine accessors in its Sponza when one mesh needed it and nobody noticed,
because nothing looked.

**A missing input file is fatal, not a skip.** `scripts/fetch_assets.sh` deliberately
skips a generator whose source is absent, because there the source is optional content.
Here the argument is a path a person typed, and a validator that says nothing when handed
a name that does not exist is the exact failure this script exists to prevent.
"""

import argparse
import base64
import json
import math
import pathlib
import struct
import sys

# C19's, both of them. Spelled once here so a change there is one edit rather than a
# search, and deliberately not re-derived: the mesh-name prefix and the attribute name
# are the runtime's contract and this is a reader of it.
FABRIC_PREFIX = "FABRIC_"
PIN_ATTRIBUTE = "_PIN_WEIGHT"

# C19 maps `invMass = (pinWeight >= 0.999) ? 0 : 1 - pinWeight`, so this is the weight at
# or above which a vertex is actually nailed down. A cloth whose highest weight is 0.99 is
# not pinned, it is heavy, and it will sag to the floor over a few hundred steps.
PIN_THRESHOLD = 0.999

COMPONENT_FLOAT = 5126
COMPONENT_NAMES = {
    5120: "BYTE", 5121: "UNSIGNED_BYTE", 5122: "SHORT",
    5123: "UNSIGNED_SHORT", 5125: "UNSIGNED_INT", 5126: "FLOAT",
}

GLB_MAGIC = 0x46546C67
GLB_CHUNK_JSON = 0x4E4F534A
GLB_CHUNK_BIN = 0x004E4942


class Refused(Exception):
    """A document that cannot be read at all, as opposed to one that reads badly."""


def load_document(path):
    """
    The JSON and the GLB binary chunk, for either container.

    Blender's default export is `.glb`, so refusing to read one would mean this check
    only ever ran on the export an artist had to be told to make.
    """
    data = path.read_bytes()
    if len(data) >= 4 and struct.unpack_from("<I", data, 0)[0] == GLB_MAGIC:
        if len(data) < 12:
            raise Refused("truncated GLB header")
        _, version, total = struct.unpack_from("<III", data, 0)
        if version != 2:
            raise Refused(f"GLB version {version}, expected 2")
        doc = None
        binary = None
        offset = 12
        while offset + 8 <= min(total, len(data)):
            length, kind = struct.unpack_from("<II", data, offset)
            body = data[offset + 8:offset + 8 + length]
            if len(body) < length:
                raise Refused("truncated GLB chunk")
            if kind == GLB_CHUNK_JSON and doc is None:
                doc = json.loads(body.decode("utf-8"))
            elif kind == GLB_CHUNK_BIN and binary is None:
                binary = body
            offset += 8 + length + (-length % 4)
        if doc is None:
            raise Refused("GLB carries no JSON chunk")
        return doc, binary

    try:
        return json.loads(data.decode("utf-8")), None
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise Refused(f"not glTF JSON and not a GLB ({exc})") from exc


def buffer_bytes(doc, binary, index, base):
    """
    One buffer's contents, from the GLB chunk, a data URI, or a file beside the document.

    All three appear in this tree: Blender writes GLB or a `.bin` sibling, and the
    committed test scenes carry base64 data URIs so each is one file.
    """
    buffers = doc.get("buffers", [])
    if index >= len(buffers):
        raise Refused(f"buffer {index} out of range")
    uri = buffers[index].get("uri")
    if uri is None:
        if binary is None:
            raise Refused(f"buffer {index} has no uri and the file has no binary chunk")
        return binary
    if uri.startswith("data:"):
        _, _, payload = uri.partition(",")
        return base64.b64decode(payload)
    source = base / uri
    if not source.is_file():
        raise Refused(f"buffer {index} names {uri}, which does not exist beside the document")
    return source.read_bytes()


def accessor_floats(doc, binary, base, index):
    """
    A SCALAR/FLOAT accessor's values, or a sentence saying why it is not one.

    Returns `(values, None)` or `(None, reason)`. The reason is the message the caller
    prints, so it names what was found rather than only what was wanted -- the same shape
    D12 gave the settings parse: the key, the text, the legal spelling, and what is
    standing in its place.
    """
    accessors = doc.get("accessors", [])
    if index >= len(accessors):
        return None, f"names accessor {index}, which the document does not have"
    acc = accessors[index]

    kind = acc.get("type")
    if kind != "SCALAR":
        return None, f"is type {kind}, expected SCALAR (one float per vertex)"
    component = acc.get("componentType")
    if component != COMPONENT_FLOAT:
        found = COMPONENT_NAMES.get(component, str(component))
        return None, (f"is componentType {found}, expected FLOAT -- author the attribute "
                      f"as Float on the Vertex domain, not Byte Color or Integer")
    if acc.get("normalized"):
        return None, "is normalized, expected raw float"
    if "sparse" in acc:
        return None, "is sparse, which this check cannot read and the loader does not expect"

    count = acc.get("count", 0)
    view_index = acc.get("bufferView")
    if view_index is None:
        # Legal glTF: an accessor with no view reads as zeros. Legal and useless here,
        # and worth its own sentence rather than falling out as "no vertex is pinned".
        return None, "has no bufferView, so every weight is zero and nothing is pinned"

    views = doc.get("bufferViews", [])
    if view_index >= len(views):
        return None, f"names bufferView {view_index}, which the document does not have"
    view = views[view_index]
    raw = buffer_bytes(doc, binary, view.get("buffer", 0), base)

    stride = view.get("byteStride") or 4
    start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    end = start + (count - 1) * stride + 4 if count else start
    if end > len(raw):
        return None, (f"reads {end} bytes from a {len(raw)}-byte buffer -- the file is "
                      f"truncated or its bufferView is wrong")

    return [struct.unpack_from("<f", raw, start + i * stride)[0] for i in range(count)], None


def accessor_count(doc, index):
    accessors = doc.get("accessors", [])
    return accessors[index].get("count") if index < len(accessors) else None


def mesh_label(doc, index):
    name = doc.get("meshes", [])[index].get("name")
    return f"mesh {index} '{name}'" if name else f"mesh {index} (unnamed)"


def near_misses(attributes):
    """
    Attribute names that were probably meant to be `_PIN_WEIGHT`.

    Case and separator, because those are the two an exporter is entitled to change and a
    person is entitled to mistype, and "attribute absent" is a much worse thing to be told
    when `_pin_weight` is sitting right there.
    """
    def fold(name):
        return name.replace("_", "").replace(".", "").replace(" ", "").lower()

    target = fold(PIN_ATTRIBUTE)
    return [name for name in attributes if name != PIN_ATTRIBUTE and fold(name) == target]


def check_document(path, doc, binary, report):
    """Every check this script makes, over one already-parsed document."""
    base = path.parent
    meshes = doc.get("meshes", [])

    # The rename trap, and it is first because it is the one that produces a file where
    # everything else here passes and the engine still sees no cloth.
    for i, node in enumerate(doc.get("nodes", [])):
        name = node.get("name", "")
        mesh = node.get("mesh")
        if not name.startswith(FABRIC_PREFIX) or mesh is None or mesh >= len(meshes):
            continue
        if not meshes[mesh].get("name", "").startswith(FABRIC_PREFIX):
            report.error(
                path, f"node {i} '{name}' is {FABRIC_PREFIX}-named but its "
                      f"{mesh_label(doc, mesh)} is not. The loader reads the mesh name, which "
                      f"Blender takes from the mesh data-block -- rename it in Object Data "
                      f"Properties, not just the object in the outliner")

    for i, mesh in enumerate(meshes):
        name = mesh.get("name", "")
        fabric = name.startswith(FABRIC_PREFIX)
        primitives = mesh.get("primitives", [])
        if fabric and not primitives:
            report.error(path, f"{mesh_label(doc, i)} is {FABRIC_PREFIX}-named and has no primitives")

        for p, prim in enumerate(primitives):
            attributes = prim.get("attributes", {})
            where = f"{mesh_label(doc, i)} primitive {p}"

            if PIN_ATTRIBUTE not in attributes:
                if fabric:
                    misses = near_misses(attributes)
                    custom = sorted(a for a in attributes if a.startswith("_"))
                    if misses:
                        detail = (f"carries {', '.join(misses)} instead. The name is "
                                  f"case-sensitive on both sides")
                    elif custom:
                        detail = f"carries only {', '.join(custom)}"
                    else:
                        detail = ("carries no custom attribute at all -- tick Data > Mesh > "
                                  "Attributes in the glTF exporter, and check the pins are an "
                                  "attribute rather than a vertex group")
                    report.error(path, f"{where} has no {PIN_ATTRIBUTE}: {detail}")
                continue

            if not fabric:
                report.warn(path, f"{where} carries {PIN_ATTRIBUTE} but its name does not begin "
                                  f"{FABRIC_PREFIX}, so the loader will never read it -- "
                                  f"dead payload in every buffer that ships it")

            values, reason = accessor_floats(doc, binary, base, attributes[PIN_ATTRIBUTE])
            if reason is not None:
                report.error(path, f"{where}: {PIN_ATTRIBUTE} {reason}")
                continue

            positions = attributes.get("POSITION")
            expected = accessor_count(doc, positions) if positions is not None else None
            if expected is not None and len(values) != expected:
                report.error(path, f"{where}: {PIN_ATTRIBUTE} has {len(values)} values for "
                                   f"{expected} vertices. It must be on the Vertex (POINT) "
                                   f"domain, one value per vertex")
                continue

            bad = [v for v in values if not math.isfinite(v) or v < 0.0 or v > 1.0]
            if bad:
                report.error(path, f"{where}: {PIN_ATTRIBUTE} has {len(bad)} values outside "
                                   f"[0, 1], first {bad[0]:g}. A weight is a fraction, not a mass")
                continue

            if fabric and not any(v >= PIN_THRESHOLD for v in values):
                top = max(values) if values else 0.0
                report.error(path, f"{where}: {PIN_ATTRIBUTE} pins nothing -- the highest weight "
                                   f"is {top:g} and a vertex is held only at >= {PIN_THRESHOLD}. "
                                   f"Cloth pinned nowhere falls through the floor on frame one")
            elif fabric:
                report.cloth(where, sum(1 for v in values if v >= PIN_THRESHOLD), len(values))


class Report:
    """Counts and prints. Separate from the checks so the tests can read the counts."""

    def __init__(self, quiet=False):
        self.errors = []
        self.warnings = []
        self.cloths = []
        self.quiet = quiet

    def error(self, path, message):
        self.errors.append(message)
        print(f"error: {path}: {message}", file=sys.stderr)

    def warn(self, path, message):
        self.warnings.append(message)
        print(f"warning: {path}: {message}", file=sys.stderr)

    def cloth(self, where, pinned, total):
        self.cloths.append((where, pinned, total))
        if not self.quiet:
            print(f"  {where}: {pinned} of {total} vertices pinned")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenes", nargs="+", type=pathlib.Path)
    ap.add_argument("--strict", action="store_true",
                    help="treat dead payload as an error too")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    report = Report(args.quiet)

    for path in args.scenes:
        if not path.is_file():
            report.error(path, "not found")
            continue
        try:
            doc, binary = load_document(path)
            check_document(path, doc, binary, report)
        except Refused as exc:
            report.error(path, str(exc))

    failed = len(report.errors) + (len(report.warnings) if args.strict else 0)
    if not args.quiet:
        def count(n, noun):
            return f"{n} {noun}" if n == 1 else f"{n} {noun}s"
        print(f"{count(len(report.cloths), 'cloth primitive')}, "
              f"{count(len(report.errors), 'error')}, "
              f"{count(len(report.warnings), 'warning')}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
