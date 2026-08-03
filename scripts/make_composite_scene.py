#!/usr/bin/env python3
"""
Graft generated props onto a glTF file this repository cannot ship.

Every other test scene is committed, because a scene written from nothing is first-party
and a file is a better record of it than a script. Two things S2 and 3.11 need cannot be:
an animated character and a mirror both want to sit in an environment, and the
environments worth using -- Sponza, a Mixamo rig -- are third-party files under licenses
that keep them out of the repository.

**That is what keeps this script alive.** Its outputs are deep copies of their inputs --
`character.gltf` carries the rig's entire node, accessor and animation tables -- so
committing them would commit the very files the licenses exclude. The script is the only
part of the arrangement this repository is allowed to hold.

So the input is a path and the output is a sibling file that references the original's
buffers and images where they already are:

  character.gltf   an external skinned character on a ground plane, with a back wall
                   for its shadow to fall on. Framing is the reason the plane exists:
                   Camera::frameBounds aims down the longest horizontal axis from a
                   quarter of its length, which for a lone 1.8 m figure puts the camera
                   inside its hip. A 16 m floor makes the same rule frame the character.
  reflect.gltf     Sponza plus a mirror sphere and a row of increasing roughness.
                   Sponza has no smooth surface anywhere, so SSR and 3.11's ray-traced
                   reflections had nothing in it to reflect *in* -- every check went
                   through a debug view rather than through the image.

    ./scripts/make_composite_scene.py [--character game/demo/assets/Mana/Mana.gltf]

Nothing here is committed. The outputs land in game/demo/assets/ beside the rest of the
demo's content, and both are regenerated from the sources by re-running this.

Sponza is the engine's, under engine/assets/, because the golden suite pins it. That puts
it and these outputs in different trees, and the relative URIs cross accordingly --
`rebase_uris` computes them with os.path.relpath rather than writing them, so moving
either tree needs no edit here.
"""

import argparse
import base64
import copy
import json
import math
import os
import pathlib
import struct

GENERATOR = "Substrate scripts/make_composite_scene.py"


# ------------------------------------------------------------------ glTF assembly
# These three were shared with the generator that wrote the rest of the test scenes.
# Those scenes are committed now and that generator is gone, leaving one caller -- so
# they live here rather than in a module shared with nobody.


def pack_floats(rows):
    return b"".join(struct.pack("<" + "f" * len(r), *r) for r in rows)


def unit_box():
    """
    A closed 1x1x1 box centred on the origin, as (positions, normals, tangents, uvs,
    indices).

    A box rather than a quad, and the reason is the shadow pass: it culls *front*
    faces, so a zero-thickness quad rasterises its own surface into the shadow map at
    its own depth and then fails its own depth test. Solid geometry has a back face
    somewhere behind the front one, which is the whole premise that trick relies on.
    """
    faces = [
        ((0.0, 0.0, 1.0), (1.0, 0.0, 0.0)),
        ((0.0, 0.0, -1.0), (-1.0, 0.0, 0.0)),
        ((1.0, 0.0, 0.0), (0.0, 0.0, -1.0)),
        ((-1.0, 0.0, 0.0), (0.0, 0.0, 1.0)),
        ((0.0, 1.0, 0.0), (1.0, 0.0, 0.0)),
        ((0.0, -1.0, 0.0), (1.0, 0.0, 0.0)),
    ]
    positions, normals, tangents, uvs, indices = [], [], [], [], []
    for n, t in faces:
        bitangent = (n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0])
        centre = tuple(c * 0.5 for c in n)
        base = len(positions)
        for su, sv in ((-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)):
            positions.append(tuple(centre[i] + t[i] * su + bitangent[i] * sv for i in range(3)))
            normals.append(n)
            tangents.append((t[0], t[1], t[2], 1.0))
            uvs.append((su + 0.5, sv + 0.5))
        # Counter-clockwise seen from outside, matching the renderer's front face.
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, tangents, uvs, indices


