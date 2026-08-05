#pragma once

#include "core/Handle.h"
#include "gfx/Light.h"
#include "scene/Audio.h"
#include "scene/InstanceTable.h"
#include "scene/Node.h"
#include "scene/ParticleSystem.h"
#include "scene/Physics.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scene {

/**
 * @file engine/scene/Scene.h
 * @brief Hierarchy, and the one place a transform reaches everything attached to it.
 *
 * Not an ECS: transforms, hierarchy and attachment indices, with a game's own data in the
 * component store. See systems.md, "The scene tree".
 *
 * **Which way a transform flows.** Down from the node to an instance, light, sound or
 * emitter; **up** into the node from a dynamic body or character, because the solver owns
 * those; down again for a kinematic body. The upstream pull runs first, so a node with
 * both a clip and a collider is one the physics owns.
 */

/// The tag that makes a node handle its own type. Declared, never defined.
struct NodeTag;

/**
 * @brief Opaque handle to one node: dense slot plus generation.
 *
 * Generation 0 is "never issued", so a zeroed handle is invalid rather than a live handle
 * to slot 0.
 */
using NodeId = core::Handle<NodeTag>;

/// An absent light or emitter index. Numerically equal to every other sentinel here,
/// which principles.md rule 8 names as a hazard rather than something to rely on.
inline constexpr uint32_t kNoAttachment = 0xFFFFFFFFu;

/// No model. The domain of this sentinel is `GltfScene::ModelId`, which holds the same value.
inline constexpr uint32_t kNoModel = 0xFFFFFFFFu;

/**
 * @brief A node's model: the asset drawn there.
 *
 * **Adding it imports**, which is what makes this the one component whose addition does
 * work. The node's world transform at the time is the placement, and everything the file
 * carries hangs under the node at the transform the *document's* hierarchy gave it.
 */
struct Model {
    /// What to import. Set by the caller; `core::Resources` turns a `res:/` name into it.
    std::filesystem::path path;
    /// `GltfScene::ModelId` of what arrived, or `kNoModel` if nothing did. Filled in by the
    /// add; a plain `uint32_t` so this header needs no `GltfScene`, which includes it.
    uint32_t id = kNoModel;
};

/// What a node has hanging off it. One of each: a node with two lights on it is two nodes.
struct Attachments {
    InstanceId instance;
    BodyId body;
    PhysicsCharacterId character;
    SoundId sound;
    /// Index into the renderer's light array, or `kNoAttachment`.
    uint32_t light = kNoAttachment;
    /// Index into the particle system's emitter array, or `kNoAttachment`.
    uint32_t emitter = kNoAttachment;

    /**
     * @brief Applied between the node's world transform and the instance's.
     *
     * A matrix here rather than a child node's TRS: a Jolt body has no scale, so a mesh a
     * body drives sits at `inverse(bodyAtRest) * placementAtRest` from it, and a transform
     * that round-trips through a decomposition every frame is not the transform it started
     * as. The golden set compares images byte for byte.
     */
    glm::mat4 instanceOffset{1.0f};
    bool hasOffset = false;
};

/**
 * @brief Where a sweep writes what it computed.
 *
 * Pointers because none of these is the scene's to own. Any may be null, which is what a
 * headless test passes and what a game with no audio gets.
 */
struct SceneTargets {
    InstanceTable* instances = nullptr;
    std::vector<gfx::GpuLight>* lights = nullptr;
    AudioEngine* audio = nullptr;
    PhysicsWorld* physics = nullptr;
    ParticleSystem* particles = nullptr;
    /// Where between the last two simulation steps the frame is being drawn, in [0, 1].
    /// Read by the upstream pull only; 1.0 snaps a drawn body to the last step.
    float alpha = 1.0f;
};

class Scene {
  public:
    /// Reserve storage for `n` nodes. An allocation hint; `create` grows.
    void reserve(uint32_t n);

    /// Add a node, optionally under a parent. A default-constructed `parent` makes a root;
    /// a *stale* one makes a root and warns, because silently reparenting to the root is
    /// how a hierarchy ends up flat without anyone noticing.
    NodeId create(std::string_view name, NodeId parent = NodeId{});

