#include "scene/SceneParse.h"

#include "scene/SceneData.h"

#include "scene/Cloth.h"

#include "core/Json.h"
#include "core/Logger.h"
#include "core/Profiler.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

/**
 * @file engine/scene/SceneParse.cpp
 * @brief The CPU half of a scene load, in a translation unit that names no Vulkan (D9).
 *
 * This was the top two thirds of `GltfScene.cpp`, and the split is what lets a baker exist
 * without a GPU. C10 had already separated the *function* -- `loadCpu` touches no device,
 * which is what makes it the half that can run on a worker thread -- but it shared a file
 * with `upload`, so nothing could link the parse without linking the device sources,
 * `volk`, and a window.
 *
 * The boundary this restores is the one the engine already checks everywhere else: a
 * dependency crossing it is a link error rather than a code review. `GltfScene.cpp` keeps
 * every line that ever took a `VkDevice`, and everything here is hosted -- which is why
 * `substrate-bake` can run in a container with no driver at all.
 */
namespace scene {

namespace {

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

glm::mat4 nodeTransform(const fastgltf::Node& node) {
    return std::visit(fastgltf::visitor{
                          [](const fastgltf::math::fmat4x4& m) { return glm::make_mat4(m.data()); },
                          [](const fastgltf::TRS& trs) {
                              const glm::vec3 t(trs.translation[0], trs.translation[1], trs.translation[2]);
                              // glTF stores quaternions xyzw; glm::quat is wxyz.
                              const glm::quat r(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
                              const glm::vec3 s(trs.scale[0], trs.scale[1], trs.scale[2]);
                              return glm::translate(glm::mat4(1.0f), t) * glm::toMat4(r) *
                                     glm::scale(glm::mat4(1.0f), s);
                          },
                      },
                      node.transform);
}

/**
 * @brief One KHR_lights_punctual light, placed by its node's world transform.
 *
 * Two conventions worth stating because getting either wrong is invisible until a
 * scene actually ships a light:
 *
 * - glTF lights point down **local -Z**, so the direction is that axis carried
 *   through the node transform. The position is the transform's translation.
 * - Intensity is photometric: lux for directional, candela for point and spot. It is
 *   passed through unscaled, which is the honest thing to do -- the engine's exposure
 *   is itself arbitrary, so `render.exposure` is where an imported scene gets
 *   balanced, not a fudge factor buried here.
 *
 * Range absent means infinite in glTF, and 0 means unbounded in `GpuLight::position.w`
 * -- the same thing said two ways. The cone defaults are the specification's.
 */
gfx::GpuLight toGpuLight(const fastgltf::Light& light, const glm::mat4& world) {
    const glm::vec3 position(world[3]);
    const glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const glm::vec3 color(light.color[0], light.color[1], light.color[2]);
    const float intensity = static_cast<float>(light.intensity);
    const float range = light.range.has_value() ? static_cast<float>(*light.range) : 0.0f;

    switch (light.type) {
    // The one negation the two conventions cost, in one place: glTF aims every light
    // down local -Z, and makeDirectionalLight wants the vector pointing at the light.
    case fastgltf::LightType::Directional: return gfx::makeDirectionalLight(-direction, color, intensity);
    case fastgltf::LightType::Spot:
        return gfx::makeSpotLight(position, direction, range,
                                  light.innerConeAngle.has_value() ? static_cast<float>(*light.innerConeAngle) : 0.0f,
                                  light.outerConeAngle.has_value() ? static_cast<float>(*light.outerConeAngle)
                                                                   : glm::quarter_pi<float>(),
                                  color, intensity);
    case fastgltf::LightType::Point:
    default: return gfx::makePointLight(position, range, color, intensity);
    }
}

/**
 * @brief Resolve a material texture slot to an image index, or -1 if absent.
 *
 * Templated on the whole optional rather than its element type: fastgltf::Optional
 * is an alias template, which is a non-deducible context.
 */
template <typename OptionalTextureInfo>
int32_t textureIndexOf(const fastgltf::Asset& asset, const OptionalTextureInfo& info) {
    if (!info.has_value()) return -1;
    const auto& tex = asset.textures[info->textureIndex];
    if (!tex.imageIndex.has_value()) return -1;
    return static_cast<int32_t>(*tex.imageIndex);
}

/// Lift an embedded payload out of the document, so the decode above needs no `asset`.
/// Copied rather than referenced: the parsed asset is destroyed with `parseSceneData`,
/// and the decode fan-out runs after it.
std::vector<uint8_t> embeddedBytes(const fastgltf::Asset& asset, const fastgltf::Image& image) {
    std::vector<uint8_t> out;
    std::visit(fastgltf::visitor{
                   [&](const fastgltf::sources::Array& array) {
                       const auto* p = reinterpret_cast<const uint8_t*>(array.bytes.data());
                       out.assign(p, p + array.bytes.size());
                   },
                   [&](const fastgltf::sources::BufferView& view) {
                       const auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                       const auto& buffer = asset.buffers[bufferView.bufferIndex];
                       std::visit(fastgltf::visitor{
                                      [&](const fastgltf::sources::Array& array) {
                                          const auto* p = reinterpret_cast<const uint8_t*>(array.bytes.data()) +
                                                          bufferView.byteOffset;
                                          out.assign(p, p + bufferView.byteLength);
                                      },
                                      [](const auto&) {},
                                  },
                                  buffer.data);
                   },
                   [](const auto&) {},
               },
               image.data);
    return out;
}

} // namespace

/**
 * @brief Everything a scene load derives from the document, with no device involved (C15).
 *
 * The function the sidecar exists to skip. It is `GltfScene::load` minus decode, upload and
 * descriptors -- which is to say minus every line that ever took a `VkDevice` -- and the
 * split falls where it does because that is where the file already put it: the parse,
 * geometry, flatten and animation passes name no Vulkan type at all, and `materials` names
 * one only in the sense that `textureIndexOf` returns a glTF image index the bindless array
 * happens to be keyed by.
 *
 * Reordered by exactly one block: `materials` used to sit *after* the texture pass, which
 * read as a dependency and was not one. Moving it up is what makes this contiguous.
 */
bool parseSceneData(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded) {

    // ------------------------------------------------------------------- parse
    auto parseStart = std::chrono::steady_clock::now();
    fastgltf::Asset asset;
    /// Emitters as authored, keyed by node index and not yet placed. Filled from the
    /// same bytes fastgltf is about to parse, and consumed by the node walk below --
    /// which is where a light is placed too, and for the same reason (S3.1).
    std::vector<ParticleEmitter> emitterTemplates;
    /// Colliders as authored, keyed by node index and not yet placed or given geometry.
    /// Same pass, same reasons, same walk (S4.2).
    std::vector<ColliderDesc> colliderTemplates;
    /// And once more for sound (S5.2). Three schemas through one scan is what put
    /// `gltfJsonSpan` in core/Json.h rather than a fourth copy of it here.
    std::vector<AudioSourceDesc> audioTemplates;
    {
        auto s = core::Profiler::scope("GltfScene::parse");

        auto mmapStart = std::chrono::steady_clock::now();
        auto mapped = fastgltf::MappedGltfFile::FromPath(path);
        data.stats.mmapMs = msSince(mmapStart);
        if (!bool(mapped)) {
            core::Logger::error(core::LogCategory::GLTF, "Failed to mmap %s: %s", path.string().c_str(),
                          fastgltf::getErrorMessage(mapped.error()).data());
            return false;
        }

        constexpr auto extensions = fastgltf::Extensions::KHR_materials_emissive_strength |
                                    fastgltf::Extensions::KHR_texture_transform |
                                    fastgltf::Extensions::KHR_lights_punctual;
        fastgltf::Parser parser(extensions);

        constexpr auto options = fastgltf::Options::LoadExternalBuffers | fastgltf::Options::GenerateMeshIndices;

        // Before loadGltf, because the getter's read cursor is where it is now and
        // loadGltf will move it. See parseSceneEmitters for why the extras are read by
        // a second, targeted pass rather than through fastgltf's simdjson callback.
        {
            auto s2 = core::Profiler::scope("GltfScene::extras");
            auto extrasStart = std::chrono::steady_clock::now();
            const auto bytes = static_cast<fastgltf::span<std::byte>>(mapped.get());

            // **One parse, three readers** (C14). Each of these used to build a complete
            // copying rapidjson::Document over the whole file to read one key out of
            // `nodes[].extras`, and C13 measured what three of those cost: about three
            // quarters of a large scene's entire load.
            core::json::GltfDocument document;
            if (!document.parse(bytes.data(), bytes.size_bytes())) {
                core::Logger::warn(core::LogCategory::GLTF, "%s: could not scan for scene extras", path.string().c_str());
            } else if (document.nodes != nullptr) {
                // A glTF with no `nodes` array is valid and simply has nothing to place,
                // which is why that is not a warning.
                if (!parseSceneEmitters(*document.nodes, emitterTemplates)) {
                    core::Logger::warn(core::LogCategory::GLTF, "%s: could not scan for particle emitters",
                                 path.string().c_str());
                }
                if (!parseSceneColliders(*document.nodes, colliderTemplates)) {
                    core::Logger::warn(core::LogCategory::GLTF, "%s: could not scan for colliders", path.string().c_str());
                }
                if (!parseSceneAudioSources(*document.nodes, audioTemplates)) {
                    core::Logger::warn(core::LogCategory::GLTF, "%s: could not scan for audio sources",
                                 path.string().c_str());
                }
            }
            data.stats.extrasMs = msSince(extrasStart);
        }

        auto gltfStart = std::chrono::steady_clock::now();
        auto s3 = core::Profiler::scope("GltfScene::loadGltf");
        auto loaded = parser.loadGltf(mapped.get(), path.parent_path(), options);
        data.stats.gltfMs = msSince(gltfStart);
        if (!bool(loaded)) {
            core::Logger::error(core::LogCategory::GLTF, "Failed to parse %s: %s", path.string().c_str(),
                          fastgltf::getErrorMessage(loaded.error()).data());
            return false;
        }
        asset = std::move(loaded.get());
    }
    data.stats.parseMs = msSince(parseStart);

    // ---------------------------------------------------------------- geometry
    auto geomStart = std::chrono::steady_clock::now();
    std::vector<uint32_t> meshFirstPrim(asset.meshes.size(), 0);
    std::vector<uint32_t> meshPrimCount(asset.meshes.size(), 0);
    std::vector<uint32_t> meshMorphTargets(asset.meshes.size(), 0);

    {
        auto s = core::Profiler::scope("GltfScene::geometry");

        // A single rough reserve beats repeated reallocation across ~100 primitives.
        data.vertices.reserve(1u << 18);
        data.indices.reserve(1u << 19);

        for (size_t meshIdx = 0; meshIdx < asset.meshes.size(); ++meshIdx) {
            const auto& mesh = asset.meshes[meshIdx];
            meshFirstPrim[meshIdx] = static_cast<uint32_t>(data.primitives.size());
            // Hoisted out of the primitive loop because the name is the mesh's, while the
            // cloth it makes is the primitive's -- see the block below.
            const bool fabric = isFabricMesh(std::string_view(mesh.name.data(), mesh.name.size()));

            for (const auto& primitive : mesh.primitives) {
                if (primitive.type != fastgltf::PrimitiveType::Triangles) continue;

                const auto* positionAttr = primitive.findAttribute("POSITION");
                if (positionAttr == primitive.attributes.end()) continue;
                if (!primitive.indicesAccessor.has_value()) continue;

                const auto& posAccessor = asset.accessors[positionAttr->accessorIndex];
                const uint32_t baseVertex = static_cast<uint32_t>(data.vertices.size());
                const size_t vertexStart = data.vertices.size();
                data.vertices.resize(vertexStart + posAccessor.count);

                glm::vec3 localMin(std::numeric_limits<float>::max());
                glm::vec3 localMax(std::numeric_limits<float>::lowest());

                fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor, [&](glm::vec3 v, size_t i) {
                    data.vertices[vertexStart + i].position = v;
                    localMin = glm::min(localMin, v);
                    localMax = glm::max(localMax, v);
                });

                if (const auto* it = primitive.findAttribute("NORMAL"); it != primitive.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(
                        asset, asset.accessors[it->accessorIndex],
                        [&](glm::vec3 v, size_t i) { data.vertices[vertexStart + i].normal = v; });
                }

                // Sponza ships MikkTSpace tangents, so none need generating here. What
                // does need handling is their *absence*: a value-initialised Vertex
                // leaves the tangent at zero, and `normalize(vec3(0))` is a NaN -- which
                // skinning.comp writes into the deformed vertex buffer and the G-buffer
                // then shades from. A Mixamo export is the case that found this: it
                // carries POSITION, NORMAL and TEXCOORD_0 and no tangent at all.
                //
                // The fallback is an arbitrary vector perpendicular to the normal, not a
                // reconstructed one. That is exactly right for a material with no normal
                // map, which is what a mesh without tangents almost always is, and it is
                // wrong in a stated way for one that has both -- the glTF spec says a
                // client *should* compute MikkTSpace tangents there, and doing it here
                // would be an hour of code for a case no asset in this repository has.
                const auto* tangentAttr = primitive.findAttribute("TANGENT");
                if (tangentAttr != primitive.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(
                        asset, asset.accessors[tangentAttr->accessorIndex],
                        [&](glm::vec4 v, size_t i) { data.vertices[vertexStart + i].tangent = v; });
                } else {
                    for (size_t i = 0; i < posAccessor.count; ++i) {
                        Vertex& v = data.vertices[vertexStart + i];
                        const glm::vec3 n =
                            glm::length(v.normal) > 0.0f ? glm::normalize(v.normal) : glm::vec3(0.0f, 1.0f, 0.0f);
                        // Cross with whichever axis the normal is least aligned to, so
                        // the result is never degenerate.
                        const glm::vec3 axis =
                            std::abs(n.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                        v.tangent = glm::vec4(glm::normalize(glm::cross(axis, n)), 1.0f);
                    }
                }

                if (const auto* it = primitive.findAttribute("TEXCOORD_0"); it != primitive.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(
                        asset, asset.accessors[it->accessorIndex],
                        [&](glm::vec2 v, size_t i) { data.vertices[vertexStart + i].uv = v; });
                }

                Primitive p;
                p.firstIndex = static_cast<uint32_t>(data.indices.size());
                p.baseVertex = baseVertex;
                p.vertexCount = static_cast<uint32_t>(posAccessor.count);
                p.materialIndex = primitive.materialIndex.has_value()
                                      ? static_cast<int32_t>(*primitive.materialIndex)
                                      : -1;

                // Skinning influences (4.4). Held in their own array rather than on
                // Vertex, so a file with no skin pays nothing. A primitive with JOINTS_0
                // but no WEIGHTS_0 is malformed; taking the joints anyway would give
                // every vertex a zero-weight rig and collapse the mesh to the origin, so
                // both have to be present.
                const auto* jointsAttr = primitive.findAttribute("JOINTS_0");
                const auto* weightsAttr = primitive.findAttribute("WEIGHTS_0");
                if (jointsAttr != primitive.attributes.end() && weightsAttr != primitive.attributes.end()) {
                    p.skinOffset = static_cast<uint32_t>(data.skinVertices.size());
                    data.skinVertices.resize(data.skinVertices.size() + posAccessor.count);
                    const size_t skinStart = p.skinOffset;

                    fastgltf::iterateAccessorWithIndex<glm::uvec4>(
                        asset, asset.accessors[jointsAttr->accessorIndex],
                        [&](glm::uvec4 v, size_t i) { data.skinVertices[skinStart + i].joints = v; });
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(
                        asset, asset.accessors[weightsAttr->accessorIndex],
                        [&](glm::vec4 v, size_t i) { data.skinVertices[skinStart + i].weights = v; });

                    data.stats.skinnedVertices += static_cast<uint32_t>(posAccessor.count);
                }

                // Morph targets (S2.1). Target-major -- every displacement of target 0,
                // then every displacement of target 1 -- because that is the order the
                // weighted sum walks, and the alternative interleaves a stride of
                // `targets` into a loop that is already reading one vertex.
                //
                // A target may declare any subset of POSITION, NORMAL and TANGENT. The
                // absent ones stay zero, which is the identity for a displacement, so
                // there is no per-target mask for a shader to test.
                if (!primitive.targets.empty()) {
                    p.morphTargets = static_cast<uint32_t>(primitive.targets.size());
                    p.morphOffset = static_cast<uint32_t>(data.morphDeltas.size());
                    data.morphDeltas.resize(data.morphDeltas.size() +
                                     static_cast<size_t>(p.morphTargets) * posAccessor.count);

                    for (size_t t = 0; t < primitive.targets.size(); ++t) {
                        const size_t base = p.morphOffset + t * posAccessor.count;
                        const auto end = primitive.targets[t].cend();

                        if (const auto it = primitive.findTargetAttribute(t, "POSITION"); it != end) {
                            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                                asset, asset.accessors[it->accessorIndex],
                                [&](glm::vec3 v, size_t i) { data.morphDeltas[base + i].position = v; });
                        }
                        if (const auto it = primitive.findTargetAttribute(t, "NORMAL"); it != end) {
                            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                                asset, asset.accessors[it->accessorIndex],
                                [&](glm::vec3 v, size_t i) { data.morphDeltas[base + i].normal = v; });
                        }
                        // A tangent *displacement* is VEC3, not VEC4: the handedness in
                        // w is a property of the surface, not something a target moves.
                        if (const auto it = primitive.findTargetAttribute(t, "TANGENT"); it != end) {
                            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                                asset, asset.accessors[it->accessorIndex],
                                [&](glm::vec3 v, size_t i) { data.morphDeltas[base + i].tangent = v; });
                        }
                    }

                    data.stats.morphTargets += p.morphTargets;
                    data.stats.morphedVertices += static_cast<uint32_t>(posAccessor.count);
                }

                /*
                 * Cloth (C19). The **one** name-driven dispatch in the loader, and the only
                 * authoring convention in this engine that is not a glTF `extras` key --
                 * because `extras` is a per-node dictionary and has nowhere at all to put a
                 * value per vertex, which is what a pin weight is.
                 *
                 * `isFabricMesh` is the single predicate and `Cloth.h` is the single place
                 * it is spelled; `tests/ClothTests.cpp` calls that one and not a copy, which
                 * is the whole of what this row set out to take from Tethered by not taking
                 * it. The mesh's name rather than the node's, deliberately: glTF's
                 * `mesh.name` is Blender's data-block, and `scripts/check_pins.py` gives the
                 * object-renamed-but-not-the-data-block trap its own refusal for exactly
                 * this reason.
                 *
                 * Per primitive rather than per mesh, because Blender splits a mesh by
                 * material and a curtain wearing two is two soft bodies -- so the array is
                 * sized and the offset written inside this loop, not outside it.
                 *
                 * A `FABRIC_` mesh whose `_PIN_WEIGHT` is missing gets an array of ones,
                 * which is a cloth pinned nowhere; that is refused with a reason by
                 * `ClothSystem::add` rather than here, so the refusal is in one place for
                 * both the missing-attribute case and the nothing-above-threshold case.
                 */
                if (fabric) {
                    p.clothOffset = static_cast<uint32_t>(data.clothVertices.size());
                    data.clothVertices.resize(data.clothVertices.size() + posAccessor.count);
                    const size_t clothStart = p.clothOffset;

                    if (const auto* it = primitive.findAttribute(kPinAttribute);
                        it != primitive.attributes.end()) {
                        fastgltf::iterateAccessorWithIndex<float>(
                            asset, asset.accessors[it->accessorIndex], [&](float w, size_t i) {
                                data.clothVertices[clothStart + i].invMass = clothInvMass(w);
                            });
                    }

                    data.stats.clothPrimitives++;
                    data.stats.clothVertices += static_cast<uint32_t>(posAccessor.count);
                }

                const auto& idxAccessor = asset.accessors[*primitive.indicesAccessor];
                data.indices.reserve(data.indices.size() + idxAccessor.count);
                fastgltf::iterateAccessor<uint32_t>(asset, idxAccessor,
                                                    [&](uint32_t i) { data.indices.push_back(baseVertex + i); });

                p.indexCount = static_cast<uint32_t>(data.indices.size()) - p.firstIndex;
                p.localMin = localMin;
                p.localMax = localMax;
                // The spec requires every primitive of a mesh to declare the same
                // number of targets, so the mesh's count is the first primitive's --
                // and it is the mesh's, not the primitive's, that a node's weight array
                // is sized from.
                if (meshPrimCount[meshIdx] == 0) meshMorphTargets[meshIdx] = p.morphTargets;
                data.primitives.push_back(p);
                meshPrimCount[meshIdx]++;
            }
        }
    }

    // -------------------------------------------------- flatten node hierarchy
    data.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    data.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    {
        auto s = core::Profiler::scope("GltfScene::flatten");

        // Retain the hierarchy as well as flattening it (4.4). The flattened world
        // transforms are what the instance table wants; the parent links and local TRS
        // are what an animation drives, and a joint's world transform is the product
        // down its parent chain, which a flat list cannot express.
        std::vector<SceneNode>& sceneNodes = data.rig.bind.nodes;
        sceneNodes.resize(asset.nodes.size());
        // Parallel to the nodes, and kept for `SceneAnimator::findNode` -- the only way a
        // game can name the joint that carries root motion. See `AnimationRig::nodeNames`.
        data.rig.nodeNames.resize(asset.nodes.size());
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            SceneNode& n = sceneNodes[i];
            data.rig.nodeNames[i] = std::string(asset.nodes[i].name);
            std::visit(fastgltf::visitor{
                           [&](const fastgltf::math::fmat4x4& m) {
                               // A node may give a matrix instead of TRS. Decomposing it
                               // is the only way an animation channel can drive one
                               // component of it, and a file that ships a matrix ships no
                               // channel for that node -- so this is the bind pose and
                               // the decomposition never has to round-trip.
                               const glm::mat4 g = glm::make_mat4(m.data());
                               n.translation = glm::vec3(g[3]);
                               glm::vec3 c0(g[0]), c1(g[1]), c2(g[2]);
                               n.scale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
                               if (n.scale.x > 0.0f) c0 /= n.scale.x;
                               if (n.scale.y > 0.0f) c1 /= n.scale.y;
                               if (n.scale.z > 0.0f) c2 /= n.scale.z;
                               n.rotation = glm::quat_cast(glm::mat3(c0, c1, c2));
                           },
                           [&](const fastgltf::TRS& trs) {
                               n.translation = glm::vec3(trs.translation[0], trs.translation[1], trs.translation[2]);
                               n.rotation = glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1],
                                                      trs.rotation[2]);
                               n.scale = glm::vec3(trs.scale[0], trs.scale[1], trs.scale[2]);
                           },
                       },
                       asset.nodes[i].transform);
        }
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            for (size_t child : asset.nodes[i].children) {
                if (child < sceneNodes.size()) sceneNodes[child].parent = static_cast<int32_t>(i);
            }
        }

        // Morph weights are per *node*, not per mesh (S2.1): the same morphed mesh may
        // be placed twice with different expressions, and glTF says so by letting a
        // node override the mesh's default weights. So every node placing a morphed
        // mesh gets its own run in one flat array, and `firstWeight` is where it starts.
        //
        // Defaults come from the node, then the mesh, then zero -- the spec's order.
        // Zero is the un-morphed shape, which is what a file that declares neither means.
        //
        // The node half is unreachable today and is written anyway. fastgltf rejects the
        // *whole file* when any node carries `weights` -- external/fastgltf/src/fastgltf.cpp inverts
        // test on the parse result, so the success case returns InvalidGltf -- which
        // means `node.weights` is empty whenever the file loaded at all. Deleting the
        // branch would be deleting the correct behaviour to match a bug; leaving it is
        // one comparison that starts working the day the submodule moves. The symptom
        // to recognise is a file that morphs fine in every other viewer and fails here
        // with "missing something or has invalid data" before a single mesh is read.
        std::vector<float>& defaultWeights = data.rig.bind.weights;
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            const auto& node = asset.nodes[i];
            if (!node.meshIndex.has_value()) continue;
            const uint32_t targets = meshMorphTargets[*node.meshIndex];
            if (targets == 0) continue;

            sceneNodes[i].firstWeight = static_cast<uint32_t>(defaultWeights.size());
            sceneNodes[i].weightCount = targets;

            const auto& fromNode = node.weights;
            const auto& fromMesh = asset.meshes[*node.meshIndex].weights;
            for (uint32_t t = 0; t < targets; ++t) {
                float w = 0.0f;
                if (t < fromNode.size()) {
                    w = static_cast<float>(fromNode[t]);
                } else if (t < fromMesh.size()) {
                    w = static_cast<float>(fromMesh[t]);
                }
                defaultWeights.push_back(w);
            }
        }

        // Collision authored by node name rather than by extras -- see `isColliderNode`.
        // Synthesised into the same template list the extras pass filled, *before* the
        // walk below, so a suffixed node gets the geometry copy, the world transform and
        // the placement-suppression from code that already exists rather than a second
        // path kept in step by inspection.
        //
        // An explicit `substrate_collider` on the same node wins and the suffix adds
        // nothing: the two would otherwise produce two bodies for one node, and the one
        // that names its own shape and motion is the one the author meant.
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            if (!isColliderNode(asset.nodes[i].name)) continue;
            bool declared = false;
            for (const ColliderDesc& tmpl : colliderTemplates) {
                if (tmpl.node == static_cast<uint32_t>(i)) declared = true;
            }
            if (declared) continue;

            // Everything else is left at its default, and that is the whole point of the
            // convention: `Auto` + `Static` resolves to an exact triangle mesh, which is
            // what a floor and a wall want and what `Engine::navMesh` bakes from.
            ColliderDesc c;
            c.node = static_cast<uint32_t>(i);
            c.name = std::string(asset.nodes[i].name);
            if (c.name.empty()) c.name = "node " + std::to_string(i);
            colliderTemplates.push_back(std::move(c));
        }

        // Iterative rather than recursive: node graphs are untrusted input and a
        // deep or cyclic one should not blow the stack.
        struct Pending {
            size_t node;
            glm::mat4 parent;
            /// Nearest ancestor that declared a collider, inherited downward. See
            /// `Placement::colliderNode`.
            uint32_t collider;
        };
        std::vector<Pending> stack;
        std::vector<bool> visited(asset.nodes.size(), false);

        const auto& scene = asset.scenes[asset.defaultScene.value_or(0)];
        for (size_t root : scene.nodeIndices) stack.push_back({root, glm::mat4(1.0f), 0xFFFFFFFFu});

        while (!stack.empty()) {
            const Pending item = stack.back();
            stack.pop_back();

            if (item.node >= asset.nodes.size() || visited[item.node]) continue;
            visited[item.node] = true;
            data.stats.nodes++;

            const auto& node = asset.nodes[item.node];
            const glm::mat4 world = item.parent * nodeTransform(node);

            // A collider on this node claims everything below it. Nearest wins, so a
            // prop with its own collider parented to a character keeps its own.
            uint32_t collider = item.collider;
            for (const ColliderDesc& tmpl : colliderTemplates) {
                if (tmpl.node == static_cast<uint32_t>(item.node)) collider = tmpl.node;
            }

            // A collision node is not drawn, and this is the only place that can say so:
            // an instance exists exactly where a placement did, so the suppression has to
            // happen before one is made. Clearing a flag later would not do -- the
            // instance table has no such flag, and six later walks re-derive slot numbers
            // from this list's order and would all have to learn the same exception.
            if (node.meshIndex.has_value() && !isColliderNode(node.name)) {
                const size_t mesh = *node.meshIndex;
                const uint32_t skin = node.skinIndex.has_value() ? static_cast<uint32_t>(*node.skinIndex)
                                                                 : 0xFFFFFFFFu;
                for (uint32_t i = 0; i < meshPrimCount[mesh]; ++i) {
                    // A skinned mesh's node transform is *not* applied: the joint
                    // matrices already take a vertex from bind pose to model space, and
                    // multiplying by the node's own transform as well would apply the
                    // skeleton root twice. The glTF spec says so explicitly, and the
                    // symptom of getting it wrong is a character at double distance from
                    // the origin, moving at double speed.
                    data.placements.push_back(
                        {meshFirstPrim[mesh] + i, skin == 0xFFFFFFFFu ? world : glm::mat4(1.0f),
                         static_cast<uint32_t>(item.node), skin, collider});
                }
            }

            // A light is placed by its node, so it is found here rather than in a
            // separate pass over asset.lights: the same light may be instanced under
            // several nodes, and only the node knows where each copy is.
            if (node.lightIndex.has_value() && *node.lightIndex < asset.lights.size()) {
                // `nodes[i].extras.substrate_light` was applied here rather than in
                // `toGpuLight`, which is handed a `fastgltf::Light` and has no idea which
                // node placed it. It carried one boolean, `castsShadows`, and went with
                // the shadow system -- see `gfx::Light.h`. If it returns it belongs here
                // again, for the reason it was here: the override belongs to the node for
                // the same reason the light's position does, since the same light under
                // two nodes is two lights and only one may be the one inside a lamp.
                data.lights.push_back(toGpuLight(asset.lights[*node.lightIndex], world));
            }

            // An emitter is placed by its node for exactly the reason a light is, and
            // the linear scan is right for exactly the same reason it would be wrong
            // for meshes: a scene has a handful of emitters, not a hundred thousand
            // (S3.1).
            for (const ParticleEmitter& tmpl : emitterTemplates) {
                if (tmpl.node != static_cast<uint32_t>(item.node)) continue;
                ParticleEmitter placed = tmpl;
                placed.transform = world;
                data.emitters.push_back(std::move(placed));
            }

            // And a collider likewise (S4.2) -- except that a collider may also need the
            // node's *geometry*, which is why this one is more than three lines. The
            // vertices are copied in node space: the body carries the node's translation
            // and rotation, and its scale reaches the shape, so a shape built from
            // already-transformed vertices would apply the placement twice.
            for (const ColliderDesc& tmpl : colliderTemplates) {
                if (tmpl.node != static_cast<uint32_t>(item.node)) continue;
                ColliderDesc placed = tmpl;
                placed.transform = world;

                if (placed.needsGeometry()) {
                    if (!node.meshIndex.has_value()) {
                        core::Logger::warn(core::LogCategory::GLTF,
                                     "Collider '%s': shape '%s' is built from the node's mesh, and node %zu has none "
                                     "-- skipped",
                                     placed.name.c_str(), colliderShapeName(placed.resolvedShape()), item.node);
                        continue;
                    }
                    const size_t mesh = *node.meshIndex;
                    for (uint32_t i = 0; i < meshPrimCount[mesh]; ++i) {
                        const Primitive& p = data.primitives[meshFirstPrim[mesh] + i];
                        // Each primitive's vertices are appended, so its indices have to
                        // be rebased twice: off the shared buffer's `baseVertex` and onto
                        // wherever this collider's copy of them started. The same
                        // rebasing S2.5's dynamic BLAS needs, and for the same reason --
                        // neither a shape build nor an acceleration-structure build has
                        // an indirect command's signed vertex offset to do it for free.
                        const auto localBase = static_cast<uint32_t>(placed.points.size());
                        for (uint32_t v = 0; v < p.vertexCount; ++v) {
                            placed.points.push_back(data.vertices[p.baseVertex + v].position);
                        }
                        if (placed.resolvedShape() == ColliderShape::Mesh) {
                            for (uint32_t k = 0; k < p.indexCount; ++k) {
                                placed.indices.push_back(data.indices[p.firstIndex + k] - p.baseVertex + localBase);
                            }
                        }
                    }
                }

                data.colliders.push_back(std::move(placed));
            }

            // And a sound, which is the simplest of the three: it has a position and no
            // geometry (S5.2). The path is resolved here because this is the only place
            // that knows where the .gltf came from -- a source authored as `"hum.wav"`
            // means the file next to the scene, and only falls back to the working
            // directory when there is nothing there.
            for (const AudioSourceDesc& tmpl : audioTemplates) {
                if (tmpl.node != static_cast<uint32_t>(item.node)) continue;
                AudioSourceDesc placed = tmpl;
                placed.transform = world;
                if (const std::filesystem::path beside = path.parent_path() / placed.file;
                    std::filesystem::exists(beside)) {
                    placed.file = beside.string();
                }
                data.audioSources.push_back(std::move(placed));
            }

            for (size_t child : node.children) stack.push_back({child, world, collider});
        }

        // Accumulate bounds in world space by transforming each primitive's eight
        // AABB corners. Using raw vertex positions would ignore node scaling.
        for (const auto& draw : data.placements) {
            const Primitive& p = data.primitives[draw.primitive];
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 local((corner & 1) ? p.localMax.x : p.localMin.x,
                                      (corner & 2) ? p.localMax.y : p.localMin.y,
                                      (corner & 4) ? p.localMax.z : p.localMin.z);
                const glm::vec3 world = glm::vec3(draw.transform * glm::vec4(local, 1.0f));
                data.boundsMin = glm::min(data.boundsMin, world);
                data.boundsMax = glm::max(data.boundsMax, world);
            }
        }

        if (data.placements.empty()) {
            data.boundsMin = glm::vec3(-1.0f);
            data.boundsMax = glm::vec3(1.0f);
        }
    }

    // ------------------------------------------------------- skins and clips (4.4)
    {
        auto s = core::Profiler::scope("GltfScene::animation");

        std::vector<Skin>& sceneSkins = data.rig.skins;
        std::vector<AnimationClip>& sceneClips = data.rig.clips;

        sceneSkins.reserve(asset.skins.size());
        for (const auto& skin : asset.skins) {
            Skin out;
            out.joints.reserve(skin.joints.size());
            for (size_t j : skin.joints) out.joints.push_back(static_cast<uint32_t>(j));

            // An absent inverseBindMatrices accessor means identity for every joint,
            // per the spec. Filling them rather than branching later keeps the joint
            // matrix loop free of a null test it would run per joint per frame.
            out.inverseBind.assign(out.joints.size(), glm::mat4(1.0f));
            if (skin.inverseBindMatrices.has_value()) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
                    asset, asset.accessors[*skin.inverseBindMatrices],
                    [&](const fastgltf::math::fmat4x4& m, size_t i) {
                        if (i < out.inverseBind.size()) out.inverseBind[i] = glm::make_mat4(m.data());
                    });
            }
            sceneSkins.push_back(std::move(out));
        }

        sceneClips.reserve(asset.animations.size());
        for (const auto& anim : asset.animations) {
            AnimationClip clip;
            clip.name = std::string(anim.name);

            clip.samplers.reserve(anim.samplers.size());
            for (const auto& sampler : anim.samplers) {
                AnimationSampler out;
                switch (sampler.interpolation) {
                case fastgltf::AnimationInterpolation::Step:
                    out.interpolation = AnimationInterpolation::Step;
                    break;
                case fastgltf::AnimationInterpolation::CubicSpline:
                    out.interpolation = AnimationInterpolation::CubicSpline;
                    break;
                default: out.interpolation = AnimationInterpolation::Linear; break;
                }

                const auto& input = asset.accessors[sampler.inputAccessor];
                out.times.reserve(input.count);
                fastgltf::iterateAccessor<float>(asset, input, [&](float t) { out.times.push_back(t); });

                // Rotations arrive as VEC4 and translations and scales as VEC3. Both are
                // widened to vec4 here so the sampler has one array type; the w a
                // translation does not use costs four bytes per key.
                //
                // A SCALAR output is the third case and can only be morph weights: no
                // TRS path produces one. That is what identifies a weights sampler here,
                // ahead of the channel that will point at it, and it is why the stride
                // is *derived* -- output.count is keys times targets (times three for
                // CUBICSPLINE), so the targets fall out of a division rather than out of
                // a lookup through a channel this loop has not read yet.
                const auto& output = asset.accessors[sampler.outputAccessor];
                if (output.type == fastgltf::AccessorType::Scalar) {
                    out.weights.reserve(output.count);
                    fastgltf::iterateAccessor<float>(asset, output,
                                                     [&](float w) { out.weights.push_back(w); });
                    const size_t groups =
                        out.times.size() * (out.interpolation == AnimationInterpolation::CubicSpline ? 3u : 1u);
                    out.stride = groups > 0 ? static_cast<uint32_t>(output.count / groups) : 0u;
                } else if (output.type == fastgltf::AccessorType::Vec4) {
                    out.values.reserve(output.count);
                    fastgltf::iterateAccessor<glm::vec4>(asset, output,
                                                         [&](glm::vec4 v) { out.values.push_back(v); });
                } else {
                    out.values.reserve(output.count);
                    fastgltf::iterateAccessor<glm::vec3>(
                        asset, output, [&](glm::vec3 v) { out.values.push_back(glm::vec4(v, 0.0f)); });
                }

                if (!out.times.empty()) clip.duration = std::max(clip.duration, out.times.back());
                clip.samplers.push_back(std::move(out));
            }

            clip.channels.reserve(anim.channels.size());
            for (const auto& ch : anim.channels) {
                if (!ch.nodeIndex.has_value()) continue;
                AnimationChannel out;
                out.node = static_cast<uint32_t>(*ch.nodeIndex);
                out.sampler = static_cast<uint32_t>(ch.samplerIndex);
                switch (ch.path) {
                case fastgltf::AnimationPath::Rotation: out.path = AnimationPath::Rotation; break;
                case fastgltf::AnimationPath::Scale: out.path = AnimationPath::Scale; break;
                case fastgltf::AnimationPath::Translation: out.path = AnimationPath::Translation; break;
                case fastgltf::AnimationPath::Weights: out.path = AnimationPath::Weights; break;
                default: continue;
                }
                clip.channels.push_back(out);
            }

            sceneClips.push_back(std::move(clip));
        }

        data.stats.skins = static_cast<uint32_t>(sceneSkins.size());
        data.stats.animations = static_cast<uint32_t>(sceneClips.size());
    }
    data.stats.meshes = static_cast<uint32_t>(asset.meshes.size());
    data.stats.geometryMs = msSince(geomStart);

    // --------------------------------------------------------------- materials
    data.materials.reserve(asset.materials.size() + 1);

    for (const auto& mat : asset.materials) {
        GpuMaterial m{};
        m.baseColorFactor = glm::vec4(mat.pbrData.baseColorFactor[0], mat.pbrData.baseColorFactor[1],
                                      mat.pbrData.baseColorFactor[2], mat.pbrData.baseColorFactor[3]);
        // Folded in here rather than carried to the shader, because every consumer wants
        // the product and none wants the two apart: gbuffer.frag writes it to an HDR
        // attachment and shadeRayHit adds it at a hit, and a second material field would
        // be a second thing for those two to forget to multiply.
        //
        // KHR_materials_emissive_strength was in the extension list from the start and
        // read by nobody, which is the worst of the three possible states -- a file
        // declaring `emissiveStrength: 8.0` loaded without complaint and rendered at 1.0.
        // The showcase orb is exactly that file, and it is why the orb did not bloom: its
        // factor peaks at 1.0 against a bloom threshold of 1.1, so the only thing reaching
        // the chain was the ~10% the soft knee passes below the cutoff. `emissiveFactor`
        // is capped at 1.0 by the spec, so without this extension no glTF material can be
        // emissive enough to bloom at all -- which is the entire reason the extension
        // exists.
        const float emissiveStrength = mat.emissiveStrength;
        m.emissiveFactor = glm::vec4(mat.emissiveFactor[0] * emissiveStrength, mat.emissiveFactor[1] * emissiveStrength,
                                     mat.emissiveFactor[2] * emissiveStrength, 0.0f);
        m.metallicFactor = mat.pbrData.metallicFactor;
        m.roughnessFactor = mat.pbrData.roughnessFactor;
        m.alphaCutoff = mat.alphaCutoff;
        m.normalScale = mat.normalTexture.has_value() ? mat.normalTexture->scale : 1.0f;

        m.baseColorTexture = textureIndexOf(asset, mat.pbrData.baseColorTexture);
        m.metallicRoughnessTexture = textureIndexOf(asset, mat.pbrData.metallicRoughnessTexture);
        m.normalTexture = textureIndexOf(asset, mat.normalTexture);
        m.occlusionTexture = textureIndexOf(asset, mat.occlusionTexture);
        m.emissiveTexture = textureIndexOf(asset, mat.emissiveTexture);
        m.alphaMask = mat.alphaMode == fastgltf::AlphaMode::Mask ? 1u : 0u;

        if (m.alphaMask != 0u) data.stats.alphaMaskedMaterials++;
        data.materials.push_back(m);
    }

    // Classify each primitive by alpha mode. BLEND cannot go through the G-buffer at
    // all -- one surface per pixel is stored and blending needs several -- so those
    // primitives are drawn forward after lighting. This is a load-time property of the
    // material, so it is decided once here rather than tested per frame in the record
    // loop; it used to produce a second draw list and is now a flag the instance table
    // carries, which is the same decision made once instead of twice.
    //
    // MASK is resolved in the same loop and for a different consumer: the shadow pass
    // splits its draws on it so that everything opaque can go through a pipeline with no
    // fragment shader at all. Two tables rather than one enum because the two answers are
    // read by different passes and a primitive is neither, one, or the other -- never both.
    {
        std::vector<bool> blendMaterial(data.materials.size(), false);
        for (size_t i = 0; i < asset.materials.size(); ++i) {
            if (asset.materials[i].alphaMode == fastgltf::AlphaMode::Blend) {
                blendMaterial[i] = true;
                data.stats.blendedMaterials++;
            }
        }

        for (Primitive& p : data.primitives) {
            const bool hasMaterial = p.materialIndex >= 0 && static_cast<size_t>(p.materialIndex) < blendMaterial.size();
            p.blended = hasMaterial && blendMaterial[p.materialIndex];
            // Read back out of `data.materials` rather than built into a second table
            // beside `blendMaterial`: `m.alphaMask` was already decided above from the
            // same `alphaMode`, and a parallel table is one more thing that can come to
            // disagree with the value the shader actually reads.
            p.masked = hasMaterial && data.materials[p.materialIndex].alphaMask != 0u;
        }
        for (const auto& draw : data.placements) {
            if (data.primitives[draw.primitive].blended) data.stats.blendedDraws++;
        }
    }

    // Fallback for primitives with no material at all.
    if (data.materials.empty()) {
        GpuMaterial fallback{};
        fallback.baseColorFactor = glm::vec4(1.0f);
        fallback.roughnessFactor = 1.0f;
        fallback.baseColorTexture = -1;
        fallback.metallicRoughnessTexture = -1;
        fallback.normalTexture = -1;
        fallback.occlusionTexture = -1;
        fallback.emissiveTexture = -1;
        data.materials.push_back(fallback);
    }

    // The image list, which is the one part of the texture pass that is not device work
    // and therefore the one part that has to travel in the sidecar. Which slot an image is
    // used in decides its format: colour data is authored in sRGB and must be decoded on
    // read, and a normal or ORM map would be corrupted by that decode. Getting it wrong is
    // silent, which is why the answer is carried rather than recomputed on the far side.
    data.images.resize(asset.images.size());
    embedded.resize(asset.images.size());
    for (size_t i = 0; i < asset.images.size(); ++i) {
        std::visit(fastgltf::visitor{
                       [&](const fastgltf::sources::URI& uri) { data.images[i].uri = std::string(uri.uri.path()); },
                       [](const auto&) {},
                   },
                   asset.images[i].data);
        // Lifted out of the document here, because the document is destroyed with this
        // function and the decode fan-out runs after it. Never written to the sidecar --
        // see `EmbeddedImages`.
        if (data.images[i].uri.empty()) embedded[i] = embeddedBytes(asset, asset.images[i]);
    }
    for (const auto& mat : asset.materials) {
        const int32_t base = textureIndexOf(asset, mat.pbrData.baseColorTexture);
        if (base >= 0) data.images[static_cast<size_t>(base)].srgb = true;
        const int32_t emissive = textureIndexOf(asset, mat.emissiveTexture);
        if (emissive >= 0) data.images[static_cast<size_t>(emissive)].srgb = true;
    }

    return true;
}

