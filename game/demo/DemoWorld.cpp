#include "DemoWorld.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "core/Resources.h"
#include "gfx/Decal.h"
#include "gfx/Light.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace {

// ============================================================================= geometry

/**
 * @brief A unit cylinder about the origin: radius 0.5, height 1, `sides` around.
 *
 * Sized to a unit so that one mesh serves the four barrels and the banner's brass bar,
 * which is not a tidiness point but the thing that decides whether the world builds at
 * all: each `Engine::createMesh` drains the device, rebuilds every pipeline and rebuilds
 * the acceleration structure, so a mesh per prop would be a dozen of those at load.
 *
 * It served the brazier stems and bowls and two hundred and forty urns as well, until the
 * fire moved into the vessels Sponza already hangs and the urns turned out to be filler.
 *
 * The side walls carry one vertex per corner per face rather than a shared ring, because
 * a cylinder's normals are per face around the barrel and per cap at the ends -- the same
 * reason `unitCube` has twenty-four vertices and not eight.
 */
scene::MeshData unitCylinder(uint32_t material, uint32_t sides = 20) {
    scene::MeshData mesh;
    mesh.material = material;

    const float step = glm::two_pi<float>() / static_cast<float>(sides);
    // The side wall: a quad per segment, its two normals pointing out along the radius.
    for (uint32_t s = 0; s < sides; ++s) {
        const float a0 = static_cast<float>(s) * step;
        const float a1 = static_cast<float>(s + 1) * step;
        const glm::vec3 n0(std::cos(a0), 0.0f, std::sin(a0));
        const glm::vec3 n1(std::cos(a1), 0.0f, std::sin(a1));
        const glm::vec4 t0(-n0.z, 0.0f, n0.x, 1.0f);
        const glm::vec4 t1(-n1.z, 0.0f, n1.x, 1.0f);
        const float u0 = static_cast<float>(s) / static_cast<float>(sides);
        const float u1 = static_cast<float>(s + 1) / static_cast<float>(sides);

        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({n0 * 0.5f + glm::vec3(0.0f, -0.5f, 0.0f), n0, t0, {u0, 1.0f}});
        mesh.vertices.push_back({n1 * 0.5f + glm::vec3(0.0f, -0.5f, 0.0f), n1, t1, {u1, 1.0f}});
        mesh.vertices.push_back({n1 * 0.5f + glm::vec3(0.0f, 0.5f, 0.0f), n1, t1, {u1, 0.0f}});
        mesh.vertices.push_back({n0 * 0.5f + glm::vec3(0.0f, 0.5f, 0.0f), n0, t0, {u0, 0.0f}});
        // Counter-clockwise seen from *outside*, which for this vertex order is 0,2,1 and
        // not the 0,1,2 `unitCube` uses -- there the quad's second vertex runs along the
        // face, here it runs around the barrel, and the two are opposite hands. Wound the
        // other way the wall is inside out: culling drops the near shell and the camera
        // reads the far one, whose normals point away, so the prop goes dark rather than
        // missing.
        for (const uint32_t i : {0u, 2u, 1u, 0u, 3u, 2u}) mesh.indices.push_back(base + i);
    }

    // Two caps, as triangle fans about their own centre vertex.
    for (uint32_t cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? 0.5f : -0.5f;
        const glm::vec3 n(0.0f, cap == 0 ? 1.0f : -1.0f, 0.0f);
        const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
        const auto centre = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({{0.0f, y, 0.0f}, n, tangent, {0.5f, 0.5f}});
        for (uint32_t s = 0; s <= sides; ++s) {
            const float a = static_cast<float>(s) * step;
            const glm::vec3 p(std::cos(a) * 0.5f, y, std::sin(a) * 0.5f);
            mesh.vertices.push_back({p, n, tangent, {p.x + 0.5f, p.z + 0.5f}});
        }
        for (uint32_t s = 0; s < sides; ++s) {
            // Wound the other way on the underside, or the cap faces into the solid.
            if (cap == 0) {
                mesh.indices.push_back(centre);
                mesh.indices.push_back(centre + 1 + s + 1);
                mesh.indices.push_back(centre + 1 + s);
            } else {
                mesh.indices.push_back(centre);
                mesh.indices.push_back(centre + 1 + s);
                mesh.indices.push_back(centre + 1 + s + 1);
            }
        }
    }
    return mesh;
}

// ============================================================================ materials

/// One opaque PBR material with no textures on it. Six of the eleven fields are "there is
/// no texture here", spelled `-1` -- which is what a game writes when its content is
/// geometry rather than a glTF, and is the shape `GpuMaterial` was designed around.
uint32_t plainMaterial(Engine& e, const glm::vec3& color, float metallic, float roughness,
                       const glm::vec3& emissive = glm::vec3(0.0f)) {
    scene::GpuMaterial m{};
    m.baseColorFactor = glm::vec4(color, 1.0f);
    m.emissiveFactor = glm::vec4(emissive, 0.0f);
    m.metallicFactor = metallic;
    m.roughnessFactor = roughness;
    m.alphaCutoff = 0.5f;
    m.normalScale = 1.0f;
    m.baseColorTexture = -1;
    m.metallicRoughnessTexture = -1;
    m.normalTexture = -1;
    m.occlusionTexture = -1;
    m.emissiveTexture = -1;
    return e.gltfScene().createMaterial(m);
}

/**
 * @brief A hanging cloth with two morph targets, as `createMesh` wants it (G11).
 *
 * Local space: the bar is the line y = 0, the hem is y = -`height`, and the sheet lies in
 * the XY plane about x = 0. Both faces are emitted, because a banner seen from behind is a
 * banner seen from behind and the G-buffer culls back faces.
 *
 * ## Why two targets and not one
 *
 * Each target is a standing wave along the width, pinned at the bar and free at the hem,
 * and the second is the first shifted a quarter period. A single target can only make the
 * cloth breathe in and out of one fixed shape; two driven in quadrature -- `sin` and
 * `cos` of the same clock -- sum to a wave that *travels* along the banner, which is the
 * cheapest thing that cannot be faked by scaling one target and is therefore the honest
 * demonstration that a weighted sum of several is what the dispatch computes.
 *
 * Normal deltas are the first-order term of the displaced surface's normal, which is what
 * a glTF `NORMAL` target carries and is exact only at full weight. Tangents are left at
 * zero: the cloth has no normal map, so nothing reads them.
 */
