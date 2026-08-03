// Per-frame uniforms. Must match FrameUniforms in engine/gfx/Renderer.h exactly.
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

layout(set = 0, binding = 0) uniform FrameData {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 cameraForward; // xyz = normalised view direction
    vec4 depthLinear;   // inverts the projection's depth; see viewDistance() below

    vec4 sunDirection;  // xyz = direction toward the light, w = intensity
    vec4 sunColor;      // rgb

    vec4 params;        // x = exposure, y = light count, z = specular AA, w = bloom strength
    vec4 lightParams;   // x = light cutoff, squared and divided out of exposure; yzw spare
    vec4 ambient;       // rgb = flat ambient radiance, w unused. See pbr.glsl.
    uvec4 flags;        // x = debug view, y = sample count, z = traced reflections composite this frame,
                        // w = the projection is orthographic
    // The tile light grid (C35). x = tiles across, y = tile size in pixels, z = mask words
    // per tile, w spare. **z is zero exactly when `render.lightTiles` is off**, and the
    // light loops read that as "walk every light" -- see light_tile.glsl.
    uvec4 tileParams;

    // Cascaded shadow maps -- the non-traced path. Unread by the ray-query variants.
    mat4 sunViewProj;         // the sun's box, fitted to the scene and not to the camera
    vec4 shadowParams;        // x texel size (UV), y depth bias (NDC), z normal bias (world), w texel (world)

    // Previous frame's view-projection, *unjittered*, for TAA reprojection (3.4).
    // Unjittered because the history buffer holds the converged image, which is
    // aligned to pixel centres rather than to any single frame's sub-pixel offset.
    mat4 prevViewProj;
    mat4 invViewProjNoJitter;
} frame;

/// One punctual light. Must match GpuLight in engine/gfx/Renderer.h exactly.
struct Light {
    vec4 position;  // xyz world, w range (0 = unbounded)
    // xyz: for a spot, the direction the light points; for a directional, the
    // direction *toward* the light. The asymmetry is deliberate and is argued for
    // in Light.h -- a sun is authored by where it is, a spot by where it aims.
    vec4 direction;
    vec4 color;     // rgb, w intensity
    vec4 params;    // x cos(inner), y cos(outer), z type, w first shadow layer or -1
};

layout(set = 0, binding = 1) readonly buffer Lights {
    Light lights[];
};

/// One view-projection per punctual atlas layer: six per point light, one per spot.
/// Written by updateLights, read by shadow.vert to render a layer and by shadow.glsl to
/// sample it.
layout(set = 0, binding = 2) readonly buffer ShadowMatrices {
    mat4 shadowMatrices[];
};

/// Reverse-Z: the far plane is depth 0, not 1. One named constant rather than a bare
/// `0.0` compared against a depth sample, because a literal zero in that position reads
/// as "the near plane" to everyone who has not worked on this renderer.
///
/// Declared here rather than in each consumer -- it was three copies, each with its own
/// comment saying the same thing (D8).
const float FAR_DEPTH = 0.0;

/// The world-space position a depth sample came from.
///
/// **This was four copies under three names** -- `worldFromDepth` in `ssao.comp`,
/// `ssr_body.glsl` and `particle_simulate.comp`, `worldPositionFromDepth` in
/// `lighting_body.glsl` -- plus the same expression open-coded in `decal.frag` and
/// `fog.comp`. Three of the four differed only in what they called their locals. It lives
/// here because `frame.glsl` owns `invViewProj`, which is what makes this the narrowest
/// scope every consumer already reaches (D8).
///
/// @param uv    0..1, the pixel being reconstructed.
/// @param depth the depth *as sampled*, in reverse-Z clip space.
vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 world = frame.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

/// How far along the view axis a depth sample is, and a distance no march reaches where
/// nothing was drawn.
///
/// **This was three copies of `pc.nearPlane / depth`** -- `ssr_body.glsl`, `fog.comp` and
/// `particle_simulate.comp` -- and that expression is satisfied by exactly one matrix,
/// the infinite reverse-Z perspective one. D8 collapsed `worldFromDepth` and left this
/// alone because at the time there was only one projection and the formula was correct;
/// P3 added a second and finished the job. It lives here for D8's reason, unchanged: the
/// coefficients are a frame property and `frame.glsl` is the narrowest scope every
/// consumer already reaches.
///
/// `frame.depthLinear` is the projection's z and w rows solved for view-space z --
/// `Camera::depthLinear` reads it straight off the matrix. Under the infinite perspective
/// matrix it evaluates `(0 - near) / (-depth - 0)`, which is `near / depth` to the bit;
/// under a parallel one it evaluates the affine `far - depth * (far - near)`.
///
/// **Four coefficients rather than two, and the two are not a matter of taste.** A
/// perspective depth inverts to a multiple of `1/depth` and a parallel one to a
/// combination of `1` and `depth`; spanning both needs three basis functions, so no
/// expression carrying two numbers can be exact for both families. Four is the general
/// rational form, and it is also the one that can be read off the matrix.
///
/// The depth buffer is *cleared* to `FAR_DEPTH` under either projection, so a sample
/// still holding it is a pixel nothing was drawn into rather than a surface sitting on
/// the far plane -- which is why the miss answers 1e9 under both, and not `orthoFar`.
float viewDistance(float depth) {
    if (depth <= FAR_DEPTH) return 1e9;
    return (depth * frame.depthLinear.x - frame.depthLinear.y) /
           (depth * frame.depthLinear.z - frame.depthLinear.w);
}
