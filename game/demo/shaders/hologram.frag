#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/**
 * A game-authored G-buffer shader, and the demo's only one (G5).
 *
 * It exists to make the variant path a thing the tree exercises rather than a thing the
 * card claims. Everything it touches is a capability the engine grew for it: the game
 * shader tree that G5b added and nothing used, `gbuffer_contract.glsl`, `GpuMaterial`'s
 * `params`, a specialisation constant above the engine's reserved base, and the command
 * grouping that lets it be drawn beside the engine's own materials in one pass.
 *
 * Deliberately *not* named after an engine shader. G5b's rule is that a game file sharing
 * an engine file's name wins the lookup, which would change every golden image; a file
 * with its own name is additive, and a scene whose materials never select it renders
 * exactly what it rendered before this existed.
 *
 * The look: horizontal bands in world space, emissive where they land, with the base
 * colour showing through in between. `material.params` carries all four numbers, which is
 * the point of that field -- a stripe frequency is not something glTF has a slot for.
 */
#include "gbuffer_contract.glsl"

/// The engine reserves constant_id 0..7 in every id space a variant is compiled into, so
/// a variant's own start at 8. Here to prove that offset is real rather than documented:
/// with it off the surface is the base colour and nothing else, which is what a variant
/// whose interesting half is compiled out should look like.
layout(constant_id = 8) const bool ENABLE_SCANLINES = true;

void main() {
    Material m = gbufferMaterial();

    // x band frequency in world units, y phase, z emissive gain, w roughness. The demo
    // advances the phase every frame through setMaterial(), which is the same mutable
    // material path G4 built -- a variant needs no per-frame channel of its own.
    float frequency = m.params.x;
    float phase = m.params.y;
    float gain = m.params.z;

    Surface s;
    s.albedo = m.baseColorFactor.rgb;
    s.normal = normalize(vNormal);
    s.occlusion = 1.0;
    s.roughness = m.params.w;
    s.metallic = m.metallicFactor;
    s.emissive = vec3(0.0);

    if (ENABLE_SCANLINES) {
        // Half a band lit, half dark, softened over one derivative's width so the bands
        // do not alias into moire the moment the cube is more than a few metres away.
        float t = vWorldPos.y * frequency + phase;
        float band = fract(t);
        float width = max(fwidth(t), 1e-4);
        float lit = smoothstep(0.5 - width, 0.5 + width, band);
        s.emissive = m.baseColorFactor.rgb * (gain * lit);
        // Darker between the bands, so the stripes read even where nothing is lighting
        // the face they are on.
        s.albedo *= mix(0.25, 1.0, lit);
    }

    gbufferWrite(s);
}
