#!/usr/bin/env python3
"""
Tests for scripts/check_pins.py.

    ./tests/check_pins_test.py

Python rather than gtest because the thing under test is a Python script, following
`tests/manifest_test.py`. It is not part of `./test.sh`, which builds and runs a C++
binary; run it directly, or through the CI workflow, which runs all three.

Every case here is a *failure* case bar one, and that is the point of the row: the
validator is worth exactly what it refuses. The last case is the correct export, and it
exists so that "refuses everything" cannot pass for "refuses the right things".

The fixtures are written here rather than shipped, because a broken glTF is not an asset
-- nothing loads it, nothing renders it, and a file in `assets/` whose whole purpose is to
be wrong is a file someone will eventually try to open.
"""

import base64
import contextlib
import importlib.util
import io
import json
import pathlib
import shutil
import struct
import tempfile
import unittest

_spec = importlib.util.spec_from_file_location(
    "check_pins", pathlib.Path(__file__).resolve().parent.parent / "scripts" / "check_pins.py")
check_pins = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_pins)


# A two-triangle quad is the smallest thing that is recognisably a curtain: four vertices,
# the top two pinned, which is the shape every case below varies from.
QUAD_POSITIONS = [(-1.0, 1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, -1.0, 0.0), (1.0, -1.0, 0.0)]
TOP_EDGE_PINNED = [1.0, 1.0, 0.0, 0.0]