bool loadSceneCpu(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded) {
    // C15. The sidecar or the document, and no caller can tell which: everything downstream
    // reads `data`, and `data` is the same either way. A cache that does not apply --
    // absent, stale, a different build -- is not an error and not logged, because falling
    // back to the document is the correct behaviour in every one of those cases and a load
    // that complained would teach people to bake one to silence it.
    //
    // Split out of `load` for C10: this half touches no device and no window, which is what
    // makes it the half that can run on a worker thread while the frame loop keeps going.
    // Everything below `load`'s call to this needs a queue and must not.
    //
    // There is no third branch, and D9 is why. This used to take a `bakeCache` flag and,
    // when it was set, build C17's LOD chains and write the sidecar -- so the process that
    // read a cache was also the process that could write one. Baking is `substrate-bake`'s
    // now; a load from a document carries no levels and selection resolves to LOD 0 for
    // everything, exactly as it did before C17.
    const auto cacheStart = std::chrono::steady_clock::now();
    const bool fromCache = readSceneCache(path, data);
    if (!fromCache) {
        if (!parseSceneData(path, data, embedded)) return false;
    } else {
        // A cached load has no embedded payloads and needs none: every embedded image in
        // a scene that was baked has a `.ktx2` beside it, which is the condition
        // `writeSceneCache` refuses to bake without. Sized so the decode below indexes it
        // without a branch.
        embedded.resize(data.images.size());
        // The baked run's own parse and geometry numbers came along in the payload, and
        // reporting them for a run that did neither would be a lie the log tells every
        // time the cache works. Replaced by what this run actually spent.
        data.stats.parseMs = 0.0;
        data.stats.geometryMs = 0.0;
        data.stats.mmapMs = 0.0;
        data.stats.extrasMs = 0.0;
        data.stats.gltfMs = 0.0;
        data.stats.cacheMs = msSince(cacheStart);
        core::Logger::status(core::LogCategory::GLTF, "scene cache hit: %s (%.1f ms)",
                             sceneCachePath(path).filename().string().c_str(), data.stats.cacheMs);
    }
    return true;
}

} // namespace scene
