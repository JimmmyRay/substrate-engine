/**
 * Shared body of the tile light-assignment pass (C35).
 *
 * Included by light_tile.comp (multisampled G-buffer) and light_tile1x.comp, for
 * the reason lighting_body.glsl has two consumers: a 1x G-buffer is genuinely
 * single-sampled and cannot bind to a `sampler2DMS` descriptor.
 *
 * One workgroup per screen tile. Each invocation reads its own pixel's depth -- every
 * sample of it -- into a shared min/max, then the invocations divide the light list
 * between them and set a bit for each light whose volume reaches the slab those bounds
 * describe.
 *
 * ## One binding out of the G-buffer set, and no shading constants
 *
 * The depth attachment is declared here rather than by including gbuffer_read.glsl,
 * because that file's `samplesAgree` pulls in features.glsl and with it the whole shading
 * id space -- seven specialisation constants for a pass that gates no feature. The sample
 * count arrives in the push constant instead. **A `pSpecializationInfo` on a compute stage
 * is also what the validation layers in SDK 1.3.280 cannot handle**: they re-run the
 * constant folding themselves and reject the SPIR-V 1.6 `OpExecutionModeId LocalSizeId`
 * that glslang emits for every compute shader here, so a specialised dispatch logs
 * `does not contain valid spirv` on a module `spirv-val` calls valid.
 *
 * ## Why this reads gDepth and not the Hi-Z pyramid
 *
 * The pyramid is a `min` reduction of the *resolved* depth, and both halves of that are
 * wrong for this. `min` alone gives the far bound and leaves the near one at the camera,
 * which is most of the volume a tile does not occupy; and the resolve is
 * `VK_RESOLVE_MODE_AVERAGE`, so a silhouette pixel reports a depth between its samples
 * rather than either of them. A bound taken from an average is not a bound -- it sits
 * nearer than the farthest sample, and a light past it would be culled from a sample that
 * can see it. That is the one failure this pass is not allowed to have.
 */

/// The G-buffer's depth, at the binding gbuffer_read.glsl gives it -- one entry of a set
/// this pass otherwise ignores. `GBUFFER_MULTISAMPLE`, defined by the includer, picks the
/// sampler type, and a 1x G-buffer is genuinely single-sampled: that is the whole reason
/// this file has two consumers.
#ifdef GBUFFER_MULTISAMPLE
layout(set = 1, binding = 3) uniform sampler2DMS gDepth;
#define DFETCH(coord, s) texelFetch(gDepth, coord, s)
#else
layout(set = 1, binding = 3) uniform sampler2D gDepth;
#define DFETCH(coord, s) texelFetch(gDepth, coord, 0)
#endif

/// Bound on the shared mask, so the array is an ordinary one rather than sized by a
/// specialisation constant. 1024 lights; `Renderer::kLightTileMaxWords` is the same
/// number and disables tiling above it rather than truncating a light list.
const uint kLightTileMaxWords = 32u;

/// The tile grid's bits, one word per 32 lights. `writeonly`: nothing here reads back
/// what another workgroup wrote, and saying so keeps the compiler from ordering against
/// a read that does not exist.
layout(set = 2, binding = 0) writeonly buffer LightTiles {
    uint lightTiles[];
};

layout(push_constant) uniform Push {
    /// The view's render extent in pixels. Tiles at the right and bottom edges are
    /// partial, and the invocations past the edge must not fetch: a `texelFetch` out of
    /// range is undefined, and folding one into the depth bounds would grow or shrink the
    /// slab by whatever the driver returned.
    uvec2 extent;
    uvec2 tiles;
    /// G-buffer samples per pixel. Every one of them is bounded, because every one of them
    /// is shaded: a silhouette pixel's samples sit at different depths and the resolve
    /// loop lights each from its own position.
    uint samples;
} pc;

/// 16 here is `Renderer::kLightTileSize` and `frame.tileParams.y`. One tile per
/// workgroup and one invocation per pixel, which is what makes `gl_WorkGroupID` the tile
/// index `lightTileBase` computes on the consuming side.
layout(local_size_x = 16, local_size_y = 16) in;