    /**
     * @brief Free the node **and its whole subtree**.
     *
     * The attachments outlive it: an instance, a body and a sound have owners with their
     * own lifetimes, and this call is not one of them.
     */
    void destroy(NodeId id);

    [[nodiscard]] bool valid(NodeId id) const {
        return id.valid() && id.index < nodes.size() && nodes[id.index].generation == id.generation &&
               nodes[id.index].live;
    }

    /**
     * @brief Reparent, keeping the node where it is in the world.
     *
     * The local transform is recomputed as `inverse(newParentWorld) * world` and decomposed
     * into TRS: **exact for any chain whose scales are uniform, lossy for the shear a
     * non-uniform parent scale introduces**, which TRS cannot represent at all.
     *
     * Refuses a cycle -- reparenting a node under its own descendant -- and says so.
     */
    void setParent(NodeId id, NodeId parent);
    /// Reparent without moving the node in its new parent's space: the local transform is
    /// kept and the world one changes. What an attach-to-socket wants.
    void setParentKeepLocal(NodeId id, NodeId parent);

    [[nodiscard]] NodeId parent(NodeId id) const;
    [[nodiscard]] const std::string& name(NodeId id) const;
    /// First node with this name, or an invalid id. A linear scan; nothing should ask this
    /// per frame.
    [[nodiscard]] NodeId find(std::string_view name) const;

    /// The handle in `slot`, for a caller holding one of `order()`'s slots. Invalid for a
    /// slot no live node occupies.
    [[nodiscard]] NodeId idAt(uint32_t slot) const;
    /// Head of the list of parentless nodes, or an invalid id when the scene is empty.
    /// Sibling order is the reverse of creation order, because `link` pushes at the head.
    [[nodiscard]] NodeId firstRoot() const;
    [[nodiscard]] NodeId firstChild(NodeId id) const;
    [[nodiscard]] NodeId nextSibling(NodeId id) const;

    /**
     * @brief Bumped by a **structural** change: create, destroy, reparent, clear.
     *
     * Starts at 1, so a holder's zero means "never built" and no live scene reports it.
     * **It does not move when a transform does**, so a cache keyed on it holds names and
     * attachment records and nothing positional.
     */
    [[nodiscard]] uint64_t structureRevision() const { return structure; }

    void setLocalPosition(NodeId id, const glm::vec3& position);
    void setLocalRotation(NodeId id, const glm::quat& rotation);
    void setLocalScale(NodeId id, const glm::vec3& scale);
    /// All three at once, for a caller that has a matrix. Decomposed, with the same
    /// caveat `setParent` states about shear.
    void setLocalTransform(NodeId id, const glm::mat4& m);

    /**
     * @brief Slew a node's yaw toward a horizontal direction by the shortest arc, at most
     *        `rate * dt` radians of it this step. Returns the yaw the node now holds.
     *
     * `direction` is deliberately not normalised: its length is the speed, and below
     * `floor` the node keeps the yaw it had rather than jittering about a direction that is
     * noise. `forwardOffset` is where the model's own forward sits relative to +Z, in
     * radians, and is subtracted -- a rig authored looking down +X passes `pi/2`.
     */
    float turnToward(NodeId id, const glm::vec3& direction, float rate, float dt, float floor = 0.0f,
                     float forwardOffset = 0.0f);

    [[nodiscard]] const glm::vec3& localPosition(NodeId id) const { return nodes[id.index].position; }
    [[nodiscard]] const glm::quat& localRotation(NodeId id) const { return nodes[id.index].rotation; }
    [[nodiscard]] const glm::vec3& localScale(NodeId id) const { return nodes[id.index].scale; }