class Geometry:
    """
    Accumulates meshes into one buffer, emitting bufferViews and accessors as it goes.

    What makes this a class rather than a function is the padding below: every accessor
    has to start 4-byte aligned, and a scene that happens to be aligned anyway would
    import perfectly right up until someone added a mesh that was not.
    """

    ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
    FLOAT, UNSIGNED_INT = 5126, 5125

    def __init__(self, buffer_index=0, view_base=0, accessor_base=0):
        """
        The three bases are why this file needs them: a graft appends generated geometry
        to a document that already has buffers, views and accessors of its own, and every
        index it emits has to be absolute in that document.
        """
        self.chunks, self.views, self.accessors = [], [], []
        self.offset = 0
        self.buffer_index, self.view_base, self.accessor_base = buffer_index, view_base, accessor_base

    def add(self, data, target, count, type_, comp_type, minv=None, maxv=None):
        pad = (-self.offset) % 4
        if pad:
            self.chunks.append(b"\x00" * pad)
            self.offset += pad
        view = {"buffer": self.buffer_index, "byteOffset": self.offset, "byteLength": len(data)}
        # Animation samplers and inverse bind matrices are read by the loader, not bound
        # as vertex or index data, and the spec says such a bufferView must *not*
        # declare a target. Emitting one anyway makes strict validators reject the file.
        if target is not None:
            view["target"] = target
        self.views.append(view)
        acc = {"bufferView": self.view_base + len(self.views) - 1, "componentType": comp_type, "count": count,
               "type": type_}
        if minv is not None:
            acc["min"], acc["max"] = minv, maxv
        self.accessors.append(acc)
        self.chunks.append(data)
        self.offset += len(data)
        return self.accessor_base + len(self.accessors) - 1

    def add_mesh(self, positions, normals, tangents, uvs, indices):
        xs, ys, zs = zip(*positions)
        p = self.add(pack_floats(positions), self.ARRAY_BUFFER, len(positions), "VEC3", self.FLOAT,
                     [min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)])
        n = self.add(pack_floats(normals), self.ARRAY_BUFFER, len(normals), "VEC3", self.FLOAT)
        t = self.add(pack_floats(tangents), self.ARRAY_BUFFER, len(tangents), "VEC4", self.FLOAT)
        u = self.add(pack_floats(uvs), self.ARRAY_BUFFER, len(uvs), "VEC2", self.FLOAT)
        i = self.add(struct.pack("<%dI" % len(indices), *indices), self.ELEMENT_ARRAY_BUFFER, len(indices),
                     "SCALAR", self.UNSIGNED_INT)
        return {"POSITION": p, "NORMAL": n, "TANGENT": t, "TEXCOORD_0": u}, i

    def buffer(self):
        blob = b"".join(self.chunks)
        return {"byteLength": len(blob),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}


def rebase_uris(gltf, base_dir, out_dir):
    """
    Point every relative `uri` at where the file actually is, seen from `out_dir`.

    A grafted scene is written somewhere else, so `Sponza.bin` and every texture name
    beside it stop resolving. Data URIs and absolute URLs are left alone -- they carry
    their own contents or their own host, and rewriting either would break them.
    """
    for holder in ("buffers", "images"):
        for entry in gltf.get(holder, []):
            uri = entry.get("uri")
            if uri is None or uri.startswith("data:") or "://" in uri:
                continue
            entry["uri"] = os.path.relpath(base_dir / uri, out_dir).replace(os.sep, "/")


def graft(base, geometry, materials, meshes, nodes):
    """
    Append generated content to a parsed glTF, fixing up every index it carries.

    The four arrays arrive already written in *local* terms -- material 0, mesh 0 -- and
    are shifted by however much the base already holds. The one index that is not
    shifted here is an accessor: `Geometry` was constructed with the base's counts, so
    what it hands back is already absolute. Two conventions in one function is a real
    hazard, and the reason it is done this way is that an accessor index is buried
    inside a primitive's attribute dictionary where a generic pass cannot see it.
    """
    material_base = len(base.setdefault("materials", []))
    mesh_base = len(base.setdefault("meshes", []))
    node_base = len(base.setdefault("nodes", []))

    for mesh in meshes:
        for prim in mesh["primitives"]:
            prim["material"] += material_base
    for node in nodes:
        node["mesh"] += mesh_base

    base["materials"] += materials
    base["meshes"] += meshes
    base["nodes"] += nodes
    base.setdefault("bufferViews", []).extend(geometry.views)
    base.setdefault("accessors", []).extend(geometry.accessors)
    base.setdefault("buffers", []).append(geometry.buffer())

    scene = base["scenes"][base.get("scene", 0)]
    scene["nodes"] += [node_base + i for i in range(len(nodes))]
    base["asset"]["generator"] = GENERATOR
    return base