scene::MeshData bannerCloth(uint32_t material, float width, float height, uint32_t cols, uint32_t rows) {
    scene::MeshData mesh;
    mesh.material = material;

    constexpr float kAmplitude = 0.26f;
    constexpr float kWaves = 1.5f; ///< periods across the width
    const float k = glm::two_pi<float>() * kWaves;

    mesh.morphTargets.assign(2, {});

    // Front (+Z) then back (-Z). One loop, two faces, because every quantity below --
    // position, uv, the wave and its two derivatives -- is the same for both and only the
    // sign of the normal and the winding differ.
    for (uint32_t face = 0; face < 2; ++face) {
        const float facing = face == 0 ? 1.0f : -1.0f;
        const auto base = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t r = 0; r <= rows; ++r) {
            const float v = static_cast<float>(r) / static_cast<float>(rows); ///< 0 at the bar
            for (uint32_t c = 0; c <= cols; ++c) {
                const float u = static_cast<float>(c) / static_cast<float>(cols);
                const glm::vec3 position((u - 0.5f) * width, -v * height, 0.0f);
                mesh.vertices.push_back({position,
                                         {0.0f, 0.0f, facing},
                                         {facing, 0.0f, 0.0f, 1.0f},
                                         {face == 0 ? u : 1.0f - u, v}});

                // sin for target 0, cos for target 1 -- one quarter period apart. `v`
                // scales both, which is what pins the cloth to its bar.
                const float phase = u * k;
                for (uint32_t t = 0; t < 2; ++t) {
                    const float wave = t == 0 ? std::sin(phase) : std::cos(phase);
                    const float slope = t == 0 ? std::cos(phase) : -std::sin(phase);
                    const float dz = kAmplitude * wave * v;
                    const float dzdx = kAmplitude * slope * (k / width) * v;
                    const float dzdy = -kAmplitude * wave / height;

                    scene::MorphDelta delta;
                    delta.position = {0.0f, 0.0f, dz};
                    delta.normal = facing * glm::vec3(-dzdx, -dzdy, 0.0f);
                    mesh.morphTargets[t].push_back(delta);
                }
            }
        }

        const uint32_t stride = cols + 1;
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < cols; ++c) {
                const uint32_t i0 = base + r * stride + c;
                const uint32_t quad[6] = {i0, i0 + stride, i0 + stride + 1, i0, i0 + stride + 1, i0 + 1};
                // The back face takes the same six in reverse, which is what makes its
                // winding face the way its normal points.
                for (uint32_t i = 0; i < 6; ++i) mesh.indices.push_back(quad[face == 0 ? i : 5 - i]);
            }
        }
    }

    // Stated rather than derived, because `createMesh` computes bounds from the *bind*
    // vertices and the cloth spends its life displaced off them by up to the amplitude.
    // A deformed instance is never frustum-culled, so this is what the spatial index and
    // the inspector report rather than what decides a draw -- but a box that does not
    // contain the object is wrong wherever it is read.
    mesh.localMin = {-width * 0.5f, -height, -kAmplitude * 2.0f};
    mesh.localMax = {width * 0.5f, 0.0f, kAmplitude * 2.0f};
    return mesh;
}

// ============================================================================= emitters


/**
 * @brief A seat inside one of Sponza's four hanging vessels, so a fire can burn *in* it.
 *
 * Fractions of the scene's bounding box rather than metres, because that is what survives
 * the world scale: the vessels are part of the building and the box scales with it.
 * Read off primitives 74, 76, 78 and 80 of `Sponza.gltf` -- the vessels themselves; 73, 75,
 * 77 and 79 are the chains they hang from.
 *
 * **Inside the bowl, not on its lip.** A vessel spans 0.79 to 1.10 in file y, which at the
 * showcase's scale of 2 is 1.58 to 2.20 m of world -- a bowl 0.62 m deep. This used to
 * return the lip exactly, and a particle is born *at* the emitter and rises: so every flame
 * started at the rim and climbed away from it, leaving the vessel's interior black and the
 * fire visibly floating above the thing it was meant to be burning in. 0.1594 is a quarter
 * of a metre down from the lip, which lights the bowl and still clears it by 0.45 m.
 *
 * Neither the top of the assembly nor the floor: above the vessel sits a hook at 1.74 that a
 * bounding box of the primitive would put the fire level with, and below it is 2.1 m of air
 * -- the earliest version stood the brazier on the floor, out in the arcades behind the
 * curtains at that.
 *
 * The pair along X is not symmetric about the centre of the box -- 0.6473 against 0.3498,
 * where symmetry wants 0.6501 -- so each side carries its own fraction rather than one
 * offset taken either way.
 */
glm::vec3 hangingPotAt(const glm::vec3& boundsMin, const glm::vec3& boundsMax, uint32_t index) {
    const glm::vec3 extent = boundsMax - boundsMin;
    const float x = boundsMin.x + extent.x * ((index & 1u) != 0u ? 0.64732f : 0.34985f);
    const float z = boundsMin.z + extent.z * ((index & 2u) != 0u ? 0.57950f : 0.42095f);
    return {x, boundsMin.y + extent.y * 0.15940f, z};
}

/// How far above the mouth a brazier's emitters spawn, as a fraction of the scene's height
/// so it travels with the vessel. `kSmokeLift` is the 0.42 m the flame was tuned against in
/// `fire.gltf`, expressed against Sponza's 24.9 m of height.
constexpr float kFireLift = 0.0f;
constexpr float kSmokeLift = 0.0169f;

/// A brazier's emitters, in the order `buildDemoWorld` pushes them.
///
/// **The lift is read twice** -- once to author the emitter's world transform and once as
/// the node offset that has to reproduce it, because the scene sweep writes `node.world`
/// straight into `emitter.transform`. A disagreement between the two is a fire that jumps
/// on the first frame, so both loops read this table rather than agreeing by hand. The code
/// this replaced indexed the lift by `emitterIndex % 3`, so adding a layer silently moved
/// the smoke onto the wrong one.
struct BrazierEmitter {
    const char* name;
    float lift;
};
constexpr BrazierEmitter kBrazierEmitters[] = {
    {"core", kFireLift}, {"flame", kFireLift}, {"smoke", kSmokeLift}, {"embers", kFireLift},
};

} // namespace

// =============================================================================== public

scene::MeshData unitCube(uint32_t material, const glm::mat4& transform) {
    scene::MeshData mesh;
    mesh.material = material;
    mesh.transform = transform;

    const glm::vec3 faces[6][4] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     // +Z
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}, // -Z
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},     // +X
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}, // -X
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},     // +Y
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}, // -Y
    };
    const glm::vec3 normals[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    const glm::vec4 tangents[6] = {{1, 0, 0, 1}, {-1, 0, 0, 1}, {0, 0, -1, 1},
                                   {0, 0, 1, 1}, {1, 0, 0, 1},  {1, 0, 0, 1}};
    const glm::vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

    for (uint32_t f = 0; f < 6; ++f) {
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t v = 0; v < 4; ++v) {
            mesh.vertices.push_back({faces[f][v] * 0.5f, normals[f], tangents[f], uvs[v]});
        }
        for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    }
    return mesh;
}