/// Nearest and farthest depth in the tile, as float bit patterns. Every depth is
/// non-negative, and IEEE-754 orders non-negative floats the same way their bit patterns
/// order as unsigned integers -- which is what lets `atomicMin`/`atomicMax` reduce them
/// without a float atomic.
shared uint sNearBits;
shared uint sFarBits;
shared uint sMask[kLightTileMaxWords];
/// Inward-facing, world space, normalised. Built once per tile by invocation 0 -- 256
/// copies of six planes would be the same six planes.
shared vec4 sPlanes[6];

vec4 normalizePlane(vec4 p) {
    float len = length(p.xyz);
    // A plane with no normal constrains nothing, so it must answer "inside" for every
    // point rather than reject everything. Reachable only from a degenerate projection,
    // and the alternative is a frame with no lights in it.
    return len > 0.0 ? p / len : vec4(0.0, 0.0, 0.0, 1.0);
}

/**
 * Could this light reach any point of the tile's slab?
 *
 * **Conservative, and that is the whole contract** -- exactly `lightVisible`'s in
 * Light.h, one volume smaller. A false positive costs a light in a loop that would have
 * contributed nothing; a false negative moves a pixel.
 */
bool lightReachesTile(Light light) {
    // A directional light has no position, and range 0 is the unbounded convention
    // `GpuLight::position.w` states. Neither is a sphere, so neither is cullable: the two
    // exemptions `lightVisible` makes against the view frustum, made again here against
    // the tile. Getting the second one wrong would delete every light in a scene that
    // authored no range.
    if (int(light.params.z) == LIGHT_DIRECTIONAL) return true;
    float range = light.position.w;
    if (range <= 0.0) return true;

    // **The dilation is not slop, and removing it is how this stops being an
    // equivalence.** The cull is exact in real arithmetic -- `lightRadiance` windows to
    // exactly `vec3(0.0)` at `dist >= range` -- but the two sides evaluate different
    // expressions in floating point: the shading pass reconstructs P through
    // `invViewProj`, a numerical inverse, while these planes come from the rows of
    // `viewProj` itself. A light whose range boundary falls within a few ulps of a tile
    // edge could then be dropped by a test that says "unreachable" while the fragment
    // computes a `dist` a hair inside it. Two parts because the error has two scales: a
    // relative term for the round-trip, and an absolute floor for a light small enough
    // that the relative term underflows the world coordinates it is measured in.
    float reach = range * 1.0002 + 1e-3;

    // A spot is culled by the sphere that bounds its cone, for the reason `lightVisible`
    // gives: the sphere is conservative, and a cone test is far easier to get subtly
    // wrong than it is to profit from.
    for (int k = 0; k < 6; ++k) {
        if (dot(sPlanes[k].xyz, light.position.xyz) + sPlanes[k].w < -reach) return false;
    }
    return true;
}

