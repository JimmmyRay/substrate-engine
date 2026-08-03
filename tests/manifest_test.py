#!/usr/bin/env python3
"""
Tests for scripts/manifest.py.

    ./tests/manifest_test.py

Python rather than gtest because the thing under test is a Python script. It is not part
of `./test.sh`, which builds and runs a C++ binary; run it directly, or through the CI
workflow, which runs both.

The case that earns most of this file is the first one: resolution order has to match
`Resources::Resources` exactly, or the packaging step stages one file and the binary opens
another. `tests/ResourcesTests.cpp` pins the engine's side against the same three
scenarios -- engine-only, game-only, and both -- and these are deliberately the same
scenarios so that a divergence shows up as one of the two suites going red.
"""

import importlib.util
import json
import pathlib
import shutil
import struct
import sys
import tempfile
import unittest

_spec = importlib.util.spec_from_file_location(
    "manifest", pathlib.Path(__file__).resolve().parent.parent / "scripts" / "manifest.py")
manifest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(manifest)


class TreeTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp(prefix="substrate_manifest_"))
        self.game = self.root / "game"
        self.engine = self.root / "engine"
        self.game.mkdir()
        self.engine.mkdir()
        self.resolver = manifest.Resolver(self.game, self.engine)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def write(self, path, body=""):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
        return path

    def gltf(self, path, doc):
        return self.write(path, json.dumps(doc))


class Resolution(TreeTest):
    """The three cases ResourcesTests.cpp pins on the engine side."""

    def test_resolves_out_of_the_engine_tree(self):
        self.write(self.engine / "only_engine.gltf")
        path, tree = self.resolver.resolve("only_engine.gltf")
        self.assertEqual(tree, "engine")
        self.assertEqual(path, self.engine / "only_engine.gltf")

    def test_resolves_out_of_the_game_tree(self):
        self.write(self.game / "only_game.gltf")
        path, tree = self.resolver.resolve("only_game.gltf")
        self.assertEqual(tree, "game")

    def test_the_game_tree_wins_when_both_hold_the_name(self):
        # The property most worth pinning, and the one that is invisible until two trees
        # disagree. Same assertion as ResourcesTests.cpp makes about the engine.
        self.write(self.engine / "shared.gltf", "engine")
        self.write(self.game / "shared.gltf", "game")
        path, tree = self.resolver.resolve("shared.gltf")
        self.assertEqual(tree, "game")
        self.assertEqual(path.read_text(), "game")

    def test_a_name_in_neither_tree_raises(self):
        with self.assertRaises(manifest.Missing):
            self.resolver.resolve("absent.gltf")