void buildDemoWorld(Engine& e, DemoWorld& world) {
    if (world.built) return;
    world.built = true;

    scene::Scene& tree = e.scene();
    const glm::vec3 boundsMin = e.gltfScene().boundsMin;
    const glm::vec3 boundsMax = e.gltfScene().boundsMax;
    const glm::vec3 centre = (boundsMin + boundsMax) * 0.5f;
    const glm::vec3 extent = boundsMax - boundsMin;
    // Sponza's floor stands a metre above the bottom of its bounding box, and that metre
    // is a measurement *of the building* -- so it grows with `--scene-scale` exactly as
    // the fractions of `extent` below do. The absolute offsets further down do not: a
    // crate is 0.62 m in a cathedral of any size. Getting this one wrong is what buries
    // every prop a metre into the ground at 2x.
    const float floorY = boundsMin.y + 1.0f * kDemoWorldScale;

    // ---------------------------------------------------------------------- materials
    const uint32_t brassMaterial = plainMaterial(e, {0.62f, 0.44f, 0.18f}, 1.0f, 0.34f);
    const uint32_t crateMaterial = plainMaterial(e, {0.44f, 0.30f, 0.16f}, 0.0f, 0.72f);
    const uint32_t barrelMaterial = plainMaterial(e, {0.28f, 0.20f, 0.13f}, 0.0f, 0.60f);
    const uint32_t stoneMaterial = plainMaterial(e, {0.55f, 0.52f, 0.47f}, 0.0f, 0.88f);
    const uint32_t steelMaterial = plainMaterial(e, {0.72f, 0.73f, 0.75f}, 1.0f, 0.22f);
    const uint32_t clothMaterial = plainMaterial(e, {0.52f, 0.09f, 0.11f}, 0.0f, 0.92f);

    // -------------------------------------------------------------------- the emitters
    //
    // Built and handed over *before* any node exists, because `Scene::attachEmitter` takes
    // an index into the list the particle system is holding -- so the list has to be the
    // final one before a node can name a row in it.
    //
    // **The pool is a consequence of these numbers rather than a choice.**
    // `requiredCapacity` is `ceil(rate x maxLifetime) + 1` summed over the emitters and
    // rounded up to a power of two. Four emitters a brazier come to about 130 particles
    // each and 520 over the four, which rounds to a capacity of 1 024 -- an eighth of what
    // the twelve hundred discs a brazier needed.
    // Nothing states a particle budget -- the pool sizes itself from the emitters and grows
    // if they outrun it (C40) -- and the check the card names is that `droppedSpawns()`
    // stays at zero.
    //
    // ------------------------------------------------------------------ why four
    //
    // **The flame is a stack, and it is a stack because colour has two stops.** A particle
    // lerps `colorStart` to `colorEnd` and nothing else, so no single emitter can run
    // white-hot to yellow to orange to red the way a flame does. Two overlapping emitters
    // with different lifetimes and different two-stop ramps do it between them: `core` is
    // the short white root that is actually burning, `flame` is the longer orange tongue
    // rising through it.
    //
    // **Every number here was tuned in `game/demo/assets/fire.gltf` and copied.** That
    // scene is one brazier in a dark room and exists precisely because a flame cannot be
    // judged at brazier distance in a lit Sponza. Change these and change that, or the next
    // person tunes against a scene that no longer shows what ships.
    //
    // **`coneAngle` is radians here and degrees in the file.** The glTF reader converts,
    // so a value moved between the two has to be converted by hand: 13 degrees is 0.227.
    //
    // Two traps worth stating, both learned by getting them wrong:
    //
    // - **Small and many is the wrong instinct.** These used to be twelve hundred 6 cm
    //   discs a brazier, on the theory that enough of them would blend into a volume. They
    //   do not: a disc has no interior, so the ones on the boundary stay visible as discs
    //   at any count. A hundred and ten *sheet* particles at half a metre read as fire and
    //   cost a tenth as much. See systems.md, "Particles: the sheet is the effect".
    // - **Sizes and speeds are world units and are deliberately not scaled by
    //   `kDemoWorldScale`.** Everything else placed in this function is multiplied by it; a
    //   flame is half a metre of burning gas whatever size the building is, and doubling it
    //   makes a bonfire in a soup bowl.
    // The sheets, and the only reason this import exists: `texture` is a *slot* in the
    // scene's bindless array, and only the append knows which slot each image got. A model
    // with no geometry, so nothing but the two images arrives.
    const scene::GltfScene::ModelId sheets = e.addModel(core::Resources("res:/sheets.gltf"));
    const uint32_t flameSheet = e.gltfScene().modelTextureSlot(sheets, 0);
    const uint32_t smokeSheet = e.gltfScene().modelTextureSlot(sheets, 1);

    std::vector<scene::ParticleEmitter> emitters;
    emitters.reserve(4 * std::size(kBrazierEmitters));
    for (uint32_t b = 0; b < 4; ++b) {
        const glm::vec3 at = hangingPotAt(boundsMin, boundsMax, b);
        const glm::mat4 fireAt = glm::translate(glm::mat4(1.0f), at + glm::vec3(0.0f, extent.y * kFireLift, 0.0f));

        // The white heart, just above the fuel. Short-lived and barely moving: this is the
        // part that is burning, not the part that has risen.
        scene::ParticleEmitter core;
        core.name = "brazier core";
        core.transform = fireAt;
        core.rate = 90.0f;
        core.lifetime = 0.42f;
        core.lifetimeJitter = 0.18f;
        core.velocity = {0.0f, 0.32f, 0.0f};
        core.speedJitter = 0.15f;
        core.coneAngle = 0.087f; ///< 5 degrees
        // Not a flat plane: every particle born in the same 2 cm slab reads as a bright
        // horizontal bar sitting on the vessel rather than as the root of a fire.
        core.boxExtent = {0.055f, 0.02f, 0.055f};
        core.gravity = {0.0f, 0.2f, 0.0f}; ///< hot gas rises; this is buoyancy, not gravity
        core.drag = 1.9f;
        core.colorStart = {1.0f, 0.88f, 0.6f, 1.0f};
        core.colorEnd = {1.0f, 0.55f, 0.16f, 0.0f};
        core.sizeStart = 0.1f;
        core.sizeEnd = 0.3f;
        core.texture = flameSheet;
        core.flipbookCols = 4;
        core.flipbookRows = 4;
        core.flipbookLoops = 0.5f;
        // Barely eroded. The source is continuous -- it is the tips above it that break up.
        core.erosion = 0.12f;
        // Radiance rather than albedo: a flame is a source, and lighting one costs a full
        // light loop per particle to arrive at a colour it already has.
        core.emissive = true;
        core.emissiveIntensity = 2.4f;
        emitters.push_back(core);

        // The body, and the layer that does the work. Lives twice as long as the core and
        // rises through it, so what is on screen is a stack of tongues at different ages.
        scene::ParticleEmitter flame;
        flame.name = "brazier flame";
        flame.transform = fireAt;
        flame.rate = 60.0f;
        flame.lifetime = 0.78f;
        flame.lifetimeJitter = 0.22f;
        flame.velocity = {0.0f, 0.6f, 0.0f};
        flame.speedJitter = 0.22f;
        flame.coneAngle = 0.192f; ///< 11 degrees
        flame.boxExtent = {0.07f, 0.02f, 0.07f};
        flame.gravity = {0.0f, 0.2f, 0.0f};
        flame.drag = 2.0f;
        flame.colorStart = {1.0f, 0.58f, 0.16f, 1.0f};
        flame.colorEnd = {0.75f, 0.13f, 0.02f, 0.0f};
        // **Growing, not narrowing, and this is what stops the base bouncing.** A
        // particle appears at `sizeStart` and at `colorStart` in one frame, so a big
        // `sizeStart` makes every birth a full-size pop at a random offset -- with a
        // population this small that is a root visibly jumping about, several times a
        // second. Born small it grows into the flame instead, and the taper comes from
        // `erosion` eating the older, larger ones.
        flame.sizeStart = 0.16f;
        flame.sizeEnd = 0.46f;
        flame.texture = flameSheet;
        flame.flipbookCols = 4;
        flame.flipbookRows = 4;
        flame.flipbookLoops = 0.4f;
        // Heavily eroded, and this is what a flame's tip actually is: the same tongue with
        // more of it burnt away, dissolving along its own filaments rather than dimming.
        flame.erosion = 0.62f;
        flame.emissive = true;
        flame.emissiveIntensity = 2.2f;
        emitters.push_back(flame);

        // Lifted clear of the flame, because smoke born inside the fire is lit by it and
        // reads as grey flame. Dark at birth and *lightening* as it expands: soot scatters
        // more as it cools and thins, and a plume that darkens looks like exhaust.
        scene::ParticleEmitter smoke;
        smoke.name = "brazier smoke";
        smoke.transform = glm::translate(glm::mat4(1.0f), at + glm::vec3(0.0f, extent.y * kSmokeLift, 0.0f));
        smoke.rate = 10.0f;
        smoke.lifetime = 4.2f;
        smoke.lifetimeJitter = 0.3f;
        smoke.velocity = {0.0f, 0.3f, 0.0f};
        smoke.speedJitter = 0.45f;
        smoke.coneAngle = 0.419f; ///< 24 degrees
        smoke.boxExtent = {0.075f, 0.02f, 0.075f};
        // A little sideways drift, so four braziers do not rise in four identical columns.
        smoke.gravity = {0.05f, 0.14f, 0.02f};
        smoke.drag = 0.9f;
        smoke.colorStart = {0.035f, 0.033f, 0.032f, 0.22f};
        smoke.colorEnd = {0.14f, 0.14f, 0.145f, 0.0f};
        smoke.sizeStart = 0.34f;
        smoke.sizeEnd = 1.3f;
        smoke.texture = smokeSheet;
        smoke.flipbookCols = 4;
        smoke.flipbookRows = 4;
        smoke.flipbookLoops = 0.25f;
        // **The only emitter that spins.** A billow tumbles; a flame licks upward, and
        // rotating one reads as a tumbling object rather than as burning gas.
        smoke.spin = 0.5f;
        smoke.erosion = 0.7f;
        emitters.push_back(smoke);

        // The one emitter with real gravity on it -- an ember is a solid that was thrown,
        // not a gas that rose -- and the one that reads the depth buffer: an ember that
        // bounces off the floor is what says the collision path runs outside
        // `particles.gltf`. **No sheet**: a spark is a point of light, and the procedural
        // disc is the right shape for one rather than a fallback it makes do with.
        scene::ParticleEmitter embers;
        embers.name = "brazier embers";
        embers.transform = fireAt;
        embers.rate = 9.0f;
        embers.lifetime = 1.8f;
        embers.lifetimeJitter = 0.35f;
        embers.velocity = {0.0f, 1.15f, 0.0f};
        embers.speedJitter = 0.6f;
        embers.coneAngle = 0.524f; ///< 30 degrees
        embers.boxExtent = {0.05f, 0.02f, 0.05f};
        embers.gravity = {0.0f, -1.5f, 0.0f};
        embers.drag = 0.4f;
        embers.colorStart = {1.0f, 0.55f, 0.12f, 1.0f};
        embers.colorEnd = {0.45f, 0.06f, 0.01f, 0.0f};
        embers.sizeStart = 0.014f;
        embers.sizeEnd = 0.005f;
        embers.emissive = true;
        embers.emissiveIntensity = 4.0f;
        embers.collides = true;
        embers.restitution = 0.25f;
        emitters.push_back(embers);
    }

    // Two calls, and the second is the one that is easy to forget: `setEmitters` resizes
    // the CPU pool and the renderer sizes its buffers from `capacity()` in `setParticles`,
    // so a game that called only the first would emit into a pool the GPU has no storage
    // for. `Engine::loadScene` makes the same pair; there is no single verb for it.
    e.particles().setEmitters(std::move(emitters), 0);
    e.renderer().setParticles(&e.particles());

    // ---------------------------------------------------------------- the fire's sound
    //
    // Four sources on one file. `impact.wav` took the decode side of the crossover as a
    // one-shot; this is the first *looping* source to take it, and four of them decoding
    // one file once between them is the per-file accounting `AudioEngine::decodedBytes`
    // documents and nothing had yet demonstrated.
    scene::AudioSourceDesc crackle;
    {
        const core::Resources file("res:/audio/fire_crackle.wav");
        if (file.found()) {
            crackle.name = "fire crackle";
            crackle.file = file.string();
            crackle.bus = "ambience";
            crackle.volume = 0.55f;
            crackle.spatial = true;
            crackle.loop = true;
            crackle.minDistance = 1.5f;
            crackle.maxDistance = 22.0f;
            crackle.load = scene::AudioLoad::Decode;
        }
    }

    // ------------------------------------------------------------------- the two meshes
    //
    // The cube's own placement is the ramp and the cylinder's is the first urn, so neither
    // `createMesh` leaves an instance nobody wanted -- there is no way to make a model
    // without one.
    const glm::vec3 rampCentre(centre.x + extent.x * 0.09f, floorY + 0.30f, centre.z - extent.z * 0.045f);
    const glm::mat4 rampTransform = glm::translate(glm::mat4(1.0f), rampCentre) *
                                    glm::rotate(glm::mat4(1.0f), glm::radians(-13.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                                    glm::scale(glm::mat4(1.0f), glm::vec3(4.2f, 0.24f, 2.2f));
    const scene::GltfScene::ModelId cubeModel = e.createMesh(unitCube(stoneMaterial, rampTransform));

    // `createMesh` makes a model *and* one instance, so the cylinder's inaugural instance
    // has to be something the world actually wants. It is the banner's brass bar: with the
    // urns gone the only other users are the four barrels, and a lone cylinder standing
    // where the first urn used to be is exactly the stray this avoids. `bannerTop` is
    // computed here rather than beside the banner for that reason alone.
    const glm::vec3 bannerTop(centre.x - extent.x * 0.055f, floorY + extent.y * 0.354f, centre.z + extent.z * 0.055f);
    constexpr float kBannerWidth = 2.6f;
    scene::MeshData barMesh = unitCylinder(brassMaterial);
    barMesh.transform = glm::translate(glm::mat4(1.0f), bannerTop) *
                        glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)) *
                        glm::scale(glm::mat4(1.0f), {0.07f, kBannerWidth + 0.3f, 0.07f});
    const scene::GltfScene::ModelId cylinderModel = e.createMesh(std::move(barMesh));

    if (cubeModel == scene::GltfScene::kNoModel || cylinderModel == scene::GltfScene::kNoModel) {
        core::Logger::warn(core::LogCategory::Scene, "demo world: the geometry buffers refused a mesh");
        return;
    }

    // ----------------------------------------------------------------- the banner (G11)
    //
    // The one thing in the demo whose *shape* is a function of the step rather than its
    // transform, and the whole of what a morphed mesh made in code costs a game: a
    // `MeshData` carrying two target arrays, one `createMesh`, and two `setMorphWeight`
    // calls per step in `stepDemoWorld`. No file, no rig, no clip.
    //
    // In the nave rather than in an arcade, and only just off the centre line: a bounding
    // box does not know where the room is, and everything past a narrow strip about the
    // centre of this one is behind a curtain.
    // Down the nave rather than beside the camera, and that is the second time this row's
    // placement had to be moved: `Camera::frameBounds` stands a quarter of the longest axis
    // back, which is x = -7.9 here, so the mirror-image position put a two-and-a-half metre
    // sheet three metres in front of the lens and filled a third of the frame with cloth.
    {
        scene::MeshData banner = bannerCloth(clothMaterial, kBannerWidth, 2.5f, 16, 12);
        banner.transform = glm::translate(glm::mat4(1.0f), bannerTop);
        const scene::GltfScene::ModelId bannerModel = e.createMesh(std::move(banner));
        if (bannerModel != scene::GltfScene::kNoModel) {
            // The handle the engine made when it saw the targets. A game never calls
            // `SceneAnimator::createMorphed` itself: the instance has to name the
            // character before the renderer sizes its weight buffer, and `createMesh` is
            // the only place that ordering is known.
            world.banner = e.morphCharacterOf(bannerModel);

        }
    }

    uint32_t crackles = 0;

    // --------------------------------------------------------------------- the braziers
    //
    // Seven nodes each, and the shape is the whole of what G3's attachment record is for:
    // one root the rest hang off, so that everything a brazier *is* -- two pieces of
    // geometry, a light, three emitters and a sound -- moves when the root does. Nothing
    // in the demo moves them; what is being shown is that the relationship is expressible.
    uint32_t emitterIndex = 0;
    for (uint32_t b = 0; b < 4; ++b) {
        const glm::vec3 at = hangingPotAt(boundsMin, boundsMax, b);
        char name[32];
        std::snprintf(name, sizeof(name), "brazier %u", b);
        const scene::NodeId root = tree.create(name, scene::NodeId{});
        tree.setLocalPosition(root, at);

        // **No geometry of its own any more.** This used to build a stem, a bowl and a bed
        // of coals out of three copies of the unit cylinder, because the fire was standing
        // in mid-nave with nothing under it. Sponza already hangs four vessels and the fire
        // belongs in those, so the brazier is now a light, three emitters and a sound at a
        // place the building already provides -- which is what the attachment record was
        // always for, and one fewer pile of primitives pretending to be furniture.

        // **A spot rather than a point, and the reason is a budget rather than a look.**
        // The punctual atlas is 24 layers; a point takes six and a spot takes one. The
        // showcase already spends 18 of them on the orb and the two fills the interior's
        // shadows come from, so four shadow-casting *points* would need 24 more, overflow
        // the atlas and evict exactly those fills -- and there is no `castsShadows` flag
        // to decline with, so an over-budget light is an error in the log rather than a
        // decision. Four spots take four layers, which fits at 22 of 24 with room to
        // spare. It is also the more honest light: a bowl occludes its own fire downward.
        //
        // Down in the bowl, with the fire, rather than the 1.15 m higher it sat when `at`
        // meant the foot of a brazier standing on the floor. A cone that starts above the
        // flame it is meant to be lights the ceiling and leaves the vessel dark -- and the
        // vessel being dark is exactly how the fire was caught floating above its lip.
        const auto lightIndex = static_cast<uint32_t>(e.renderer().lights.size());
        e.renderer().lights.push_back(gfx::makeSpotLight(at, {0.0f, 1.0f, 0.0f}, 9.0f, glm::radians(50.0f),
                                                         glm::radians(85.0f), {1.0f, 0.58f, 0.26f}, 14.0f));
        const scene::NodeId lightNode = tree.create("light", root);
        // **The rotation is load-bearing and the direction above is not.** A light on a
        // node is aimed by the node's -Z, glTF's forward, and the sweep overwrites
        // `direction` with it every frame -- so the `{0, 1, 0}` handed to `makeSpotLight`
        // survives exactly until the first `update()`. A quarter turn about X is what
        // actually points the cone up. Nothing says so at the call site of either.
        tree.setLocalRotation(lightNode, glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)));
        tree.attachLight(lightNode, lightIndex);

        for (const BrazierEmitter& which : kBrazierEmitters) {
            const scene::NodeId node = tree.create(which.name, root);
            // The emitters were authored at their world positions above, so the node's
            // local offset has to reproduce that or the first sweep would move them.
            tree.setLocalPosition(node, {0.0f, extent.y * which.lift, 0.0f});
            tree.attachEmitter(node, emitterIndex++);
        }

        if (!crackle.file.empty() && e.audio().active()) {
            scene::AudioSourceDesc one = crackle;
            one.transform = glm::translate(glm::mat4(1.0f), at);
            const scene::SoundId sound = e.audio().create(one);
            if (sound.valid()) {
                ++crackles;
                const scene::NodeId node = tree.create("crackle", root);
                tree.attachSound(node, sound);
            }
        }

        // A scorch mark under each. **Not `GameSetup::decals`**, which is the field the row
        // set out to use: those are applied to whatever scene loaded, and `configure` runs
        // before the engine knows which that is -- so a demo that filled them would put
        // four scorch marks into all eleven golden cases. `gfx::decalAt` is the door C3
        // built and it takes a position, which is what a game placing content actually has.
        //
        // Texture 0 with a near-black tint, because a decal's texture is a slot in the
        // *scene's* bindless array and a game has no way to put one there.
        // Sunk most of a metre so the 1.9 m box catches the floor and almost nothing of
        // the brazier standing on it: `decalAt` takes one size for the footprint *and* the
        // projection depth, so a mark big enough to read is also deep enough to paint what
        // is standing in it.
        e.renderer().decals.push_back(gfx::decalAt(at + glm::vec3(0.0f, -0.86f, 0.0f), {0.0f, 1.0f, 0.0f}, 1.9f, 0,
                                                   {0.06f, 0.045f, 0.04f, 0.55f}));
    }

    // ------------------------------------------------------------------------- the ramp
    //
    // `ColliderMotion::Static`, and the first of the three the tree had no caller for.
    // Shape stated rather than left at `Auto`: `Auto` on a static body resolves to a
    // triangle mesh built from the node's geometry, and a body created in code has no node
    // for the loader to have read geometry off.
    {
        scene::ColliderDesc desc;
        desc.name = "ramp";
        desc.shape = scene::ColliderShape::Box;
        desc.motion = scene::ColliderMotion::Static;
        desc.halfExtent = {2.1f, 0.12f, 1.1f};
        desc.friction = 0.8f;
        desc.transform = glm::translate(glm::mat4(1.0f), rampCentre) *
                         glm::rotate(glm::mat4(1.0f), glm::radians(-13.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        const scene::BodyId body = e.physics().createBody(desc);
        const scene::NodeId node = tree.create("ramp", scene::NodeId{});
        if (body.valid()) tree.attachBody(node, body);
        // The cube model's own placement *is* the ramp, and it already carries the scale in
        // its transform, so it is deliberately not attached to the node: a body pushes a
        // rigid transform down into whatever it drives, and the first sweep would flatten
        // the ramp back to a 1 m cube. Nothing moves it, so nothing needs the attachment.
    }

    // ------------------------------------------------------------------ the crate stack
    //
    // `ColliderMotion::Dynamic`, six of them, in a 3-2-1 pyramid on the floor beside the
    // ramp. Each is a node with a body and an instance on it, which is the pairing that
    // makes the solver's answer reach the renderer: the body pushes its transform up into
    // the node, and the instance below it follows.
    {
        const float side = 0.62f;
        // A quarter of a metre above where they rest, so the stack *lands* in the first
        // half second rather than starting settled. That is not decoration: the card's row
        // is "a crate landing plays a sound and throws dust", and a stack already at rest
        // fires no contact above the 1 m/s floor `playImpacts` screens at, so the path
        // would ship unexercised by anything but a keypress.
        const glm::vec3 base(centre.x - extent.x * 0.10f, floorY + side * 0.5f + 0.25f, centre.z + extent.z * 0.048f);
        uint32_t crate = 0;
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t col = 0; col + row < 3; ++col) {
                const glm::vec3 at = base + glm::vec3((static_cast<float>(col) + static_cast<float>(row) * 0.5f) *
                                                          side * 1.04f,
                                                      static_cast<float>(row) * side * 1.02f, 0.0f);
                const glm::mat4 transform =
                    glm::translate(glm::mat4(1.0f), at) * glm::scale(glm::mat4(1.0f), glm::vec3(side));

                scene::ColliderDesc desc;
                desc.name = "crate";
                desc.shape = scene::ColliderShape::Box;
                desc.motion = scene::ColliderMotion::Dynamic;
                desc.halfExtent = glm::vec3(side * 0.5f);
                desc.mass = 12.0f;
                desc.friction = 0.6f;
                desc.restitution = 0.1f;
                desc.transform = glm::translate(glm::mat4(1.0f), at);

                const scene::BodyId body = e.physics().createBody(desc);
                const scene::InstanceId instance = e.addInstance(cubeModel, crateMaterial, transform, scene::InstanceMotion::Dynamic);
                char crateName[24];
                std::snprintf(crateName, sizeof(crateName), "crate %u", crate++);
                const scene::NodeId node = tree.create(crateName, scene::NodeId{});
                tree.setLocalPosition(node, at);
                if (body.valid()) tree.attachBody(node, body);
                // The offset carries the scale, exactly as the loader's does for a mesh
                // authored bigger than its collider: a Jolt body has no scale, so the node's
                // transform is a rigid one and the size has to live on the attachment.
                if (instance.valid()) tree.attachInstance(node, instance, glm::scale(glm::mat4(1.0f), glm::vec3(side)));
            }
        }
    }

    // --------------------------------------------------------------------- the barrels
    {
        const glm::vec3 base(centre.x + extent.x * 0.23f, floorY + 0.46f, centre.z - extent.z * 0.050f);
        for (uint32_t i = 0; i < 4; ++i) {
            const glm::vec3 at = base + glm::vec3(static_cast<float>(i % 2) * 0.86f, 0.0f,
                                                  static_cast<float>(i / 2) * 0.86f);
            const glm::mat4 scale = glm::scale(glm::mat4(1.0f), {0.68f, 0.92f, 0.68f});

            scene::ColliderDesc desc;
            desc.name = "barrel";
            desc.shape = scene::ColliderShape::Cylinder;
            desc.motion = scene::ColliderMotion::Dynamic;
            desc.radius = 0.34f;
            desc.halfHeight = 0.46f;
            desc.mass = 18.0f;
            desc.friction = 0.5f;
            desc.transform = glm::translate(glm::mat4(1.0f), at);

            const scene::BodyId body = e.physics().createBody(desc);
            const scene::InstanceId instance =
                e.addInstance(cylinderModel, barrelMaterial, glm::translate(glm::mat4(1.0f), at) * scale,
                              scene::InstanceMotion::Dynamic);
            const scene::NodeId node = tree.create("barrel", scene::NodeId{});
            tree.setLocalPosition(node, at);
            if (body.valid()) tree.attachBody(node, body);
            if (instance.valid()) tree.attachInstance(node, instance, scale);
        }
    }

    // ------------------------------------------------------------- the sliding platform
    //
    // `ColliderMotion::Kinematic`, the third value and the one nothing in the tree had
    // ever built. It is driven from `stepDemoWorld` by `setBodyTransform`, which is the
    // only verb that moves one -- a kinematic body ignores forces by definition.
    {
        world.platformCentre = glm::vec3(centre.x - extent.x * 0.25f, floorY + 0.85f, centre.z);
        world.platformTravel = extent.z * 0.055f;
        const glm::vec3 scale(2.4f, 0.22f, 1.6f);

        scene::ColliderDesc desc;
        desc.name = "platform";
        desc.shape = scene::ColliderShape::Box;
        desc.motion = scene::ColliderMotion::Kinematic;
        desc.halfExtent = scale * 0.5f;
        desc.friction = 0.9f;
        desc.transform = glm::translate(glm::mat4(1.0f), world.platformCentre);

        world.platform = e.physics().createBody(desc);
        const scene::InstanceId instance =
            e.addInstance(cubeModel, steelMaterial,
                          glm::translate(glm::mat4(1.0f), world.platformCentre) * glm::scale(glm::mat4(1.0f), scale),
                          scene::InstanceMotion::Dynamic);
        world.platformNode = tree.create("platform", scene::NodeId{});
        tree.setLocalPosition(world.platformNode, world.platformCentre);
        if (world.platform.valid()) tree.attachBody(world.platformNode, world.platform);
        if (instance.valid()) tree.attachInstance(world.platformNode, instance, glm::scale(glm::mat4(1.0f), scale));
    }

    // The two hundred and forty urns that stood here are gone. They were copies of
    // `unitCylinder` with a flat brown material -- the code called them urns and nothing
    // about them was urn-shaped -- and they existed for one reason: to give GPU frustum
    // culling something to reject, because Sponza is one draw of one enormous mesh and the
    // cull dispatch had almost nothing to decide about it.
    //
    // That is a real thing to want and this was a bad way to get it. A synthetic pile of
    // primitives is not what the demo should be showing, and the honest version of the same
    // test is a scene with genuinely repeated architecture in it -- see the chore card that
    // retires `showcase.gltf`. `C` toggling culling now moves a smaller number, and that
    // number is the truth about this scene rather than about the filler in it.

    // ------------------------------------------------------------ what the composite held
    //
    // A mirror, a glowing orb, a floor to stand on, two sounds, two fill lights and a
    // character: everything `showcase.gltf` grafted onto Sponza at build time, placed here
    // instead. The generator is gone and so is the scene it wrote; what is below is the same
    // world composed by the game that wanted it.
    //
    // **Every position is scaled and none of them was in the file.** The composite authored
    // its coordinates inside the glTF, so the world scale carried them along with the
    // building for free. Placed from code they are absolute metres unless multiplied, and a
    // mirror at an unscaled 2.4 stands inside a wall of a building twice that size.
    const float sceneScale = kDemoWorldScale;
    // **Sponza's ground floor is at -0.02, not at 0, and not at `floorY` either.** It is one
    // flat quad -- primitive 46, seven vertices -- and the two centimetres are the whole of
    // `bug-the-character-stands-on-a-floor-two-centimetres-above-the-one-it-can-see`: the
    // collision box a character stands on and the height it spawns at have to agree with the
    // floor it can *see*, or it starts the scene falling and plays a two-second hard landing
    // with nobody touching a key. `floorY` above is a fraction of the bounding box, which is
    // right for a crate and wrong by four centimetres at `sceneScale` 2 for this.
    const float groundY = -0.02f * sceneScale;
    const glm::vec3 orbCentre(-2.6f * sceneScale, 1.9f * sceneScale, 0.0f);
    const glm::vec3 mirrorCentre(2.4f * sceneScale, 2.0f * sceneScale, 0.0f);
    {
        // **Imported, not generated.** `sphere.gltf` is one mirror-finish sphere and the
        // file carries the material -- fully metallic at roughness 0, the one material
        // where the reflection *is* the whole surface, and Sponza has no smooth face
        // anywhere for one to be seen in. A sphere built here instead would be a triangle
        // winding this game has to get right, and getting it wrong shows up as a black
        // disc that reads exactly like the reflection pass returning nothing.
        //
        // **These two radii are the exception to the rule above: they are *not* scaled.**
        // `appendModel` multiplies an import's geometry by the world scale before it applies
        // the caller's transform, so the file's radius-0.5 sphere is already radius
        // `0.5 * sceneScale` by the time this scale reaches it. A radius written in world
        // units here would carry `sceneScale` twice and the prop comes out double size --
        // which reads as a mirror that swallows the nave rather than as a scaling bug.
        // The translations *are* world units: `placeSceneData` applies them after.
        constexpr float kMirrorRadius = 1.1f;
        const scene::GltfScene::ModelId sphereModel =
            e.addModel(core::Resources("res:/sphere.gltf"),
                       glm::translate(glm::mat4(1.0f), mirrorCentre) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(kMirrorRadius * 2.0f)));

        if (sphereModel != scene::GltfScene::kNoModel) {
            // Emissive *and* dark. The emissive term dominates, so a bright base colour
            // under it only makes the orb read as a lit sphere rather than as a source.
            // The orb is the second copy of the same geometry, so it is the one that
            // names a material rather than taking the file's.
            const uint32_t orbMaterial = plainMaterial(e, {0.05f, 0.06f, 0.08f}, 0.0f, 0.4f,
                                                       glm::vec3(0.35f, 0.62f, 1.0f) * 8.0f);
            // Unscaled for the same reason: this instance reuses the primitive the import
            // left behind, which `scaleSceneData` has already been over.
            constexpr float kOrbRadius = 0.45f;
            e.addInstance(sphereModel, orbMaterial,
                          glm::translate(glm::mat4(1.0f), orbCentre) *
                              glm::scale(glm::mat4(1.0f), glm::vec3(kOrbRadius * 2.0f)),
                          scene::InstanceMotion::Static);
        }
    }

    // **The interior's whole light set, and the demo authors all of it.** The sun is the
    // engine's and arrives from `GameSetup`; everything else in here is these three points
    // plus the four brazier cones above. `DemoGame::init` skips its fallback placement for
    // this scene precisely so that this is the list rather than an addition to one -- see
    // the note on that branch for why a game that composes a world has to own its lighting.
    //
    // The two fills are the composite's, to the digit: it authored them inside
    // `showcase.gltf` at +/-7.0, 3.2, 0 with range 18, and a file's coordinates were carried
    // into world units by `sceneScale`. Placed from code they have to be multiplied here,
    // which is the rule the rest of this function follows -- but the *intensities* are not,
    // because a candela is a candela at any scale and the falloff over the longer distance
    // is what the composite got too.
    //
    // Warm and slightly different from each other. Two fills at one colour read as a single
    // light source smeared across the nave rather than as an interior lit from both ends.
    //
    // The orb light is the one that has to exist either way: an emissive surface that does
    // not illuminate is the classic thing to get wrong and the classic thing not to notice.
    // It casts no shadow, and not as an optimisation -- it sits at the centre of the sphere
    // that represents it, so every cube face would record that sphere at near-zero distance
    // and the light would illuminate nothing at all.
    //
    // **The atlas has room for exactly this and not much more.** It is 24 layers, a point
    // costs six and a spot one: three points is 18 and the four braziers take it to 22. A
    // fourth point does not fit, and the fallback set this replaces did not either -- three
    // points and a spot is 19 before a single brazier.
    //
    // **Appended, never inserted.** The braziers above have already handed
    // `Scene::attachLight` an index into this vector; inserting ahead of them renumbers
    // every one and each brazier's cone silently becomes somebody else's light.
    {
        std::vector<gfx::GpuLight>& lights = e.renderer().lights;
        lights.push_back(gfx::makePointLight(orbCentre, 14.0f * sceneScale, {0.35f, 0.62f, 1.0f}, 30.0f));

        // Handed to `DemoGame` rather than found by it: `E` reparents this light onto the
        // character, and the index it lands at is a fact about the order above.
        world.torchLight = static_cast<uint32_t>(lights.size());
        lights.push_back(gfx::makePointLight(glm::vec3(-7.0f, 3.2f, 0.0f) * sceneScale,
                                             18.0f * sceneScale, {1.0f, 0.72f, 0.42f}, 45.0f));
        lights.push_back(gfx::makePointLight(glm::vec3(7.0f, 3.2f, 0.0f) * sceneScale,
                                             18.0f * sceneScale, {1.0f, 0.85f, 0.65f}, 45.0f));
    }

    // The floor. Sponza declares no collider anywhere, so anything physical dropped into it
    // falls for ever -- and this box is what the crates, the barrels and the character all
    // land on. **Its top face is at `groundY` and it is a box rather than `Auto`**: `Auto` on
    // a static body resolves to a triangle mesh built from the node's geometry, and a body
    // made in code has no node whose geometry anyone read.
    {
        scene::ColliderDesc desc;
        desc.name = "ground";
        desc.shape = scene::ColliderShape::Box;
        desc.motion = scene::ColliderMotion::Static;
        desc.halfExtent = {20.0f * sceneScale, 0.5f * sceneScale, 12.0f * sceneScale};
        desc.friction = 0.7f;
        desc.transform = glm::translate(glm::mat4(1.0f),
                                        glm::vec3(0.0f, groundY - 0.5f * sceneScale, 0.0f));
        const scene::BodyId body = e.physics().createBody(desc);
        const scene::NodeId node = tree.create("ground", scene::NodeId{});
        if (body.valid()) tree.attachBody(node, body);
    }

    // Two sounds, and they are different on purpose. The crickets are a non-spatial bed on
    // the ambience bus, 8.9 minutes long, so the streaming side of the decode/stream
    // crossover carries it without anybody naming a path. The hum is spatial and lives on
    // the orb, so it pans as the camera orbits and ducks the bed under it.
    //
    // **`res:/`, not the bare relative path the composite's extras carried.** A path inside a
    // glTF is resolved against the document; a path a game hands to `AudioEngine::create` is
    // resolved against the working directory, so the literal that worked in the file finds
    // nothing from code. It is also what `scripts/manifest.py` scans for, so a bare path
    // ships a package with the sound missing and no error anywhere.
    if (e.audio().active()) {
        const core::Resources bedFile("res:/audio/atmosphere_crickets.wav");
        const core::Resources humFile("res:/audio/atmosphere_hum.wav");

        scene::AudioSourceDesc bed;
        bed.name = "ambience";
        bed.file = bedFile.string();
        bed.bus = "ambience";
        bed.volume = 0.45f;
        bed.spatial = false;
        bed.occlusion = false;
        if (bedFile.found()) e.audio().create(bed);

        scene::AudioSourceDesc hum;
        hum.name = "orb hum";
        hum.file = humFile.string();
        hum.bus = "sfx";
        hum.volume = 0.85f;
        hum.minDistance = 2.0f * sceneScale;
        hum.maxDistance = 40.0f * sceneScale;
        hum.transform = glm::translate(glm::mat4(1.0f), orbCentre);
        const scene::SoundId sound = humFile.found() ? e.audio().create(hum) : scene::SoundId{};
        if (sound.valid()) {
            const scene::NodeId node = tree.create("orb hum", scene::NodeId{});
            tree.setLocalPosition(node, orbCentre);
            tree.attachSound(node, sound);
        }
    }

    // ------------------------------------------------------------------- the character
    //
    // The rig itself, not the stage it is usually shown on: `character.gltf` is
    // `build_character`'s scene -- a floor, a back wall, a block and three lights around the
    // same Mixamo file -- and importing that into Sponza would drop a second room inside the
    // first. `Mana/Mana.gltf` is the rig alone.
    //
    // Sponza's atrium runs about 30 units along X against 18 along Z, and X is the axis
    // `Camera::frameBounds` aims the default camera down. The character therefore stands
    // *along* the walkway rather than beside it: z = 2.4 put the composite's first attempt
    // inside the arcade curtains where nothing could see it.
    {
        const glm::vec3 at(0.0f, groundY, 0.9f * sceneScale);
        // Facing -Z, which is glTF's forward and the direction `Camera::frameBounds` leaves
        // the camera looking from.
        // **Placed by the scale, sized without it.** `configure` doubles the building to buy
        // a 1.8 m character room to walk in, and the whole point of doing it that way is that
        // the character does not grow: the stride length, the jump arc and every collider
        // tuned against them stay as they were. So the position below is scaled and nothing
        // else here is -- scaling the rig too makes it 3.6 m tall, walking at 6.4 m/s and
        // jumping 3.7 m, which is what `scripts/locomotion.sh` reads back as eleven failures.
        const glm::mat4 place = glm::translate(glm::mat4(1.0f), at);
        const scene::GltfScene::ModelId rig = e.addModel(core::Resources("res:/Mana/Mana.gltf"), place);

        if (rig != scene::GltfScene::kNoModel) {
            // **The controller is the game's, because the file has none.** A rig fetched from
            // Mixamo carries no `substrate_collider`, so `Engine::addModel`'s collider walk
            // finds nothing to make a player out of -- which is the whole of G16: without a
            // way to say which controller is the player, WASD, the follow camera, the navmesh
            // follower and gait-from-speed are all switched off at once for a character the
            // game spawned. The capsule numbers are the composite's, unchanged: 2 x (0.6 +
            // 0.3) is the 1.8 m the rig is tall, and the offset raises the capsule to its own
            // centre because the node is at the character's feet.
            scene::ColliderDesc desc;
            desc.name = "player";
            desc.motion = scene::ColliderMotion::Character;
            desc.radius = 0.3f;
            desc.halfHeight = 0.6f;
            desc.offset = {0.0f, 0.9f, 0.0f};
            desc.moveSpeed = 3.2f;
            desc.jumpSpeed = 4.2f;
            desc.transform = glm::translate(glm::mat4(1.0f), at);
            const scene::PhysicsCharacterId controller = e.physics().createCharacter(desc);
            if (!controller.valid()) {
                core::Logger::error(core::LogCategory::Scene,
                                    "Demo world: the player's controller was refused -- no WASD, no follow camera");
            }

            if (controller.valid()) {
                const scene::NodeId node = tree.create("player", scene::NodeId{});
                tree.attachCharacter(node, controller);

                // The pose and the capsule, joined. The engine derives this pairing for a
                // scene that binds a skinned mesh to a collider node; nothing binds them
                // here, so the game says it -- `Engine::locomotion()` is the door C22 and
                // G15 left open for exactly a rig a game spawned itself. The rig's own
                // character is the one the merge appended, which is the last one.
                scene::AnimatorId pose;
                const uint32_t appended = e.animator().characterCount();
                if (appended > 0) {
                    pose = e.animator().characterAt(appended - 1);
                    e.locomotion().pair(controller, pose);
                }

                // **The game says who is playing, and this is the whole of G17.** The engine
                // used to latch the first `Character` collider it walked and call it the
                // player, which answered a scene that authors one and silently promoted an
                // NPC everywhere else. Nothing in the engine ever read the answer.
                world.players.push_back({controller, node, pose});

                // The mesh follows the capsule. Every instance the import made hangs off the
                // character's node, so the solver's answer reaches the renderer through the
                // same sweep every other body in this file uses.
                //
                // **Named `mesh`, and the name is load-bearing.** `Engine::addModel` calls
                // these nodes `mesh` when *it* wires a file's collider, and `DemoGame` finds
                // the parts it turns by that name -- `facingNodes` is every child of
                // `playerNode()` called `mesh`. Anything else and the list comes back empty,
                // nothing is rotated, and `scripts/locomotion.sh` reports `faced 0.00 of the
                // way it walked` on all three camera arms while every other number is right.
                for (const scene::InstanceId instance : e.instancesOf(rig)) {
                    const scene::NodeId meshNode = tree.create("mesh", node);
                    tree.attachInstance(meshNode, instance, glm::mat4(1.0f));
                }
            }
        }
    }

    // One re-hand at the end rather than one per prop. `Engine::createMesh` does it for the
    // instances *it* makes, and `InstanceTable::create` cannot -- it does not know a
    // renderer exists -- so a game creating instances of its own has to say when it has
    // finished. This is what resizes the indirect command buffer and rebuilds the
    // acceleration structure the traced shadows and reflections read.
    e.renderer().setInstances(&e.instances());

    // ------------------------------------------------------------------------- the dust
    //
    // The template a landing throws. `burst` makes it a one-shot: `spawnEffect` releases
    // the slot itself once the last particle has died, which is what lets an impact be
    // fired and forgotten.
    world.dust.name = "impact dust";
    world.dust.burst = 26;
    world.dust.lifetime = 0.8f;
    world.dust.lifetimeJitter = 0.35f;
    world.dust.velocity = {0.0f, 1.5f, 0.0f};
    world.dust.speedJitter = 0.6f;
    world.dust.coneAngle = 1.1f;
    world.dust.boxExtent = {0.06f, 0.02f, 0.06f};
    world.dust.gravity = {0.0f, -2.2f, 0.0f};
    world.dust.drag = 2.2f;
    world.dust.colorStart = {0.42f, 0.38f, 0.33f, 0.5f};
    world.dust.colorEnd = {0.46f, 0.43f, 0.39f, 0.0f};
    world.dust.sizeStart = 0.07f;
    world.dust.sizeEnd = 0.34f;
    world.dustReady = true;

    // Reported rather than asserted, because every one of these numbers is a budget the
    // row has to respect and none of them is checked by the golden set: the pool the
    // emitters sized, the sources four braziers decoded between them, the bodies the world
    // asked the solver for and the instances the culling dispatch now has to decide about.
    // The audio and physics summaries the engine prints are written at *load*, which is
    // before a game's `init` has created any of this.
    core::Logger::status(core::LogCategory::Scene,
                         "Demo world: 4 braziers (%zu emitters, pool %u, %u dropped), %u crackles, 6 crates, "
                         "4 barrels, a ramp, a kinematic platform and a %s banner -- %u bodies, "
                         "%u instances",
                         4 * std::size(kBrazierEmitters), e.particles().capacity(),
                         e.particles().droppedSpawns(), crackles,
                         world.banner.valid() ? "2-target morphed" : "MISSING", e.physics().bodyCount(),
                         e.instances().liveCount());
    core::Logger::status(core::LogCategory::Scene,
                         "Demo world: %zu player%s, %u animator characters, %u locomotion pairs",
                         world.players.size(), world.players.size() == 1 ? "" : "s",
                         e.animator().characterCount(), e.locomotion().pairCount());
}

