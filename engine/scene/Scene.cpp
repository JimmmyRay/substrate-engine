#include "scene/Scene.h"

#include "core/Logger.h"
#include "core/Profiler.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>

namespace scene {

namespace {

glm::mat4 compose(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale) {
    // Translate * rotate * scale, in that order, which is what glTF and every authoring
    // tool mean by TRS. Any other order places a scaled child somewhere else.
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

/// The inverse of `compose`, as far as TRS can express it: the skew and perspective
/// `glm::decompose` also reports are dropped, which is the loss `setParent` documents.
void decompose(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale) {
    glm::vec3 skew{0.0f};
    glm::vec4 perspective{0.0f};
    if (!glm::decompose(m, scale, rotation, position, skew, perspective)) {
        // A singular matrix -- in practice a zero scale on some axis. Keeping the
        // translation leaves the node somewhere findable rather than at the origin.
        position = glm::vec3(m[3]);
        rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        scale = glm::vec3(1.0f);
    }
}

} // namespace

void Scene::reserve(uint32_t n) {
    nodes.reserve(n);
    sorted.reserve(n);
}

void Scene::link(uint32_t slot, uint32_t parentSlot) {
    Node& n = nodes[slot];
    n.parentSlot = parentSlot;
    n.prevSibling = kNoNode;
    // Pushed at the head, so sibling order is the reverse of creation order. Nothing may
    // depend on it; `sorted` is the only ordering with a guarantee attached.
    uint32_t& head = parentSlot == kNoNode ? rootList : nodes[parentSlot].firstChild;
    n.nextSibling = head;
    if (head != kNoNode) nodes[head].prevSibling = slot;
    head = slot;
}

void Scene::unlink(uint32_t slot) {
    Node& n = nodes[slot];
    if (n.prevSibling != kNoNode) {
        nodes[n.prevSibling].nextSibling = n.nextSibling;
    } else {
        uint32_t& head = n.parentSlot == kNoNode ? rootList : nodes[n.parentSlot].firstChild;
        head = n.nextSibling;
    }
    if (n.nextSibling != kNoNode) nodes[n.nextSibling].prevSibling = n.prevSibling;
    n.prevSibling = kNoNode;
    n.nextSibling = kNoNode;
}

NodeId Scene::create(std::string_view name, NodeId parent) {
    uint32_t parentSlot = kNoNode;
    if (parent.valid()) {
        if (!valid(parent)) {
            // A root with a warning rather than in silence: a hierarchy that went flat
            // because one parent handle was stale is noticed three features later.
            core::Logger::warn(core::LogCategory::Scene, "Scene: '%s' names a parent that is not live; making a root",
                               std::string(name).c_str());
        } else {
            parentSlot = parent.index;
        }
    }

    uint32_t slot;
    if (!freeSlots.empty()) {
        slot = freeSlots.back();
        freeSlots.pop_back();
    } else {
        slot = static_cast<uint32_t>(nodes.size());
        nodes.emplace_back();
        // 1 rather than 0: `Handle` reserves 0 for "never issued", so a zeroed NodeId is
        // invalid rather than a live handle to slot 0.
        nodes[slot].generation = 1;
    }

    Node& n = nodes[slot];
    n.position = glm::vec3(0.0f);
    n.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    n.scale = glm::vec3(1.0f);
    n.world = glm::mat4(1.0f);
    n.firstChild = kNoNode;
    n.live = true;
    n.dirty = true;
    n.name.assign(name);
    n.attached = Attachments{};
    link(slot, parentSlot);

    ++liveNodes;
    orderStale = true;
    ++structure;
    return NodeId{slot, n.generation};
}

uint32_t Scene::nextComponentType() {
    // Process-wide and monotonic, so two `Scene`s share the numbering and a type id means
    // the same thing in both. It costs an empty slot in the smaller one's vector.
    static uint32_t next = 0;
    return next++;
}

void Scene::eraseComponents(uint32_t slot) {
    for (ComponentStore& s : componentStores) {
        if (s.map != nullptr && s.eraseNode != nullptr) s.eraseNode(s.map.get(), slot);
    }
}

template <>
Model& Scene::add<Model>(NodeId id, Model value) {
    if (importer == nullptr) {
        core::Logger::error(core::LogCategory::Scene,
                            "add<Model>('%s'): no importer installed -- `Engine::init` installs one, so this is a "
                            "Scene built outside an Engine and nothing can be imported into it",
                            value.path.string().c_str());
        value.id = kNoModel;
    } else {
        value.id = importer(importerContext, id, value.path);
    }
    // Stored after the import, not before: `value.id` is not filled in until the importer
    // has run, and a component written first records a model that may never have arrived.
    auto [it, inserted] = store<Model>().insert_or_assign(id.index, std::move(value));
    return it->second;
}

void Scene::destroy(NodeId id) {
    if (!valid(id)) return;

    // Unlinked once, at the top: everything below it goes too, so no sibling list needs
    // repairing per node.
    unlink(id.index);

    std::vector<uint32_t> doomed{id.index};
    for (size_t i = 0; i < doomed.size(); ++i) {
        for (uint32_t child = nodes[doomed[i]].firstChild; child != kNoNode; child = nodes[child].nextSibling) {
            doomed.push_back(child);
        }
    }

    for (const uint32_t slot : doomed) {
        Node& n = nodes[slot];
        n.live = false;
        // Both of these have to happen before the slot reaches the free list: a handle
        // issued for it must never match again, and a component left behind would be
        // handed to whatever node is created into this slot next.
        ++n.generation;
        n.name.clear();
        n.attached = Attachments{};
        eraseComponents(slot);
        n.firstChild = kNoNode;
        n.nextSibling = kNoNode;
        n.prevSibling = kNoNode;
        n.parentSlot = kNoNode;
        freeSlots.push_back(slot);
        --liveNodes;
    }
    orderStale = true;
    ++structure;
}

NodeId Scene::parent(NodeId id) const {
    if (!valid(id)) return NodeId{};
    const uint32_t slot = nodes[id.index].parentSlot;
    if (slot == kNoNode) return NodeId{};
    return NodeId{slot, nodes[slot].generation};
}

const std::string& Scene::name(NodeId id) const {
    static const std::string kNone;
    return valid(id) ? nodes[id.index].name : kNone;
}

NodeId Scene::find(std::string_view name) const {
    for (uint32_t slot = 0; slot < nodes.size(); ++slot) {
        if (nodes[slot].live && nodes[slot].name == name) return NodeId{slot, nodes[slot].generation};
    }
    return NodeId{};
}

NodeId Scene::idAt(uint32_t slot) const {
    // A dead slot yields an invalid handle, not the generation it is holding for its next
    // occupant: that one would compare equal to the handle the next `create` here issues.
    if (slot >= nodes.size() || !nodes[slot].live) return NodeId{};
    return NodeId{slot, nodes[slot].generation};
}

NodeId Scene::firstRoot() const { return rootList == kNoNode ? NodeId{} : idAt(rootList); }

NodeId Scene::firstChild(NodeId id) const {
    if (!valid(id)) return NodeId{};
    const uint32_t slot = nodes[id.index].firstChild;
    return slot == kNoNode ? NodeId{} : idAt(slot);
}

NodeId Scene::nextSibling(NodeId id) const {
    if (!valid(id)) return NodeId{};
    const uint32_t slot = nodes[id.index].nextSibling;
    return slot == kNoNode ? NodeId{} : idAt(slot);
}

void Scene::setParentKeepLocal(NodeId id, NodeId parent) {
    if (!valid(id)) return;

    uint32_t parentSlot = kNoNode;
    if (parent.valid()) {
        if (!valid(parent)) {
            core::Logger::warn(core::LogCategory::Scene, "Scene: reparenting '%s' under a node that is not live",
                               nodes[id.index].name.c_str());
            return;
        }
        // A walk up the chain. Without it, a node reparented under its own descendant is
        // a cycle `resort` cannot reach from any root.
        for (uint32_t up = parent.index; up != kNoNode; up = nodes[up].parentSlot) {
            if (up == id.index) {
                core::Logger::warn(core::LogCategory::Scene, "Scene: '%s' cannot be reparented under its own child",
                                   nodes[id.index].name.c_str());
                return;
            }
        }
        parentSlot = parent.index;
    }

    unlink(id.index);
    link(id.index, parentSlot);
    nodes[id.index].dirty = true;
    orderStale = true;
    ++structure;
}

void Scene::setParent(NodeId id, NodeId parent) {
    if (!valid(id)) return;

    // Captured before the move: `setParentKeepLocal` below leaves the world transform
    // pointing at the new parent's space.
    const glm::mat4 world = nodes[id.index].world;
    const uint32_t before = nodes[id.index].parentSlot;

    setParentKeepLocal(id, parent);
    if (nodes[id.index].parentSlot == before) return; // refused, or already there

    glm::mat4 local = world;
    if (nodes[id.index].parentSlot != kNoNode) {
        local = glm::inverse(nodes[nodes[id.index].parentSlot].world) * world;
    }
    decompose(local, nodes[id.index].position, nodes[id.index].rotation, nodes[id.index].scale);
}

void Scene::setLocalPosition(NodeId id, const glm::vec3& position) {
    if (!valid(id)) return;
    nodes[id.index].position = position;
    nodes[id.index].dirty = true;
}

void Scene::setLocalRotation(NodeId id, const glm::quat& rotation) {
    if (!valid(id)) return;
    nodes[id.index].rotation = rotation;
    nodes[id.index].dirty = true;
}

void Scene::setLocalScale(NodeId id, const glm::vec3& scale) {
    if (!valid(id)) return;
    nodes[id.index].scale = scale;
    nodes[id.index].dirty = true;
}

void Scene::setLocalTransform(NodeId id, const glm::mat4& m) {
    if (!valid(id)) return;
    decompose(m, nodes[id.index].position, nodes[id.index].rotation, nodes[id.index].scale);
    nodes[id.index].dirty = true;
}

float Scene::turnToward(NodeId id, const glm::vec3& direction, float rate, float dt, float floor,
                        float forwardOffset) {
    if (!valid(id)) return 0.0f;

    // **The node is the state**: the yaw is read back out of the rotation it holds, so a
    // caller keeping its own copy has two facts to keep in step. Measured from +Z about
    // +Y, the convention `Camera::forward()` and every atan2 in this engine use.
    const glm::vec3 facing = nodes[id.index].rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    float yaw = std::atan2(facing.x, facing.z);

    const glm::vec3 flat(direction.x, 0.0f, direction.z);
    if (const float reach = glm::length(flat); reach > floor && reach > 1e-6f) {
        // `remainder` folds into [-pi, pi], which is the shortest arc: without it a turn
        // across the seam is a revolution minus one degree.
        const float delta = std::remainder(std::atan2(flat.x, flat.z) - forwardOffset - yaw, glm::two_pi<float>());
        const float limit = std::max(rate, 0.0f) * std::max(dt, 0.0f);
        yaw += std::clamp(delta, -limit, limit);
    }

    // Folded back into (-pi, pi] before it is returned. `angleAxis` does not care, but an
    // angle a caller keeps and adds to grows without bound.
    yaw = std::remainder(yaw, glm::two_pi<float>());
    setLocalRotation(id, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    return yaw;
}

void Scene::attachInstance(NodeId id, InstanceId instance, const glm::mat4& offset) {
    if (!valid(id)) return;
    Attachments& a = nodes[id.index].attached;
    a.instance = instance;
    a.instanceOffset = offset;
    a.hasOffset = offset != glm::mat4(1.0f);
    nodes[id.index].dirty = true;
}

void Scene::attachBody(NodeId id, BodyId body) {
    if (valid(id)) nodes[id.index].attached.body = body;
}

void Scene::attachCharacter(NodeId id, PhysicsCharacterId character) {
    if (valid(id)) nodes[id.index].attached.character = character;
}

void Scene::attachSound(NodeId id, SoundId sound) {
    if (valid(id)) {
        nodes[id.index].attached.sound = sound;
        nodes[id.index].dirty = true;
    }
}

void Scene::attachLight(NodeId id, uint32_t lightIndex) {
    if (valid(id)) {
        nodes[id.index].attached.light = lightIndex;
        nodes[id.index].dirty = true;
    }
}

void Scene::attachEmitter(NodeId id, uint32_t emitterIndex) {
    if (valid(id)) {
        nodes[id.index].attached.emitter = emitterIndex;
        nodes[id.index].dirty = true;
    }
}

gfx::GpuLight& Scene::light(NodeId id, std::vector<gfx::GpuLight>& lights) const {
    const uint32_t index = valid(id) ? nodes[id.index].attached.light : kNoAttachment;
    return lights[index < lights.size() ? index : 0];
}

void Scene::resort() {
    sorted.clear();
    if (liveNodes == 0) {
        orderStale = false;
        return;
    }

    // Breadth-first from the roots. Every node's in-degree is zero or one, so this is what
    // a topological sort degenerates to, and `sorted` comes out parent before child.
    for (uint32_t slot = rootList; slot != kNoNode; slot = nodes[slot].nextSibling) sorted.push_back(slot);
    for (size_t i = 0; i < sorted.size(); ++i) {
        for (uint32_t child = nodes[sorted[i]].firstChild; child != kNoNode; child = nodes[child].nextSibling) {
            sorted.push_back(child);
        }
    }

    // Unreachable from any root, which the subtree destroy makes impossible through the
    // public API. Checked anyway: the cost of being wrong is a node that silently stops
    // being drawn.
    if (sorted.size() != liveNodes) {
        core::Logger::warn(core::LogCategory::Scene, "Scene: %zu of %u nodes are unreachable from a root",
                           liveNodes - sorted.size(), liveNodes);
    }
    orderStale = false;
}

void Scene::update(const SceneTargets& targets) {
    auto s = core::Profiler::scope("Scene::update");
    if (orderStale) resort();

    // The pull from the solver is folded into this pass, not run before it: `sorted` is
    // parent before child, so a driven node's world transform is final by the time any
    // child reads it. Dirtiness decides only the push below.
    for (const uint32_t slot : sorted) {
        Node& n = nodes[slot];
        if (n.parentSlot != kNoNode && nodes[n.parentSlot].dirty) n.dirty = true;

        // Taken exactly, with no decomposition on the way in, and with no parent transform
        // applied: what Jolt hands back is already world space.
        const Attachments& a = n.attached;
        if (targets.physics != nullptr && a.character.valid()) {
            n.world = targets.physics->characterTransform(a.character, targets.alpha);
            n.dirty = true;
            continue;
        }
        if (targets.physics != nullptr && a.body.valid() && targets.physics->bodyMoves(a.body) &&
            !targets.physics->bodyKinematic(a.body)) {
            n.world = targets.physics->bodyTransform(a.body, targets.alpha);
            n.dirty = true;
            continue;
        }

        const glm::mat4 local = compose(n.position, n.rotation, n.scale);
        n.world = n.parentSlot == kNoNode ? local : nodes[n.parentSlot].world * local;
    }

    for (const uint32_t slot : sorted) {
        Node& n = nodes[slot];
        if (!n.dirty) continue;
        const Attachments& a = n.attached;

        if (targets.instances != nullptr && a.instance.valid() && targets.instances->valid(a.instance)) {
            targets.instances->setTransform(a.instance, a.hasOffset ? n.world * a.instanceOffset : n.world);
        }
        if (targets.audio != nullptr && a.sound.valid()) {
            targets.audio->setSourceTransform(a.sound, n.world);
        }
        if (a.emitter != kNoAttachment) targets.emitterTransform(a.emitter, n.world);
        if (targets.lights != nullptr && a.light != kNoAttachment && a.light < targets.lights->size()) {
            gfx::GpuLight& l = (*targets.lights)[a.light];
            // Place and aim only: everything else is what `light()` hands out a reference
            // to, and a sweep that overwrote it would make that reference a lie. And only
            // what the *type* has -- a point light has no direction, a directional one no
            // position, and writing both is a difference the byte-for-byte image suite
            // sees. -Z is glTF's punctual direction, shared with `makeSpotLight`.
            const auto type = static_cast<gfx::LightType>(static_cast<uint32_t>(l.params.z));
            if (type != gfx::LightType::Directional) {
                l.position = glm::vec4(glm::vec3(n.world[3]), l.position.w);
            }
            if (type != gfx::LightType::Point) {
                l.direction = glm::vec4(glm::normalize(-glm::vec3(n.world[2])), l.direction.w);
            }
        }
        // Kinematic only. Writing a dynamic body's transform back here puts the node and
        // the solver in a fight over it, since the sweep above just read it.
        if (targets.physics != nullptr && a.body.valid() && targets.physics->bodyKinematic(a.body)) {
            targets.physics->setBodyTransform(a.body, n.world);
        }

        n.dirty = false;
    }
}

void Scene::clear() {
    nodes.clear();
    freeSlots.clear();
    sorted.clear();
    componentStores.clear();
    rootList = kNoNode;
    liveNodes = 0;
    orderStale = false;
    // Bumped even though the order is not stale: a listing cached against the tree that was
    // just emptied is as wrong as one cached against a tree that was resorted.
    ++structure;
}

} // namespace scene