class Closure(TreeTest):
    def test_follows_buffers_and_images(self):
        self.write(self.game / "tex.png")
        self.gltf(self.game / "s.gltf", {"buffers": [{"uri": "s.bin"}], "images": [{"uri": "tex.png"}]})
        self.write(self.game / "s.bin")

        staged, missing, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(missing, [])
        dests = sorted(str(d) for d in staged.values())
        self.assertIn("game/demo/assets/s.bin", dests)
        self.assertIn("game/demo/assets/tex.png", dests)

    def test_skips_data_uris(self):
        self.gltf(self.game / "s.gltf", {"buffers": [{"uri": "data:application/octet-stream;base64,AAAA"}]})
        staged, missing, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(missing, [])
        self.assertEqual(len(staged), 1)

    def test_stages_a_ktx2_sidecar_when_one_exists(self):
        self.write(self.game / "tex.png")
        self.write(self.game / "tex.png.ktx2")
        self.gltf(self.game / "s.gltf", {"images": [{"uri": "tex.png"}]})

        staged, _, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertIn("game/demo/assets/tex.png.ktx2", [str(d) for d in staged.values()])

    def test_a_missing_ktx2_sidecar_is_not_an_error(self):
        # The loader looks for one and decodes the source image when there is none, so an
        # absent cache is the normal state and not a broken package.
        self.write(self.game / "tex.png")
        self.gltf(self.game / "s.gltf", {"images": [{"uri": "tex.png"}]})

        staged, missing, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(missing, [])
        self.assertNotIn("game/demo/assets/tex.png.ktx2", [str(d) for d in staged.values()])

    def test_follows_audio_declared_in_node_extras(self):
        # Not a glTF-standard array, and the reason a grep for `res:/` finds nothing: the
        # engine resolves these relative to the document, never through the scheme.
        self.write(self.game / "audio" / "hum.wav")
        self.gltf(self.game / "s.gltf",
                  {"nodes": [{"extras": {"substrate_audio": {"file": "audio/hum.wav"}}}]})

        staged, missing, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(missing, [])
        self.assertIn("game/demo/assets/audio/hum.wav", [str(d) for d in staged.values()])

    def test_follows_a_reference_across_into_the_other_tree(self):
        # What the composite scenes actually do: a game scene grafted onto a glTF in the
        # engine tree, reached with ../../../engine/assets/...
        self.write(self.engine / "shared" / "tex.png")
        self.gltf(self.game / "s.gltf", {"images": [{"uri": "../engine/shared/tex.png"}]})

        staged, missing, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(missing, [])
        self.assertIn("engine/assets/shared/tex.png", [str(d) for d in staged.values()])

    def test_a_reference_outside_both_trees_is_reported(self):
        self.write(self.root / "stray.png")
        self.gltf(self.game / "s.gltf", {"images": [{"uri": "../stray.png"}]})

        _, missing, _, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(len(missing), 1)
        self.assertIn("outside both asset trees", missing[0])

    def test_a_cycle_terminates(self):
        self.gltf(self.game / "a.gltf", {"buffers": [{"uri": "b.gltf"}]})
        self.gltf(self.game / "b.gltf", {"buffers": [{"uri": "a.gltf"}]})

        staged, missing, _, _ = manifest.build(self.resolver, {"a.gltf"}, set())
        self.assertEqual(missing, [])
        self.assertEqual(len(staged), 2)

    def test_reads_a_glb_json_chunk(self):
        doc = json.dumps({"buffers": [{"uri": "s.bin"}]}).encode()
        pad = (-len(doc)) % 4
        doc += b" " * pad
        blob = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(doc)) + struct.pack("<II", len(doc), 0x4E4F534A) + doc
        (self.game / "s.glb").write_bytes(blob)
        self.write(self.game / "s.bin")

        staged, missing, _, _ = manifest.build(self.resolver, {"s.glb"}, set())
        self.assertEqual(missing, [])
        self.assertIn("game/demo/assets/s.bin", [str(d) for d in staged.values()])


class Seeds(TreeTest):
    """Which seeds may be missing, and which may not.

    The balance shifted with S1. While `substrate.json` named the scene, a source literal
    was only a compiled-in fallback the packaged config overrode, so a missing one was the
    normal case. Now `GameSetup::scene` in the game's own source *is* where the scene is
    named, so a game literal that resolves to nothing has to fail the build -- otherwise a
    package builds happily with no scene in it and opens on a black screen.
    """

    def test_a_required_seed_that_resolves_to_nothing_fails(self):
        _, missing, _, _ = manifest.build(self.resolver, {"absent.gltf"}, set())
        self.assertEqual(missing, ["absent.gltf"])

    def test_an_optional_seed_that_resolves_to_nothing_is_fine(self):
        staged, missing, _, _ = manifest.build(self.resolver, set(), {"absent.gltf"})
        self.assertEqual(missing, [])
        self.assertEqual(staged, {})


class ColdCache(TreeTest):
    """The fourth thing `build` returns, and the reason it is separate from `missing`.

    A `.ktx2` sidecar that was never built is the normal state of a source tree -- the
    loader looks for one and decodes the source image when there is none -- so it is not a
    missing file. It *is* wrong in a package, because the decode path then ships to somebody
    who cannot rebuild the cache, which is what `--require-cache` exists to refuse.

    These cases exist because the tests dropped this value for as long as it has existed:
    every call site unpacked three of four, which is a `ValueError` in twenty-four places and
    was invisible locally because nothing ran the suite outside CI.
    """

    def sceneWithAnImage(self):
        self.write(self.game / "tex.png")
        self.gltf(self.game / "s.gltf", {"images": [{"uri": "tex.png"}]})

    def test_a_cold_sidecar_is_reported_when_the_cache_is_required(self):
        self.sceneWithAnImage()
        _, _, _, cold = manifest.build(self.resolver, {"s.gltf"}, set(), require_cache=True)
        self.assertEqual(len(cold), 1, cold)
        self.assertIn("tex.png.ktx2", cold[0])
        self.assertIn("s.gltf", cold[0], "the report has to name the document that wants it")

    def test_a_cold_sidecar_is_silent_by_default(self):
        # The distinction the whole field is for: same tree, same absence, no complaint.
        self.sceneWithAnImage()
        _, missing, _, cold = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(cold, [])
        self.assertEqual(missing, [], "a cold sidecar is not a missing file")

    def test_a_built_sidecar_is_not_cold(self):
        self.sceneWithAnImage()
        self.write(self.game / "tex.png.ktx2")
        staged, _, _, cold = manifest.build(self.resolver, {"s.gltf"}, set(), require_cache=True)
        self.assertEqual(cold, [])
        self.assertIn("game/demo/assets/tex.png.ktx2", [str(d) for d in staged.values()])