void main() {
    // Clamped rather than trusted: `Renderer` refuses to run this pass above the bound,
    // and an out-of-range shared write would be the kind of corruption that reads as a
    // driver fault.
    const uint words = min(frame.tileParams.z, kLightTileMaxWords);
    const uvec2 tile = gl_WorkGroupID.xy;
    const uint local = gl_LocalInvocationIndex;

    if (local == 0u) {
        sFarBits = 0x7F800000u; // +inf, so the first depth wins the min
        sNearBits = 0u;         // +0.0, which is FAR_DEPTH and is what "nothing here" means
    }
    if (local < words) sMask[local] = 0u;
    barrier();

    const ivec2 coord = ivec2(tile * gl_WorkGroupSize.xy + gl_LocalInvocationID.xy);
    if (uint(coord.x) < pc.extent.x && uint(coord.y) < pc.extent.y) {
        for (uint s = 0u; s < pc.samples; ++s) {
            float d = DFETCH(coord, int(s)).r;
            // A sample still holding the clear is a pixel nothing was drawn into. The
            // shading loop returns the skybox for it and reaches no light at all, so
            // letting it into the bounds would stretch the slab to the far plane for a
            // sample that shades no lights.
            if (d > FAR_DEPTH) {
                atomicMin(sFarBits, floatBitsToUint(d));
                atomicMax(sNearBits, floatBitsToUint(d));
            }
        }
    }
    barrier();

    // Uniform across the workgroup: it is read from shared memory behind a barrier, which
    // is what lets the branches below skip work without a barrier inside one of them.
    const bool anyGeometry = sNearBits != 0u;

    if (anyGeometry && local == 0u) {
        // The tile's rectangle in NDC. `min` against the extent rather than the tile
        // grid's own bound, so an edge tile describes the pixels it holds instead of the
        // pixels it would hold if the screen were a multiple of the tile size.
        const vec2 lo = vec2(tile * gl_WorkGroupSize.xy) / vec2(pc.extent);
        const vec2 hi = vec2(min((tile + 1u) * gl_WorkGroupSize.xy, pc.extent)) / vec2(pc.extent);
        const vec2 n0 = lo * 2.0 - 1.0;
        const vec2 n1 = hi * 2.0 - 1.0;

        // Gribb-Hartmann, but for a sub-rectangle of the frustum rather than the whole
        // of it: each plane is the clip-space inequality the tile imposes, written as a
        // linear function of world position. `ndc.x >= n0.x` is `c.x - n0.x * c.w >= 0`,
        // and `c` is `viewProj * vec4(P, 1)` -- so the plane is row x less `n0.x` times
        // row w. The same construction gives the depth bounds from rows z and w, which is
        // why this needs no separate near/far handling and no knowledge of which
        // projection built the matrix.
        //
        // `frame.viewProj` and not the unjittered one: the G-buffer was rasterised
        // through the jittered matrix and `worldFromDepth` inverts the same member, so a
        // tile built from the other one would be offset by the jitter every frame TAA is on.
        const mat4 vp = frame.viewProj;
        const vec4 rx = vec4(vp[0].x, vp[1].x, vp[2].x, vp[3].x);
        const vec4 ry = vec4(vp[0].y, vp[1].y, vp[2].y, vp[3].y);
        const vec4 rz = vec4(vp[0].z, vp[1].z, vp[2].z, vp[3].z);
        const vec4 rw = vec4(vp[0].w, vp[1].w, vp[2].w, vp[3].w);

        // Reverse-Z: the *smaller* depth is the farther surface, so the far plane is the
        // lower bound and the near plane the upper one. Reading these the other way round
        // builds a slab behind the camera and culls every light in the frame.
        const float dFar = uintBitsToFloat(sFarBits);
        const float dNear = uintBitsToFloat(sNearBits);

        sPlanes[0] = normalizePlane(rx - n0.x * rw);
        sPlanes[1] = normalizePlane(n1.x * rw - rx);
        sPlanes[2] = normalizePlane(ry - n0.y * rw);
        sPlanes[3] = normalizePlane(n1.y * rw - ry);
        sPlanes[4] = normalizePlane(rz - dFar * rw);
        sPlanes[5] = normalizePlane(dNear * rw - rz);
    }
    barrier();

    if (anyGeometry) {
        const int lightCount = int(frame.params.y);
        const int stride = int(gl_WorkGroupSize.x * gl_WorkGroupSize.y);
        for (int i = int(local); i < lightCount; i += stride) {
            if (lightReachesTile(lights[i])) atomicOr(sMask[i >> 5], 1u << (uint(i) & 31u));
        }
    }
    barrier();

    // Every word, every tile, every frame. A tile that shades nothing writes zeros rather
    // than being skipped: the buffer is never cleared, and a tile left holding last
    // frame's bits would light this frame's pixels from wherever the camera used to be.
    if (local < words) {
        lightTiles[(tile.y * pc.tiles.x + tile.x) * words + local] = sMask[local];
    }
}
