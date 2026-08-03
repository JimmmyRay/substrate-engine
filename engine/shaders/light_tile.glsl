/**
 * Where a pixel's light bits live: the screen tile grid shared by the pass that builds
 * the per-tile light mask and the two passes that read it (C35).
 *
 * ## Tiled, not clustered
 *
 * One 16x16 screen tile carries one bit per light, and its depth bounds come from the
 * G-buffer depth the tile actually holds -- so the volume a light is tested against is a
 * frustum slab, not a fixed froxel. There is no subdivision along z. The tree calls this
 * tiled and means it; see rendering.md.
 *
 * ## The bitmask is the residency model, and that is what keeps the sum bit-exact
 *
 * The shading loop accumulates `color += shadeLight(...)` in buffer order, and
 * floating-point addition is not associative -- so a per-tile list that compacted indices
 * would have to keep them ascending or move pixels for no reason. A bitmask iterated
 * low bit first *is* ascending order, with no compaction, no per-tile count and no
 * atomics whose completion order could be observed. Dropping a light is bit-exact for the
 * separate reason that `lightRadiance` returns exactly `vec3(0.0)` outside a light's
 * range, so the loop's first `continue` already contributed nothing.
 *
 * Requires frame.glsl: every value below is read out of `frame.tileParams`.
 */

/// First word of this pixel's tile in the mask buffer.
///
/// **The integer arithmetic here has to match light_tile.comp's `gl_WorkGroupID`
/// exactly** -- that pass writes one tile per workgroup and this is what indexes the
/// result. `tileParams.y` is the tile size, and it is the compute shader's
/// `local_size_x` and `Renderer::kLightTileSize` written a third time; change one and all
/// three move.
uint lightTileBase(ivec2 coord) {
    uvec2 tile = uvec2(coord) / frame.tileParams.y;
    return (tile.y * frame.tileParams.x + tile.x) * frame.tileParams.z;
}

/// Mask words a light loop must walk for `lightCount` lights.
int lightMaskWords(int lightCount) {
    return (lightCount + 31) / 32;
}

/// The bits of word `w`, or every bit where `render.lightTiles` is off.
///
/// **The all-ones fallback is what makes the escape hatch an escape hatch**: the loop
/// shape stays one loop, in one order, and switching tiling off walks 0..lightCount-1
/// exactly as the flat loop it replaced did. A second loop body for the off case would be
/// two things to keep bit-identical rather than one.
#define LIGHT_TILE_WORD(base, w) (frame.tileParams.z == 0u ? 0xFFFFFFFFu : lightTiles[(base) + uint(w)])