    /**
     * @brief The node's transform in world space, as of the last `update()`.
     *
     * A read after a `setLocalPosition` in the same frame sees the **old** value; a caller
     * that needs it now calls `update()`. For a node a body or a character drives this is
     * the solver's matrix exactly, and `localPosition` and its two companions are **not**
     * written back, because deriving them is a decomposition round trip the golden set can
     * see.
     */
    [[nodiscard]] const glm::mat4& worldTransform(NodeId id) const { return nodes[id.index].world; }

    /**
     * @brief Draw `instance` at this node.
     *
     * @param offset applied between the node's transform and the instance's. Identity for
     *        everything except a mesh driven by a body it does not sit exactly on -- see
     *        `Attachments::instanceOffset`.
     */
    void attachInstance(NodeId id, InstanceId instance, const glm::mat4& offset = glm::mat4(1.0f));
    void attachBody(NodeId id, BodyId body);
    void attachCharacter(NodeId id, PhysicsCharacterId character);
    void attachSound(NodeId id, SoundId sound);
    void attachLight(NodeId id, uint32_t lightIndex);
    void attachEmitter(NodeId id, uint32_t emitterIndex);

    [[nodiscard]] const Attachments& attachments(NodeId id) const { return nodes[id.index].attached; }

    /**
     * @brief Attach a value of any type to a node, including a type the engine has never
     *        heard of.
     *
     * One value per type per node: adding a second replaces the first. Components are
     * destroyed with their node, and with its whole subtree.
     */
    template <class T>
    T& add(NodeId id, T value) {
        auto& map = store<T>();
        auto [it, inserted] = map.insert_or_assign(id.index, std::move(value));
        return it->second;
    }

    /// Install what performs an import. `Engine` calls this; a game never does. A function
    /// pointer rather than an interface is what keeps this header free of `Engine.h`.
    using Importer = uint32_t (*)(void* context, NodeId node, const std::filesystem::path& path);
    void setImporter(Importer fn, void* context) {
        importer = fn;
        importerContext = context;
    }

    /// The node's component of this type, or null -- for a stale handle as much as for a
    /// node that never had one, so a caller testing the pointer needs no `valid` call.
    template <class T>
    [[nodiscard]] T* get(NodeId id) {
        if (!valid(id)) return nullptr;
        auto& map = store<T>();
        const auto it = map.find(id.index);
        return it == map.end() ? nullptr : &it->second;
    }

    template <class T>
    [[nodiscard]] const T* get(NodeId id) const {
        return const_cast<Scene*>(this)->get<T>(id);
    }

    template <class T>
    [[nodiscard]] bool has(NodeId id) const {
        return get<T>(id) != nullptr;
    }

    /// Drop this node's component of this type. Nothing happens if it had none.
    template <class T>
    void remove(NodeId id) {
        if (!valid(id)) return;
        store<T>().erase(id.index);
    }

    /// Call `fn(NodeId, T&)` for every node carrying this component, in unspecified order.
    /// **Unspecified and not stable**: this is a hash map, so anything whose output depends
    /// on the order has to sort what it collects rather than rely on the walk.
    template <class T, class Fn>
    void each(Fn&& fn) {
        for (auto& [slot, value] : store<T>()) {
            if (slot < nodes.size() && nodes[slot].live) fn(NodeId{slot, nodes[slot].generation}, value);
        }
    }

    /**
     * @brief The light attached to a node, by reference.
     *
     * `position` and `direction` are overwritten by the next `update()`; every other field
     * is the caller's. `lights` must be the same array the light was attached against, and
     * an unattached node yields the first light rather than reading past the end.
     */
    [[nodiscard]] gfx::GpuLight& light(NodeId id, std::vector<gfx::GpuLight>& lights) const;

