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
 * @brief The CPU half of a scene load, in a translation unit that names no Vulkan.
 *
 * Everything here is hosted, which is what lets `substrate-bake` run in a container with no
 * driver at all. A line that takes a `VkDevice` belongs in `GltfScene.cpp`; putting one here
 * pulls the device sources, `volk` and a window in behind anything that wants only the parse.
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
 * glTF aims a light down **local -Z**, so the direction is that axis carried through the
 * node transform and the position is the transform's translation. Intensity is photometric
 * -- lux for directional, candela for point and spot -- and passes through unscaled, so
 * `render.exposure` is where an imported scene gets balanced and not a factor buried here.
 *
 * An absent range means infinite in glTF and 0 means unbounded in `GpuLight::position.w`.
 * The cone defaults are the specification's.
 */
gfx::GpuLight toGpuLight(const fastgltf::Light& light, const glm::mat4& world) {
    const glm::vec3 position(world[3]);
    const glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const glm::vec3 color(light.color[0], light.color[1], light.color[2]);
    const float intensity = static_cast<float>(light.intensity);
    const float range = light.range.has_value() ? static_cast<float>(*light.range) : 0.0f;

    switch (light.type) {
    // The one negation the two conventions cost: glTF aims a light down local -Z, and
    // `makeDirectionalLight` wants the vector pointing *at* the light.
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

/// Lift an embedded payload out of the document. Copied, not referenced: the parsed asset
/// is destroyed with `parseSceneData` and the decode fan-out runs after it.
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
 * @brief Everything a scene load derives from the document, with no device involved.
 *
 * `GltfScene::load` minus decode, upload and descriptors -- minus every line that could take
 * a `VkDevice`. The sidecar is what skips it.
 */
bool parseSceneData(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded) {

    auto parseStart = std::chrono::steady_clock::now();
    fastgltf::Asset asset;
    /// As authored, keyed by node index and **not yet placed**; the node walk below is what
    /// applies a world transform, and for colliders the geometry too.
    std::vector<ParticleEmitter> emitterTemplates;
    std::vector<ColliderDesc> colliderTemplates;
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

        // Before `loadGltf`, which moves the getter's read cursor. See `parseSceneEmitters`
        // for why the extras are read by a second, targeted pass.
        {
            auto s2 = core::Profiler::scope("GltfScene::extras");
            auto extrasStart = std::chrono::steady_clock::now();
            const auto bytes = static_cast<fastgltf::span<std::byte>>(mapped.get());

            // **One parse, three readers.** A document per reader re-scans the whole file to
            // reach one key of `nodes[].extras`, and three of those is most of a large
            // scene's load.
            core::json::GltfDocument document;
            if (!document.parse(bytes.data(), bytes.size_bytes())) {
                core::Logger::warn(core::LogCategory::GLTF, "%s: could not scan for scene extras", path.string().c_str());
            } else if (document.nodes != nullptr) {
                // A glTF with no `nodes` array is valid, so its absence is not a warning.
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

    auto geomStart = std::chrono::steady_clock::now();
    std::vector<uint32_t> meshFirstPrim(asset.meshes.size(), 0);
    std::vector<uint32_t> meshPrimCount(asset.meshes.size(), 0);
    std::vector<uint32_t> meshMorphTargets(asset.meshes.size(), 0);

    {
        auto s = core::Profiler::scope("GltfScene::geometry");

        // Rough: the real counts are not known until every accessor has been read, and one
        // over-reserve beats reallocating across a hundred primitives.
        data.vertices.reserve(1u << 18);
        data.indices.reserve(1u << 19);

        for (size_t meshIdx = 0; meshIdx < asset.meshes.size(); ++meshIdx) {
            const auto& mesh = asset.meshes[meshIdx];
            meshFirstPrim[meshIdx] = static_cast<uint32_t>(data.primitives.size());
            // Out here because the name is the *mesh's* while the cloth it makes is the
            // primitive's -- see the block below.
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

                // TANGENT is optional in glTF, and a value-initialised `Vertex` leaves it at
                // zero: `normalize(vec3(0))` is a NaN that skinning.comp writes into the
                // deformed vertex buffer for the G-buffer to shade from. The fallback is an
                // arbitrary perpendicular, which is right for a material with no normal map
                // and wrong in a stated way for one that has both -- the spec asks a client
                // to compute MikkTSpace tangents there.
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

                // Both attributes or neither: a primitive with JOINTS_0 and no WEIGHTS_0 is
                // malformed, and taking the joints anyway gives every vertex a zero-weight
                // rig, which collapses the mesh to the origin.
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

                // Target-major -- every displacement of target 0, then of target 1 -- because
                // that is the order the weighted sum walks; interleaving costs the shader a
                // stride of `targets` per vertex.
                //
                // A target may declare any subset of POSITION, NORMAL and TANGENT, and the
                // absent ones stay zero, which is the identity for a displacement. That is
                // what saves a shader from needing a per-target mask.
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
                        // A tangent *displacement* is VEC3, not VEC4: the handedness in w is
                        // a property of the surface, not something a target moves.
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
                 * The **one** name-driven dispatch in the loader: `extras` is a per-node
                 * dictionary with nowhere to put a value per vertex, which is what a pin
                 * weight is. `isFabricMesh` in `Cloth.h` is the only place the predicate is
                 * spelled, and the tests call that one rather than a copy.
                 *
                 * The *mesh's* name, not the node's -- glTF's `mesh.name` is Blender's
                 * data-block, so renaming the object leaves it behind and
                 * `scripts/check_pins.py` refuses that case. Per primitive, because Blender
                 * splits a mesh by material and a curtain wearing two is two soft bodies.
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
                // The spec requires every primitive of a mesh to declare the same number of
                // targets, so the first primitive's count is the mesh's -- and it is the
                // mesh's that a node's weight array is sized from.
                if (meshPrimCount[meshIdx] == 0) meshMorphTargets[meshIdx] = p.morphTargets;
                data.primitives.push_back(p);
                meshPrimCount[meshIdx]++;
            }
        }
    }

    data.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    data.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    {
        auto s = core::Profiler::scope("GltfScene::flatten");

        // The hierarchy is retained as well as flattened: an animation drives the parent
        // links and local TRS, and a joint's world transform is the product down its parent
        // chain, which the flat list cannot express.
        std::vector<SceneNode>& sceneNodes = data.rig.bind.nodes;
        sceneNodes.resize(asset.nodes.size());
        // Parallel to the nodes, and the only way a game can name the joint that carries
        // root motion -- see `SceneAnimator::findNode` and `AnimationRig::nodeNames`.
        data.rig.nodeNames.resize(asset.nodes.size());
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            SceneNode& n = sceneNodes[i];
            data.rig.nodeNames[i] = std::string(asset.nodes[i].name);
            std::visit(fastgltf::visitor{
                           [&](const fastgltf::math::fmat4x4& m) {
                               // A node may give a matrix instead of TRS, and an animation
                               // channel drives one component, so it has to be decomposed.
                               // A file that ships a matrix ships no channel for that node,
                               // so this is the bind pose and never has to round-trip.
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

        // Morph weights are per *node*, not per mesh -- glTF lets a node override the mesh's
        // defaults, so the same morphed mesh can be placed twice with different expressions.
        // Each such node gets its own run in one flat array, starting at `firstWeight`, and
        // the defaults come from the node, then the mesh, then zero: the spec's order.
        //
        // The node branch is unreachable while the current fastgltf is vendored -- it
        // rejects the *whole file* when any node carries `weights`, so `node.weights` is
        // empty whenever the file loaded at all. Deleting it would be deleting the correct
        // behaviour to match somebody else's bug. The symptom to recognise is a file that
        // morphs in every other viewer and fails here before a single mesh is read.
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

        // Collision authored by node name -- see `isColliderNode` -- synthesised into the
        // same template list the extras pass filled and *before* the walk below, so a
        // suffixed node reaches the geometry copy, the world transform and the
        // placement-suppression through the code that already exists.
        //
        // An explicit `substrate_collider` on the same node wins: the two would otherwise
        // produce two bodies for one node.
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            if (!isColliderNode(asset.nodes[i].name)) continue;
            bool declared = false;
            for (const ColliderDesc& tmpl : colliderTemplates) {
                if (tmpl.node == static_cast<uint32_t>(i)) declared = true;
            }
            if (declared) continue;

            // Everything else stays at its default: `Auto` + `Static` resolves to an exact
            // triangle mesh, which is what a floor wants and what `Engine::navMesh` bakes
            // from.
            ColliderDesc c;
            c.node = static_cast<uint32_t>(i);
            c.name = std::string(asset.nodes[i].name);
            if (c.name.empty()) c.name = "node " + std::to_string(i);
            colliderTemplates.push_back(std::move(c));
        }

        // Iterative rather than recursive: a node graph is untrusted input, and a deep or
        // cyclic one must not blow the stack.
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

            // A collider on this node claims everything below it, nearest winning, so a prop
            // with its own collider parented to a character keeps its own.
            uint32_t collider = item.collider;
            for (const ColliderDesc& tmpl : colliderTemplates) {
                if (tmpl.node == static_cast<uint32_t>(item.node)) collider = tmpl.node;
            }

            // A collision node is not drawn, and this is the only place that can say so: an
            // instance exists exactly where a placement did, and several later walks
            // re-derive slot numbers from this list's order, so a suppression made anywhere
            // downstream would have to be learned by all of them.
            if (node.meshIndex.has_value() && !isColliderNode(node.name)) {
                const size_t mesh = *node.meshIndex;
                const uint32_t skin = node.skinIndex.has_value() ? static_cast<uint32_t>(*node.skinIndex)
                                                                 : 0xFFFFFFFFu;
                for (uint32_t i = 0; i < meshPrimCount[mesh]; ++i) {
                    // A skinned mesh's node transform is *not* applied -- the spec says so:
                    // the joint matrices already take a vertex from bind pose to model
                    // space, and multiplying by the node's transform as well applies the
                    // skeleton root twice. The symptom is a character at double distance
                    // from the origin, moving at double speed.
                    data.placements.push_back(
                        {meshFirstPrim[mesh] + i, skin == 0xFFFFFFFFu ? world : glm::mat4(1.0f),
                         static_cast<uint32_t>(item.node), skin, collider});
                }
            }

            // Found here rather than in a pass over `asset.lights`: the same light may be
            // instanced under several nodes, and only the node knows where each copy is.
            if (node.lightIndex.has_value() && *node.lightIndex < asset.lights.size()) {
                data.lights.push_back(toGpuLight(asset.lights[*node.lightIndex], world));
            }

            // An emitter is placed by its node for the same reason. The linear scan holds
            // because a scene has a handful of emitters, not a hundred thousand.
            for (const ParticleEmitter& tmpl : emitterTemplates) {
                if (tmpl.node != static_cast<uint32_t>(item.node)) continue;
                ParticleEmitter placed = tmpl;
                placed.transform = world;
                data.emitters.push_back(std::move(placed));
            }

            // A collider likewise, except that it may also need the node's *geometry*. Those
            // vertices are copied in **node space**: the body carries the node's translation
            // and rotation and its scale reaches the shape, so a shape built from
            // already-transformed vertices applies the placement twice.
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
                        // Each primitive's vertices are appended, so its indices are rebased
                        // twice: off the shared buffer's `baseVertex` and onto wherever this
                        // collider's copy started. A shape build has no equivalent of an
                        // indirect command's signed vertex offset to do it for free.
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

            // The path is resolved here because this is the only place that knows where the
            // .gltf came from: `"hum.wav"` means the file next to the scene, and falls back
            // to the working directory only when there is nothing there.
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

        // Eight transformed AABB corners per placement: raw vertex positions would ignore
        // node scaling, and any two corners would understate a rotated box.
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

    {
        auto s = core::Profiler::scope("GltfScene::animation");

        std::vector<Skin>& sceneSkins = data.rig.skins;
        std::vector<AnimationClip>& sceneClips = data.rig.clips;

        sceneSkins.reserve(asset.skins.size());
        for (const auto& skin : asset.skins) {
            Skin out;
            out.joints.reserve(skin.joints.size());
            for (size_t j : skin.joints) out.joints.push_back(static_cast<uint32_t>(j));

            // An absent inverseBindMatrices accessor means identity for every joint, per the
            // spec. Filled here so the joint matrix loop carries no null test per joint per
            // frame.
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

                // VEC3 outputs are widened to vec4 so the sampler has one array type, at four
                // bytes a key.
                //
                // A SCALAR output can only be morph weights -- no TRS path produces one --
                // which is what identifies a weights sampler *before* the channel pointing
                // at it has been read. The stride is derived for the same reason:
                // `output.count` is keys times targets (times three for CUBICSPLINE), so the
                // target count falls out of a division rather than a lookup this loop cannot
                // yet make.
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

    data.materials.reserve(asset.materials.size() + 1);

    for (const auto& mat : asset.materials) {
        GpuMaterial m{};
        m.baseColorFactor = glm::vec4(mat.pbrData.baseColorFactor[0], mat.pbrData.baseColorFactor[1],
                                      mat.pbrData.baseColorFactor[2], mat.pbrData.baseColorFactor[3]);
        // KHR_materials_emissive_strength, folded into the factor rather than carried as a
        // second field: gbuffer.frag and shadeRayHit both want the product, and a second
        // field is a second thing for either to forget to multiply. `emissiveFactor` is
        // capped at 1.0 by the spec, so a file whose strength is dropped here renders at 1.0
        // without complaint and cannot cross a bloom threshold above it.
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

    // BLEND cannot go through the G-buffer at all -- it stores one surface per pixel and
    // blending needs several -- so those primitives are drawn forward after lighting. A
    // load-time property of the material, decided once here rather than per frame in the
    // record loop.
    //
    // MASK is resolved in the same loop for a different consumer: the shadow pass splits its
    // draws on it so everything opaque can go through a pipeline with no fragment shader.
    // Two flags rather than one enum, because a primitive may be neither.
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
            // Read back out of `data.materials`, not a second table beside `blendMaterial`:
            // `m.alphaMask` is the value the shader actually reads, and a parallel copy is
            // one more thing that can come to disagree with it.
            p.masked = hasMaterial && data.materials[p.materialIndex].alphaMask != 0u;
        }
        for (const auto& draw : data.placements) {
            if (data.primitives[draw.primitive].blended) data.stats.blendedDraws++;
        }
    }

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

    // The one part of the texture pass that is not device work, and so the part that travels
    // in the sidecar. The slot an image is used in decides its format -- colour is authored
    // in sRGB and must be decoded on read, and that decode corrupts a normal or ORM map --
    // and getting it wrong is silent, so the answer is carried rather than recomputed.
    data.images.resize(asset.images.size());
    embedded.resize(asset.images.size());
    for (size_t i = 0; i < asset.images.size(); ++i) {
        std::visit(fastgltf::visitor{
                       [&](const fastgltf::sources::URI& uri) { data.images[i].uri = std::string(uri.uri.path()); },
                       [](const auto&) {},
                   },
                   asset.images[i].data);
        // Lifted here because the document is destroyed with this function and the decode
        // fan-out runs after it. Never written to the sidecar -- see `EmbeddedImages`.
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
    // The sidecar or the document, and no caller can tell which: `data` is the same either
    // way. A cache that does not apply -- absent, stale, a different build -- is not an
    // error and is not logged, because falling back to the document is correct in every one
    // of those cases and a complaint would teach people to bake one to silence it.
    //
    // There is no third branch that writes: baking is `substrate-bake`'s. A load from a
    // document carries no LOD levels and selection resolves to level 0 throughout.
    const auto cacheStart = std::chrono::steady_clock::now();
    const bool fromCache = readSceneCache(path, data);
    if (!fromCache) {
        if (!parseSceneData(path, data, embedded)) return false;
    } else {
        // A cached load has no embedded payloads: every embedded image in a baked scene has
        // a `.ktx2` beside it, which is what `writeSceneCache` refuses to bake without.
        // Sized anyway so the decode indexes it without a branch.
        embedded.resize(data.images.size());
        // The baked run's own parse and geometry numbers travelled in the payload. Reporting
        // them for a run that did neither is a lie the log would tell on every cache hit.
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
