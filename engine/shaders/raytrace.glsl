/**
 * Shading the surface a ray query actually hit.
 *
 * ## Why this exists
 *
 * 3.11 traced a reflection ray, found the exact triangle, and then threw that away:
 * it reprojected the hit into screen space and sampled the lit image, falling back to
 * the environment cube wherever the depth buffer disagreed. That made a ray query into
 * an expensive way to ask the depth buffer a question -- it inherited screen-space
 * reflection's blind spot instead of removing it, and an off-screen hit still came back
 * as sky.
 *
 * This header is the other half: given a committed intersection, read the geometry and
 * the material behind it and shade the surface properly. What that buys is a reflection
 * of things the camera cannot see, and -- because the same function serves the ambient
 * pass -- one bounce of indirect light that carries the *colour* of what it bounced off.
 *
 * ## What a ray query gives you, and what it does not
 *
 * Three integers and two barycentrics. Not a material, not a normal, not a UV. The
 * bridge is `SceneAccelStruct::hitRecords`, indexed by `instanceCustomIndex +
 * geometryIndex` -- see GpuHitRecord in AccelStruct.h for why that one addition covers
 * both the static and the deformed tier with no branch.
 *
 * ## Buffer references rather than descriptors
 *
 * The vertex, index and hit-record buffers arrive as device addresses in the caller's
 * push constants, not as bound SSBOs. `bufferDeviceAddress` is enabled unconditionally
 * (VMA wants it anyway), and the alternative costs another descriptor set on every
 * pipeline that traces -- for buffers whose contents are fixed for the life of the
 * scene. `scalar` layout, not std430, because the C++ `Vertex` is tightly packed at 48
 * bytes and std430 would round its `vec3`s up to 16 and read every field from the wrong
 * offset.
 *
 * ## What is still approximate
 *
 * Alpha-masked geometry is opaque in the acceleration structure, so a hit on foliage
 * shades the whole quad. Ambient at a hit is the split-sum IBL lookup -- the same
 * smooth, scene-blind lookup the lighting pass uses, by decision: the traced
 * alternative was grain everywhere ambient dominates. Both are stated in
 * limitations.md.
 */

// The extensions this header needs -- GL_EXT_buffer_reference2, GL_EXT_scalar_block_layout
// and GL_EXT_nonuniform_qualifier -- are declared by the file that includes it, not here.
// An #extension directive has to precede every declaration in the translation unit, and
// this header is included after frame.glsl has already declared a uniform block; put them
// here and glslang quietly drops them, after which `scalar` stops applying and the vertex
// struct is validated under std430 rules it does not satisfy. GL_EXT_ray_query is declared
// the same way for the same reason.

/// Which set the bindless scene data is bound at. The deferred path spends set 1 on the
/// G-buffer, so the tracing compute passes put the scene set past everything else.
#ifndef RT_SCENE_SET
#define RT_SCENE_SET 5
#endif

layout(set = RT_SCENE_SET, binding = 0) readonly buffer RtMaterials {
    Material rtMaterials[];
};
layout(set = RT_SCENE_SET, binding = 1) uniform sampler2D rtTextures[];

/// Must match `Vertex` in engine/scene/GltfScene.h. 48 bytes, and `scalar` is what keeps
/// it 48 rather than 64.
struct RtVertex {
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RtVertices {
    RtVertex v[];
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RtIndices {
    uint i[];
};
/// Must match GpuHitRecord in engine/gfx/AccelStruct.h.
struct RtHitRecord {
    uint firstIndex;
    uint instanceSlot;
    uint deformed;
    uint pad;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RtHitRecords {
    RtHitRecord r[];
};

/**
 * Every buffer a hit lookup needs, gathered so callers pass one value rather than five.
 *
 * The static and deformed tiers keep separate vertex *and* index buffers, and they are
 * never mixed: a record's `deformed` flag picks a pair. The deformed indices were
 * rebased onto the deformed vertex buffer once at load, which is why nothing here has to
 * add a base offset -- see the "Indices are rebased once" note in AccelStruct.h.
 */
struct RayScene {
    RtHitRecords records;
    RtVertices sceneVertices;
    RtIndices sceneIndices;
    RtVertices deformedVertices;
    RtIndices deformedIndices;
};

/// Geometry of a hit, in world space.
struct RayHit {
    vec3 position;
    vec3 normal;
    vec2 uv;
    uint material;
};

/**
 * Resolve a committed intersection to world-space geometry.
 *
 * The normal is the interpolated vertex normal put through the instance's normal matrix.
 * That is correct for both tiers for the same reason from two different directions: the
 * static tier bakes its transform into the BLAS geometry so its *positions* are already
 * world space while its stored normals are not, and the deformed tier keeps model-space
 * vertices under a TLAS instance transform. Either way the stored normal is model space
 * and the instance's normal matrix is what takes it out.
 */
RayHit resolveRayHit(rayQueryEXT rq, RayScene scene, vec3 rayOrigin, vec3 rayDir) {
    const uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) +
                        uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
    RtHitRecord h = scene.records.r[record];

    const uint base = h.firstIndex + 3u * uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));

    uvec3 tri;
    RtVertices verts;
    if (h.deformed == 0u) {
        tri = uvec3(scene.sceneIndices.i[base], scene.sceneIndices.i[base + 1u], scene.sceneIndices.i[base + 2u]);
        verts = scene.sceneVertices;
    } else {
        tri = uvec3(scene.deformedIndices.i[base], scene.deformedIndices.i[base + 1u],
                    scene.deformedIndices.i[base + 2u]);
        verts = scene.deformedVertices;
    }