class Licensing(TreeTest):
    """Restricted content is packaged and reported, not refused.

    A local build is not distribution, and refusing to package Sponza made the common case
    -- a developer building a runnable artifact for themselves -- impossible. What is worth
    keeping is the bookkeeping: the list exists, so `--strict` can act on it the day a build
    does go to other people.
    """

    def test_restricted_content_is_packaged_and_listed(self):
        self.write(self.engine / "Sponza" / "glTF" / "Sponza.gltf")
        staged, missing, restricted, _ = manifest.build(self.resolver, {"Sponza/glTF/Sponza.gltf"}, set())
        self.assertEqual(missing, [])
        self.assertIn("engine/assets/Sponza/glTF/Sponza.gltf", [str(d) for d in staged.values()])
        self.assertEqual(restricted, ["engine/assets/Sponza/glTF/Sponza.gltf"])

    def test_a_scene_grafted_onto_restricted_content_still_packages(self):
        # showcase.gltf's real shape: self-contained by name, grafted onto Sponza by uri.
        # Both files land in the package, and both are reported.
        self.write(self.engine / "Sponza" / "glTF" / "Sponza.bin")
        self.gltf(self.game / "showcase.gltf",
                  {"buffers": [{"uri": "../engine/Sponza/glTF/Sponza.bin"}]})

        staged, missing, restricted, _ = manifest.build(self.resolver, {"showcase.gltf"}, set())
        self.assertEqual(missing, [])
        dests = sorted(str(d) for d in staged.values())
        self.assertIn("game/demo/assets/showcase.gltf", dests)
        self.assertIn("engine/assets/Sponza/glTF/Sponza.bin", dests)
        self.assertEqual(restricted, ["engine/assets/Sponza/glTF/Sponza.bin"])

    def test_unrestricted_content_reports_nothing(self):
        self.write(self.game / "tex.png")
        self.gltf(self.game / "s.gltf", {"images": [{"uri": "tex.png"}]})

        _, _, restricted, _ = manifest.build(self.resolver, {"s.gltf"}, set())
        self.assertEqual(restricted, [])

    def test_an_absent_optional_default_is_not_an_error(self):
        # `run.sh` names Sponza when no game was named, and a machine that never ran
        # fetch_assets.sh does not have it. A packaged game names its own scene, so this
        # must not fail the build.
        staged, missing, _, _ = manifest.build(self.resolver, set(), {"Sponza/glTF/Sponza.gltf"})
        self.assertEqual(staged, {})
        self.assertEqual(missing, [])


class LiteralScanning(unittest.TestCase):
    def scan(self, source):
        d = pathlib.Path(tempfile.mkdtemp(prefix="substrate_manifest_src_"))
        try:
            (d / "x.cpp").write_text(source, encoding="utf-8")
            return manifest.scan_literals([d])
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_finds_a_name_in_a_string_literal(self):
        self.assertEqual(self.scan('const char* s = "res:/main.gltf";'), {"main.gltf"})

    def test_ignores_a_name_in_a_line_comment(self):
        self.assertEqual(self.scan('// Resources("res:/showcase.gltf") resolves to ...'), set())

    def test_ignores_a_name_in_a_block_comment(self):
        # Resources.h's own file comment is exactly this, and taking its examples as
        # dependencies had the packaging step demanding a scene nothing asks for.
        self.assertEqual(self.scan('/**\n * Resources("res:/showcase.gltf")\n */'), set())

    def test_ignores_a_printf_format_string(self):
        # Resources.cpp logs `res:/%.*s not found in ...`, which names nothing.
        self.assertEqual(self.scan('Logger::warn(c, "res:/%.*s not found", n);'), set())

    def test_a_literal_containing_a_double_slash_is_not_cut_short(self):
        self.assertEqual(self.scan('auto a = "http://x"; auto b = "res:/real.gltf";'), {"real.gltf"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