def build(mesh_name="FABRIC_Curtain", node_name=None, weights=TOP_EDGE_PINNED,
          attribute="_PIN_WEIGHT", component=5126, kind="SCALAR", normalized=False,
          drop_attribute=False):
    """A one-mesh glTF, self-contained in a data URI, wrong in whichever way was asked for."""
    payload = b"".join(struct.pack("<fff", *p) for p in QUAD_POSITIONS)
    pin_offset = len(payload)
    if component == 5126:
        payload += b"".join(struct.pack("<f", w) for w in weights)
        pin_stride = 4
    else:
        payload += bytes(min(255, max(0, round(w * 255))) for w in weights)
        pin_stride = 1

    attributes = {"POSITION": 0}
    if not drop_attribute:
        attributes[attribute] = 1

    return {
        "asset": {"version": "2.0", "generator": "Substrate tests/check_pins_test.py"},
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "nodes": [{"name": node_name or mesh_name, "mesh": 0}],
        "meshes": [{"name": mesh_name, "primitives": [{"attributes": attributes}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(QUAD_POSITIONS),
             "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": component, "count": len(weights),
             "type": kind, "normalized": normalized},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": pin_offset},
            {"buffer": 0, "byteOffset": pin_offset, "byteLength": len(weights) * pin_stride},
        ],
        "buffers": [{"byteLength": len(payload),
                     "uri": "data:application/octet-stream;base64,"
                            + base64.b64encode(payload).decode("ascii")}],
    }


class CheckPinsTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp(prefix="substrate_pins_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write(self, doc, name="curtain.gltf"):
        path = self.root / name
        path.write_text(json.dumps(doc, indent=2) + "\n")
        return path

    def run_check(self, *args):
        """The script's exit code, its stdout and its stderr, as a person would see them."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = check_pins.main([str(a) for a in args])
        return code, out.getvalue(), err.getvalue()

    # -- the five the card named ------------------------------------------------------

    def test_fabric_mesh_without_the_attribute_is_refused(self):
        code, _, err = self.run_check(self.write(build(drop_attribute=True)))
        self.assertEqual(code, 1)
        self.assertIn("FABRIC_Curtain", err)
        self.assertIn("_PIN_WEIGHT", err)
        # The refusal has to say what to do, because "attribute missing" is what an artist
        # who ticked no export checkbox sees, and the checkbox is the usual cause.
        self.assertIn("Attributes", err)

    def test_wrong_type_is_refused_and_says_what_it_found(self):
        code, _, err = self.run_check(self.write(build(component=5121, normalized=True)))
        self.assertEqual(code, 1)
        self.assertIn("UNSIGNED_BYTE", err)
        self.assertIn("FLOAT", err)

    def test_wrong_domain_shows_up_as_a_count_that_disagrees(self):
        # A face- or corner-domain attribute does not arrive as one value per vertex. The
        # domain itself is gone by the time the file exists, so a count that disagrees with
        # POSITION is the shape the mistake has on this side of the exporter.
        code, _, err = self.run_check(self.write(build(weights=[1.0, 1.0])))
        self.assertEqual(code, 1)
        self.assertIn("2 values for 4 vertices", err)
        self.assertIn("Vertex (POINT) domain", err)

    def test_cloth_pinned_nowhere_is_refused(self):
        code, _, err = self.run_check(self.write(build(weights=[0.9, 0.9, 0.0, 0.0])))
        self.assertEqual(code, 1)
        self.assertIn("pins nothing", err)
        self.assertIn("0.9", err)

    def test_dead_payload_warns_rather_than_refusing(self):
        path = self.write(build(mesh_name="Curtain"))
        code, _, err = self.run_check(path)
        self.assertEqual(code, 0)
        self.assertIn("warning:", err)
        self.assertNotIn("error:", err)

        # ...and is an error under --strict, which is what a packaging step wants.
        code, _, _ = self.run_check("--strict", path)
        self.assertEqual(code, 1)

    def test_a_correct_export_reports_nothing(self):
        code, out, err = self.run_check(self.write(build()))
        self.assertEqual(code, 0)
        self.assertEqual(err, "")
        self.assertIn("2 of 4 vertices pinned", out)
        self.assertIn("0 errors, 0 warnings", out)

    # -- the traps the export side adds -----------------------------------------------

    def test_object_renamed_but_not_the_mesh_data_is_refused(self):
        # The one that produces a file where every other check here passes and the engine
        # still sees no cloth: glTF's mesh.name is the Blender data-block, not the object.
        doc = build(mesh_name="Plane", node_name="FABRIC_Curtain")
        code, _, err = self.run_check(self.write(doc))
        self.assertEqual(code, 1)
        self.assertIn("mesh data-block", err)
        self.assertIn("Object Data Properties", err)

    def test_a_near_miss_name_is_named_rather_than_called_absent(self):
        code, _, err = self.run_check(self.write(build(attribute="_pin_weight")))
        self.assertEqual(code, 1)
        self.assertIn("_pin_weight", err)
        self.assertIn("case-sensitive", err)

    def test_weights_outside_zero_to_one_are_refused(self):
        code, _, err = self.run_check(self.write(build(weights=[1.0, 1.0, -0.5, 0.0])))
        self.assertEqual(code, 1)
        self.assertIn("outside [0, 1]", err)

    def test_a_missing_file_is_fatal_rather_than_silent(self):
        code, _, err = self.run_check(self.root / "nothing-here.gltf")
        self.assertEqual(code, 1)
        self.assertIn("not found", err)

    def test_a_directory_of_scenes_is_checked_in_one_run(self):
        good = self.write(build(), "good.gltf")
        bad = self.write(build(weights=[0.0, 0.0, 0.0, 0.0]), "bad.gltf")
        code, out, err = self.run_check(good, bad)
        self.assertEqual(code, 1)
        self.assertIn("bad.gltf", err)
        self.assertNotIn("good.gltf", err)
        self.assertIn("1 error,", out)

    # -- the container Blender writes by default --------------------------------------

    def test_a_glb_is_read_the_same_as_a_gltf(self):
        payload = b"".join(struct.pack("<fff", *p) for p in QUAD_POSITIONS)
        payload += b"".join(struct.pack("<f", w) for w in TOP_EDGE_PINNED)

        doc = build()
        doc["buffers"] = [{"byteLength": len(payload)}]
        blob = json.dumps(doc).encode("utf-8")
        blob += b" " * (-len(blob) % 4)
        binary = payload + b"\0" * (-len(payload) % 4)

        glb = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(blob) + 8 + len(binary))
        glb += struct.pack("<II", len(blob), 0x4E4F534A) + blob
        glb += struct.pack("<II", len(binary), 0x004E4942) + binary

        path = self.root / "curtain.glb"
        path.write_bytes(glb)

        code, out, err = self.run_check(path)
        self.assertEqual(code, 0, err)
        self.assertIn("2 of 4 vertices pinned", out)

    def test_a_file_that_is_neither_is_refused_by_name(self):
        path = self.root / "curtain.gltf"
        path.write_bytes(b"\x00\x01 not a scene")
        code, _, err = self.run_check(path)
        self.assertEqual(code, 1)
        self.assertIn("not glTF JSON and not a GLB", err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