void stepDemoWorld(Engine& e, DemoWorld& world, float step) {
    auto s = core::Profiler::scope("stepDemoWorld");
    if (!world.built) return;

    // A function of the accumulated step rather than of wall time, so `--locked` makes
    // frame 60 the same frame 60 -- the property every capture in this repository rests on.
    world.clock += step;

    if (world.platform.valid()) {
        glm::vec3 at = world.platformCentre;
        at.z += std::sin(world.clock * 0.55f) * world.platformTravel;

        // **The node, not the body.** `PhysicsWorld::setBodyTransform` is the verb that moves
        // a kinematic body and it is not the one to call here: the scene sweep already pushes
        // a node's world transform into any kinematic body attached to it, so writing the body
        // directly would have the node overwrite it on the same frame. Moving the node moves
        // the body *and* the mesh riding on it, which is the whole of what an attachment is.
        e.scene().setLocalPosition(world.platformNode, at);
    }

    // ----------------------------------------------------------------- the banner (G11)
    //
    // Quadrature: the two standing waves the mesh carries, weighted by sine and cosine of
    // one clock, sum to a wave that travels along the cloth. Weights outside 0..1 are legal
    // and are used here -- a morph weight is a coefficient, not a blend factor, and the
    // negative half of each cycle is the same wave the other way up.
    if (world.banner.valid()) {
        const float phase = world.clock * 1.6f;
        e.animator().setMorphWeight(world.banner, 0, std::sin(phase));
        e.animator().setMorphWeight(world.banner, 1, std::cos(phase));
    }
}