def open_base(path, out_dir):
    with open(path, "r", encoding="utf-8") as f:
        gltf = json.load(f)
    rebase_uris(gltf, pathlib.Path(path).resolve().parent, out_dir)
    return gltf, Geometry(buffer_index=len(gltf.get("buffers", [])),
                          view_base=len(gltf.get("bufferViews", [])),
                          accessor_base=len(gltf.get("accessors", [])))


# --------------------------------------------------------------------------- geometry


def plane(width, depth, normal_y=1.0, tile=1.0):
    """A double-width quad in the XZ plane, wound counter-clockwise seen from +Y."""
    hw, hd = width * 0.5, depth * 0.5
    positions = [(-hw, 0.0, -hd), (hw, 0.0, -hd), (hw, 0.0, hd), (-hw, 0.0, hd)]
    normals = [(0.0, normal_y, 0.0)] * 4
    tangents = [(1.0, 0.0, 0.0, 1.0)] * 4
    uvs = [(0.0, 0.0), (tile, 0.0), (tile, tile), (0.0, tile)]
    # Reversed relative to the naive winding: with [0, 1, 2, ...] the front face points
    # down, back-face culling removes it, and the floor renders as more sky. Invisible in
    # the lit view -- the floor just looks like sky -- and obvious in the cascade view.
    return positions, normals, tangents, uvs, [0, 2, 1, 0, 3, 2]


def wall(depth, height, x):
    """
    A quad in the YZ plane at `x`, facing -X: the backdrop for a camera looking along
    +X, which is where `Camera::frameBounds` puts one when the floor is square.

    Facing matters twice over. A wall the camera sees the back of is culled away, and a
    wall whose front faces away from the sun is a black slab against the sky -- which is
    what the first version of this scene rendered, and it reads as a lighting bug rather
    than as a wall pointed the wrong way.
    """
    hd = depth * 0.5
    positions = [(x, 0.0, hd), (x, 0.0, -hd), (x, height, -hd), (x, height, hd)]
    normals = [(-1.0, 0.0, 0.0)] * 4
    tangents = [(0.0, 0.0, -1.0, 1.0)] * 4
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    return positions, normals, tangents, uvs, [0, 1, 2, 0, 2, 3]