    RtVertex v0 = verts.v[tri.x];
    RtVertex v1 = verts.v[tri.y];
    RtVertex v2 = verts.v[tri.z];

    const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

    Instance inst = instances[h.instanceSlot];

    RayHit hit;
    // From the ray rather than from the vertices: `t` is exact and an interpolation of
    // three positions is not, and for the static tier the stored positions are in the
    // pre-transform space anyway.
    hit.position = rayOrigin + rayDir * rayQueryGetIntersectionTEXT(rq, true);
    hit.normal = normalize(instanceNormalMatrix(inst) * (w.x * v0.normal + w.y * v1.normal + w.z * v2.normal));
    hit.uv = w.x * v0.uv + w.y * v1.uv + w.z * v2.uv;
    hit.material = inst.meta.y;
    return hit;
}

vec4 rtSampleOr(int index, vec2 uv, vec4 fallback) {
    if (index < 0) return fallback;
    return textureLod(rtTextures[nonuniformEXT(index)], uv, 0.0);
}

/**
 * Full shading of a committed hit, for the reflection pass: the terms the deferred
 * lighting pass computes for a world pixel, evaluated at the hit point the same way.
 *
 * Unshadowed, and *unlike* the lighting pass, which traces a shadow ray per light. Every
 * light reaches every hit here. A shadow ray cast from a reflection hit is a second
 * bounce, and inline ray query gives no recursion to spend on one; it would also be a
 * ray per light on top of a pass already tracing one per pixel.
 *
 * So a reflected surface is lit as though nothing occluded it, and the reflection of a
 * shadowed floor carries no shadow. That disagreement between a surface and its own
 * reflection is known and accepted rather than pending -- it is in limitations.md, and
 * rayshadow.glsl argues the other side of it.
 *
 * Ambient is the same split-sum IBL lookup the lighting pass uses, times the baked
 * occlusion texture, exactly as the lighting pass applies orm.r.
 *
 * `textureLod(..., 0.0)` throughout: there are no screen-space derivatives on a ray
 * hit, and an implicit-LOD fetch here is undefined rather than merely blurry.
 */
vec3 shadeRayHit(rayQueryEXT rq, accelerationStructureEXT tlas, RayScene scene, vec3 rayOrigin, vec3 rayDir,
                 bool shadowed) {
    RayHit hit = resolveRayHit(rq, scene, rayOrigin, rayDir);
    Material m = rtMaterials[hit.material];

    vec4 base = m.baseColorFactor * rtSampleOr(m.baseColorTexture, hit.uv, vec4(1.0));
    vec4 mr = rtSampleOr(m.metallicRoughnessTexture, hit.uv, vec4(1.0));
    float roughness = clamp(m.roughnessFactor * mr.g, 0.04, 1.0);
    float metallic = clamp(m.metallicFactor * mr.b, 0.0, 1.0);

    vec3 diffuseColor = base.rgb * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), base.rgb, metallic);

    // Face the incoming ray, as in shadeHitLambert.
    vec3 N = hit.normal;
    vec3 V = -rayDir;
    if (dot(N, V) < 0.0) N = -N;

    // The same loop the lighting pass runs, down to the same `lightShadow` call. The sun
    // is lights[0], so one loop covers it -- there is no separate sun term to keep in
    // step. A shadow ray from here is a second ray, not a nested traversal: it opens its
    // own query from the hit point, so the lack of recursion in ray query costs nothing.
    vec3 color = vec3(0.0);
    int lightCount = int(frame.params.y);
    for (int i = 0; i < lightCount; ++i) {
        Light light = lights[i];

        vec3 Lp;
        vec3 radiance = lightRadiance(light, hit.position, N, Lp);
        if (dot(radiance, radiance) <= 0.0) continue;

        // Facing test before the shadow ray, for the reason lighting_body.glsl gives at
        // length: `lightRadiance` has no cosine term, so without this a hit facing away
        // from a light traces a ray and then multiplies the answer by `shadeLight`'s zero.
        // It matters more here than there -- every one of these is a traversal, and a
        // reflection pass traces them at hits the primary pass never visited.
        if (dot(N, Lp) <= 0.0) continue;

        // `N` here is the hit normal already flipped to face the incoming ray, which is
        // also what the shadow ray wants to be offset along -- a bias along a normal
        // pointing into the surface would start the ray inside it.
        if (shadowed && lightShadow(tlas, light, hit.position, N, Lp) <= 0.0) continue;

        color += shadeLight(N, V, Lp, radiance, diffuseColor, f0, roughness);
    }

    // The same flat ambient the primary path adds, so a reflected surface is lifted out
    // of black by exactly what the world one is. Only the baked occlusion texture is
    // available here -- SSAO is a screen-space buffer and a hit has no screen position --
    // so a reflected crease is fractionally brighter than the world crease beside it.
    // That is the one place the two ambients differ, and it is the smaller error by far
    // against feeding a hit some other pixel's occlusion.
    float bakedAO = rtSampleOr(m.occlusionTexture, hit.uv, vec4(1.0)).r;
    color += constantAmbient(diffuseColor, bakedAO);

    color += m.emissiveFactor.rgb * rtSampleOr(m.emissiveTexture, hit.uv, vec4(1.0)).rgb;
    return color;
}