    /**
     * @brief One frame: pull from the solver, sweep the transforms, push to everything.
     *
     * One call rather than three because the order between the phases is the whole
     * correctness argument. Every node's world transform is recomputed; dirtiness decides
     * only the **push**, which is what keeps a static scene from re-uploading its instances
     * every frame.
     */
    void update(const SceneTargets& targets);

    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(nodes.size()); }
    [[nodiscard]] uint32_t liveCount() const { return liveNodes; }
    /// Live slots in topological order, parent before child -- breadth-first, so every root
    /// precedes any child. Not a tree listing; `firstChild` and `nextSibling` are that.
    [[nodiscard]] const std::vector<uint32_t>& order() const { return sorted; }

    void clear();

  private:
    struct Node {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        glm::mat4 world{1.0f};
        uint32_t parentSlot = kNoNode;
        /// The child list, doubly linked so that unlinking -- which is what reparenting
        /// does -- costs no walk over the siblings.
        uint32_t firstChild = kNoNode;
        uint32_t nextSibling = kNoNode;
        uint32_t prevSibling = kNoNode;
        uint32_t generation = 0;
        bool live = false;
        /// Set by any write to the local transform and by a structural change, cleared by
        /// the push. What decides whether an attachment is written this frame.
        bool dirty = true;
        std::string name;
        Attachments attached;
    };

    /**
     * @brief One component type's storage, type-erased so `Scene` can hold a vector of them.
     *
     * `shared_ptr<void>` carries the deleter for whatever it was constructed from, which is
     * what a virtual destructor would have bought; the one operation that has to be callable
     * without knowing `T` is erasing a node's entry, and that is `eraseNode`.
     */
    struct ComponentStore {
        std::shared_ptr<void> map;
        void (*eraseNode)(void*, uint32_t) = nullptr;
    };

    /// A dense id per component type, assigned on first use. Not stable across runs or
    /// across translation units linked in a different order -- nothing may serialise it.
    [[nodiscard]] static uint32_t nextComponentType();

    template <class T>
    [[nodiscard]] static uint32_t componentType() {
        static const uint32_t id = nextComponentType();
        return id;
    }

    template <class T>
    [[nodiscard]] std::unordered_map<uint32_t, T>& store() {
        using Map = std::unordered_map<uint32_t, T>;
        const uint32_t type = componentType<T>();
        if (type >= componentStores.size()) componentStores.resize(type + 1);
        ComponentStore& slot = componentStores[type];
        if (slot.map == nullptr) {
            slot.map = std::make_shared<Map>();
            slot.eraseNode = [](void* m, uint32_t node) { static_cast<Map*>(m)->erase(node); };
        }
        return *static_cast<Map*>(slot.map.get());
    }

    /// Drop every component this node held. Called for each node of a destroyed subtree.
    void eraseComponents(uint32_t slot);

    std::vector<ComponentStore> componentStores;

    Importer importer = nullptr;
    void* importerContext = nullptr;

    /// Rebuild `sorted`. Called on a structural change and never per frame.
    void resort();
    /// Put `slot` into `parentSlot`'s child list, or the root list. Does not touch the
    /// transform: both `setParent` overloads decide that for themselves.
    void link(uint32_t slot, uint32_t parentSlot);
    /// Take `slot` out of whichever list holds it.
    void unlink(uint32_t slot);

    std::vector<Node> nodes;
    /// Head of the list of parentless nodes, threaded through the same sibling links.
    uint32_t rootList = kNoNode;
    std::vector<uint32_t> freeSlots;
    /// Live slots, parent before child. Rebuilt on a structural change.
    std::vector<uint32_t> sorted;
    uint32_t liveNodes = 0;
    /// Whether `sorted` is stale. Cleared by `resort`, which `update` calls, so fifty
    /// structural changes in a frame pay for one sort.
    bool orderStale = false;
    /// What `structureRevision` reports. Every write to `orderStale` is a write to this
    /// too; the two answer the same question, one inside this class and one outside.
    uint64_t structure = 1;
};

/**
 * @brief Adding a `Model` imports the asset it names onto the node. See `Model`.
 *
 * A specialisation rather than an overload, because an overload is not a candidate when the
 * template argument is written out: `add<Model>(node, {path})` would silently store the
 * component without importing anything.
 *
 * `id` comes back `kNoModel` when nothing arrived, which includes a `Scene` built outside an
 * `Engine` -- only an `Engine` has the device and buffers an import needs.
 */
template <>
Model& Scene::add<Model>(NodeId id, Model value);

} // namespace scene