def uv_sphere(radius=1.0, rings=48, sectors=96):
    """
    A UV sphere with real tangents, since a mirror is exactly what shows a wrong one.

    Rings and sectors are high because this is the surface a reflection is *read off*:
    a coarse sphere reflects in facets, and a facet edge looks enough like a ray-march
    artefact to send someone debugging the wrong thing.
    """
    positions, normals, tangents, uvs, indices = [], [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        phi = v * math.pi
        for s in range(sectors + 1):
            u = s / sectors
            theta = u * 2.0 * math.pi
            n = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
            positions.append(tuple(c * radius for c in n))
            normals.append(n)
            # d/dtheta of the position, normalised: the direction u increases in.
            tangents.append((-math.sin(theta), 0.0, math.cos(theta), 1.0))
            uvs.append((u, v))
    row = sectors + 1
    for r in range(rings):
        for s in range(sectors):
            a, b = r * row + s, (r + 1) * row + s
            # Counter-clockwise seen from *outside*. The other order is the one that
            # reads naturally and it is inside out: back-face culling then removes the
            # outer shell, the camera sees the inside of the far hemisphere, and its
            # shading normals point away -- so the sphere renders as a black disc that
            # looks exactly like a reflection returning nothing.
            indices += [a, a + 1, b, a + 1, b + 1, b]
    return positions, normals, tangents, uvs, indices


def aim_quat(direction):
    """
    Rotation taking a light's local -Z onto `direction`, as glTF's xyzw quaternion.

    Shortest arc, built from the half-vector: the axis is `-Z x d` and the angle is
    twice its half, which `w = 1 + dot` and a normalise express without a trig call.
    The antiparallel case is the one that needs saying -- `-Z x +Z` is the zero vector
    and the formula degenerates, so a light aimed straight up picks an axis by hand.

    The cross product carried a sign error once, which negated the axis and so rotated
    the light the wrong way about the right line -- mirroring the aim through the plane
    that line lies in. It put both suns below the horizon, where they lit nothing and
    self-shadowed every surface in the scene, and nothing downstream could tell: the
    file validated, loaded and rendered, it just rendered a world with no sunlight in
    it. tests/make_composite_scene_test.py rotates -Z by the result and checks it comes
    back as `direction`, which is the property this function actually promises.
    """
    length = math.sqrt(sum(c * c for c in direction))
    d = tuple(c / length for c in direction)
    dot = -d[2]  # dot((0, 0, -1), d)
    if dot < -0.999999:
        return [1.0, 0.0, 0.0, 0.0]  # half turn about X
    axis = (d[1], -d[0], 0.0)  # cross((0, 0, -1), d)
    q = [axis[0], axis[1], axis[2], 1.0 + dot]
    n = math.sqrt(sum(c * c for c in q))
    return [c / n for c in q]


def punctual(gltf, lights, nodes):
    """Attach a KHR_lights_punctual block and the nodes that place it."""
    gltf.setdefault("extensionsUsed", [])
    if "KHR_lights_punctual" not in gltf["extensionsUsed"]:
        gltf["extensionsUsed"].append("KHR_lights_punctual")
    gltf.setdefault("extensions", {})["KHR_lights_punctual"] = {"lights": lights}

    node_base = len(gltf["nodes"])
    gltf["nodes"] += nodes
    gltf["scenes"][gltf.get("scene", 0)]["nodes"] += [node_base + i for i in range(len(nodes))]


def matte(name, colour, roughness=0.85):
    return {"name": name,
            "pbrMetallicRoughness": {"baseColorFactor": list(colour) + [1.0], "metallicFactor": 0.0,
                                     "roughnessFactor": roughness}}


def metal(name, colour, roughness):
    return {"name": name,
            "pbrMetallicRoughness": {"baseColorFactor": list(colour) + [1.0], "metallicFactor": 1.0,
                                     "roughnessFactor": roughness}}


# ------------------------------------------------------------------------- merging


def build_character(path, out_dir, ground=16.0):
    gltf, g = open_base(path, out_dir)

    floor_attrs, floor_idx = g.add_mesh(*plane(ground, ground, tile=8.0))
    back_attrs, back_idx = g.add_mesh(*wall(ground, ground * 0.4, ground * 0.5))
    box_attrs, box_idx = g.add_mesh(*unit_box())

    materials = [matte("stage_floor", [0.34, 0.35, 0.38], 0.75),
                 matte("stage_wall", [0.52, 0.50, 0.47], 0.95),
                 matte("stage_block", [0.55, 0.50, 0.42], 0.85)]
    meshes = [{"name": "stage_floor", "primitives": [{"attributes": floor_attrs, "indices": floor_idx,
                                                      "material": 0}]},
              {"name": "stage_wall", "primitives": [{"attributes": back_attrs, "indices": back_idx,
                                                     "material": 1}]},
              {"name": "stage_block", "primitives": [{"attributes": box_attrs, "indices": box_idx,
                                                      "material": 2}]}]
    # The block is a control, not scenery. A character that casts no shadow and a scene
    # whose shadows are switched off look identical, and a static box of about the
    # character's height standing beside it is what tells the two apart in one glance.
    nodes = [{"name": "stage_floor", "mesh": 0}, {"name": "stage_wall", "mesh": 1},
             {"name": "stage_block", "mesh": 2, "translation": [0.0, 0.9, -2.2],
              "scale": [0.7, 1.8, 0.7]}]
    scene = graft(gltf, g, materials, meshes, nodes)

    # The stage ships its own lights, and that is the point rather than a nicety.
    # The demo auto-places a point set for scenes that declare none, and that heuristic
    # is fitted to Sponza: three lights and a spot, scaled by the scene *radius*, in a
    # building whose surfaces are metres away from them. Drop the same set into a flat
    # 16 m room and the nearest lit surface is the floor a metre and a half below --
    # inverse-square does the rest and the floor blows out to white. A file that states
    # its own lighting takes priority over the whole heuristic, which is both the fix
    # here and the reason the heuristic is left alone for Sponza.
    lights = [
        {"type": "directional", "name": "stage_sun", "color": [1.0, 0.96, 0.9], "intensity": 3.0},
        {"type": "point", "name": "stage_key", "color": [1.0, 0.9, 0.78], "intensity": 30.0, "range": 20.0},
        {"type": "point", "name": "stage_fill", "color": [0.6, 0.72, 1.0], "intensity": 14.0, "range": 20.0},
    ]
    # Aimed across the camera rather than down it: a sun directly behind the viewer puts
    # the shadow behind the character where nothing can see it, which is exactly what
    # this scene exists to show.
    light_nodes = [
        {"name": "stage_sun", "rotation": aim_quat((0.62, -0.52, -0.58)),
         "extensions": {"KHR_lights_punctual": {"light": 0}}},
        {"name": "stage_key", "translation": [-3.0, 4.5, 3.0],
         "extensions": {"KHR_lights_punctual": {"light": 1}}},
        {"name": "stage_fill", "translation": [4.0, 3.5, -3.5],
         "extensions": {"KHR_lights_punctual": {"light": 2}}},
    ]
    punctual(scene, lights, light_nodes)
    return scene


def build_reflect(path, out_dir, centre=(1.5, 2.0, 0.0), radius=1.1):
    """
    Sponza with a mirror over the atrium floor and five spheres of rising roughness.

    The mirror is `roughnessFactor` 0.0 and fully metallic, which is the one material
    where a reflection is the *whole* surface -- there is no diffuse term underneath to
    hide a reflection that is subtly wrong. The row beside it is the complement: SSR and
    3.11 both fade out with roughness, and a row is how you see *where*, rather than
    reading a cutoff out of the config and trusting it.
    """
    gltf, g = open_base(path, out_dir)

    sphere_attrs, sphere_idx = g.add_mesh(*uv_sphere())

    roughnesses = [0.0, 0.1, 0.25, 0.45, 0.7]
    materials = [metal("mirror", [0.95, 0.95, 0.95], 0.0)]
    materials += [metal(f"metal_r{int(r * 100):02d}", [0.90, 0.78, 0.42], r) for r in roughnesses]

    meshes = [{"name": "mirror", "primitives": [{"attributes": sphere_attrs, "indices": sphere_idx,
                                                 "material": 0}]}]
    meshes += [{"name": f"rough_{i}", "primitives": [{"attributes": sphere_attrs, "indices": sphere_idx,
                                                      "material": 1 + i}]}
               for i in range(len(roughnesses))]

    nodes = [{"name": "mirror", "mesh": 0, "translation": list(centre),
              "scale": [radius, radius, radius]}]
    # A row down X, which is the axis Sponza's nave runs along -- 30 units against 18 --
    # and therefore both the direction `Camera::frameBounds` aims the default camera and
    # the one with something at the far end to reflect.
    small = radius * 0.5
    for i in range(len(roughnesses)):
        x = centre[0] + (i - 1) * radius * 2.4
        nodes.append({"name": f"rough_{i}", "mesh": 1 + i,
                      "translation": [x, small, centre[2] - radius * 2.6],
                      "scale": [small, small, small]})
    return graft(gltf, g, materials, meshes, nodes)


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    out_dir = root / "game" / "demo" / "assets"

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--character", default=str(root / "game" / "demo" / "assets" / "Mana" / "Mana.gltf"),
                        help="skinned, animated glTF to stage (default: game/demo/assets/Mana/Mana.gltf)")
    parser.add_argument("--sponza", default=str(root / "engine" / "assets" / "Sponza" / "glTF" / "Sponza.gltf"),
                        help="scene to put the mirror in (default: engine/assets/Sponza/glTF/Sponza.gltf)")
    args = parser.parse_args()

    out_dir.mkdir(parents=True, exist_ok=True)

    # Each half is skipped rather than fatal when its source is missing: the two inputs
    # come from different places and neither is in the repository, so a checkout with
    # only Sponza fetched should still get the mirror.
    for name, source, build in (("character.gltf", args.character, build_character),
                                ("reflect.gltf", args.sponza, build_reflect)):
        if not pathlib.Path(source).is_file():
            print(f"skipping {name}: {source} not found")
            continue
        out = out_dir / name
        out.write_text(json.dumps(build(source, out_dir), indent=2) + "\n")
        print(f"wrote {out} from {source}")


if __name__ == "__main__":
    main()
