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
 * @brief Hierarchy, and the one place a transform reaches everything attached to it (G3).
 *
 * ## What this replaces
 *
 * Before it, a thing that moved and had more than one aspect was three unrelated flat
 * lists plus a loop per pair. `Engine` carried `DrivenInstance` and `DrivenSource` --
 * a body, an instance, and `inverse(bodyAtRest) * placementAtRest` to carry the scale
 * across -- and its own comment said *"this exists because there is no scene tree, and
 * G3 deletes it."* An emitter followed an animated node through a third loop, a sound
 * through a fourth. Four loops, four ways of saying "this follows that", and nothing a
 * game could reach to say it about something of its own.
 *
 * A node says it once. Its world transform is computed in one sweep and pushed to
 * whatever is attached: the instance the renderer draws, the light the shader reads, the
 * sound the mixer places, the emitter the particle system spawns from. A body attached
 * to a node is the exception and goes the other way -- see **which direction a transform
 * flows**, below.
 *
 * ## What it deliberately is not
 *
 * Not an ECS, not a component system, and not a place for gameplay fields. It holds
 * transforms, hierarchy and attachment indices. A registry adopted later sits *beside*
 * it, holding a `NodeId` as a component -- the same argument `InstanceTable` already
 * makes about why `entt` cannot own it, and for the same reason: entt's storage is paged
 * and its removal is swap-and-pop, and slot stability is what handles here are for.
 *
 * There is also no `SceneNode` class with a parent pointer and a `vector<SceneNode*>` of
 * children. The tree is indices into flat arrays, which is the design
 * `principles.md` names explicitly as the thing to build instead.
 *
 * ## Slots never move, and that decides the rest of the design
 *
 * A `NodeId` is a slot plus a generation, so a stale handle is detectable rather than a
 * silent alias onto whatever was created in the slot afterwards. That rules out
 * compaction -- and therefore rules out keeping the arrays in parent-before-child order,
 * because reparenting would have to move slots to maintain it.
 *
 * So a separate `order` array holds a topological ordering, rebuilt only on a
 * **structural** change: create, destroy, or reparent. The per-frame sweep walks `order`
 * linearly and computes each node's world transform from a parent whose world transform
 * is already final. That is exactly what `Pose` does for the animation rig, and it is why
 * a moving scene costs one pass over an array rather than a recursion per node.
 *
 * ## Which direction a transform flows
 *
 * | Attachment | Direction | Why |
 * |---|---|---|
 * | instance, light, sound, emitter | **down**, from the node | The node is where the thing is |
 * | dynamic body, character | **up**, into the node | The solver owns it; the node reports it |
 * | kinematic body | **down** | Kinematic means "moved by something else", and this is that |
 *
 * The upstream pull runs first, so a node driven by a body is final before its children
 * are computed. This is the same ordering `Engine::endFrame` used to hold by hand, with
 * the same comment attached: a node with both a clip and a collider is one the physics
 * owns, and the last writer wins.
 *
 * ## A call where there is derived state, a reference where there is not
 *
 * `setLocalPosition` is a call because it invalidates a world transform, a world bounding
 * box and a normal matrix -- which is exactly why `InstanceTable::setTransform` is a call
 * rather than a reference handed out. `light(node)` is a reference because a light has no
 * derived state; the whole array is re-uploaded every frame regardless. The rule is worth
 * stating once because it is the rule everywhere in this API.
 */

/// The tag that makes a node handle its own type. Declared, never defined.
struct NodeTag;

/**
 * @brief Opaque handle to one node: dense slot plus generation.
 *
 * The same lifetime model as `InstanceId`, `BodyId` and `SoundId`, from the same
 * `core/Handle.h`: generation 0 is "never issued", so a zeroed handle is invalid rather
 * than a live handle to slot 0.
 */
using NodeId = core::Handle<NodeTag>;

/// An attachment index that is absent. The same numeric value as every other sentinel
/// here, which `principles.md` rule 8 names as a hazard rather than something to rely on:
/// this one's domain is light and emitter indices.
inline constexpr uint32_t kNoAttachment = 0xFFFFFFFFu;

/**
 * @brief What a node has hanging off it. One flat record, no allocation, no container.
 *
 * Six indices, each defaulting to invalid. The precedent is `Placement::colliderNode`,
 * which is one inherited index doing exactly this job for one attachment kind -- this is
 * that generalised to the kinds that exist, and stopping there. A node with two lights on
 * it is two nodes, which is what a parent is for.
 *
 * `body` and `character` are the pair `DrivenInstance` already used, for the reason it
 * gave: with two handle types the discriminator is the handle, and the state where a
 * bool disagrees with the index it describes stops being expressible.
 */
/// No model. The domain of this sentinel is `GltfScene::ModelId`, which holds the same value.
inline constexpr uint32_t kNoModel = 0xFFFFFFFFu;

/**
 * @brief A node's model: the asset drawn there (C41).
 *
 * ```cpp
 * const NodeId arena = scene.create("arena");
 * scene.add<Model>(arena, {core::Resources("res:/arena.glb")});
 * ```
 *
 * **Adding it imports.** A `.glb` is not a scene, it is one asset that goes in one, and the
 * node is where it goes -- the node's world transform is the placement, so positioning the
 * node first is how you place the import. Everything the file carries comes with it:
 * geometry, materials, lights, emitters, sounds, colliders and a rig. Each instance hangs
 * under `node` at the transform the *document's* own hierarchy gave it, so moving the node
 * afterwards moves the whole import.
 *
 * This is the one component whose addition does work, and it is why `Scene::add` has an
 * overload for it rather than only the template. The alternative was a verb somewhere else
 * -- `engine.addModel(node, path)` -- which says a model is a thing the engine has and the
 * node is a parameter, and that is backwards: the scene has nodes, and a node has a model.
 */
struct Model {
    /// What to import. Set by the caller; `core::Resources` turns a `res:/` name into it.
    std::filesystem::path path;
    /// `GltfScene::ModelId` of what arrived, or `kNoModel` if nothing did. Filled in by the
    /// add; a plain `uint32_t` so this header needs no `GltfScene`, which includes it.
    uint32_t id = kNoModel;
};

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
     * This is `DrivenInstance::localOffset` kept rather than dissolved, and the reason it
     * is a matrix on the record instead of a child node's local transform is arithmetic.
     * A Jolt body has a position and an orientation and no scale at all -- the node's
     * scale went into the shape -- so the mesh a body drives sits at
     * `inverse(bodyAtRest) * placementAtRest` from it, which carries both the scale and a
     * mesh authored off-centre from its collider. Expressed as a child node it would be
     * decomposed to TRS and recomposed every frame, and while that is exact in
     * mathematics it is not exact in floats: the golden set compares images byte for
     * byte, and a transform that round-trips through a quaternion is not the transform it
     * started as.
     *
     * `hasOffset` rather than comparing against the identity, so the ordinary node pays
     * nothing.
     */
    glm::mat4 instanceOffset{1.0f};
    bool hasOffset = false;
};

/**
 * @brief Where a sweep writes what it computed.
 *
 * Four pointers rather than four members on `Scene`, because none of them is the scene's
 * to own and two of them cannot be: `AudioEngine` and `PhysicsWorld` are subsystems with
 * their own lifetimes, and a scene holding them would decide when they die. Any may be
 * null, which is what a headless test passes and what a game with no audio gets.
 */
struct SceneTargets {
    InstanceTable* instances = nullptr;
    std::vector<gfx::GpuLight>* lights = nullptr;
    AudioEngine* audio = nullptr;
    PhysicsWorld* physics = nullptr;
    ParticleSystem* particles = nullptr;
    /// Where between the last two simulation steps the frame is being drawn. Read by the
    /// upstream pull only -- a body's transform is the interpolated one, so a drawn thing
    /// lands between steps rather than snapping to the last one.
    float alpha = 1.0f;
};

class Scene {
  public:
    /// Reserve storage for `n` nodes. An allocation hint; `create` grows.
    void reserve(uint32_t n);

    /**
     * @brief Add a node, optionally under a parent.
     *
     * An invalid `parent` makes a root, which is what most callers want and why it is the
     * default rather than a separate `createRoot`. A parent that is not valid *now* is a
     * caller error and makes a root with a warning -- silently reparenting to the root is
     * how a hierarchy ends up flat without anyone noticing.
     */
    NodeId create(std::string_view name, NodeId parent = NodeId{});

    /**
     * @brief Free the node **and its whole subtree**.
     *
     * The subtree goes because the alternative is a child holding a parent index into a
     * reused slot, which is precisely the aliasing generations exist to stop -- and
     * because "delete the torch" meaning "delete the torch and orphan its flame" is not
     * what anybody means. What it does *not* do is destroy the attachments: an instance,
     * a body and a sound have owners with their own lifetimes, and a scene that destroyed
     * them would be deciding for those owners.
     */
    void destroy(NodeId id);

    [[nodiscard]] bool valid(NodeId id) const {
        return id.valid() && id.index < nodes.size() && nodes[id.index].generation == id.generation &&
               nodes[id.index].live;
    }

    /**
     * @brief Reparent, keeping the node where it is in the world.
     *
     * The local transform is recomputed as `inverse(newParentWorld) * world`, so a torch
     * picked up by a character does not jump. That expression is a matrix and the storage
     * is translation/rotation/scale, so it is decomposed on the way in -- **exact for any
     * chain whose scales are uniform, and lossy for the shear a non-uniform parent scale
     * introduces**, which TRS cannot represent at all. Stated rather than hidden: the
     * alternative is storing a matrix per node and losing `setLocalScale` as a cheap
     * write, and non-uniform scale on a *parent* of something being reparented is rare
     * enough not to pay for everywhere.
     *
     * Refuses a cycle -- reparenting a node under its own descendant -- and says so. That
     * check is a walk up the new parent's chain, which is bounded by the depth of the
     * tree and happens on a structural change rather than per frame.
     */
    void setParent(NodeId id, NodeId parent);
    /// Reparent without moving the node in its new parent's space: the local transform is
    /// kept and the world one changes. What an attach-to-socket wants.
    void setParentKeepLocal(NodeId id, NodeId parent);

    [[nodiscard]] NodeId parent(NodeId id) const;
    [[nodiscard]] const std::string& name(NodeId id) const;
    /// First node with this name, or an invalid id. A linear scan, deliberately: names
    /// are for a person reading a log or an inspector, and a map maintained for a lookup
    /// nothing does per frame is a second copy of the names to keep in step.
    [[nodiscard]] NodeId find(std::string_view name) const;

    // ------------------------------------------------------------------ walking it
    /**
     * @brief The four calls that let something outside this class read the tree (G6).
     *
     * Everything above answers a question about a node a caller already holds. Nothing
     * answered *which nodes are there*, so a panel, a save file or a console listing had
     * no way in at all -- `order()` hands out slots rather than handles, and a slot cannot
     * be passed back to any call here.
     *
     * `idAt` is that conversion and is spelled the way `InstanceTable::idAt` already is,
     * because it is the same operation on the same shape of table. The other three expose
     * the sibling list `resort` already walks, which is what makes a depth-first listing --
     * a child directly under its parent, which is how a person reads a hierarchy -- cost a
     * visit per node rather than a scan per node. `order()` stays breadth-first and stays
     * the sweep's; it lists every root before any child, which is correct for the sweep and
     * unreadable as a tree.
     *
     * Sibling order is the reverse of creation order, because `link` pushes at the head.
     * Nothing here promises otherwise; a caller that wants creation order gets it by
     * pushing this list onto a stack, which is what a depth-first walk already does.
     */
    [[nodiscard]] NodeId idAt(uint32_t slot) const;
    /// Head of the list of parentless nodes, or an invalid id when the scene is empty.
    [[nodiscard]] NodeId firstRoot() const;
    [[nodiscard]] NodeId firstChild(NodeId id) const;
    [[nodiscard]] NodeId nextSibling(NodeId id) const;

    /**
     * @brief Bumped by a **structural** change: create, destroy, reparent, clear.
     *
     * The same counter `InstanceTable::revision` is and for the same consumer: a listing
     * that costs a `std::string` per node is rebuilt when the tree moves rather than every
     * frame. Starts at 1, so a holder's zero means "never built" and no live scene reports
     * it.
     *
     * **It does not move when a transform does**, and the name says structure for that
     * reason. A caption here is a name and an attachment record; neither changes when a
     * node is moved, and a counter that ticked every frame would cache nothing.
     */
    [[nodiscard]] uint64_t structureRevision() const { return structure; }

    // ------------------------------------------------------------------ transforms

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
     * A vehicle facing its velocity, a turret facing a target, a boat, an aircraft, an agent
     * on a navmesh and a character facing where it walked all want this identical turn, and
     * none of them is a character -- so it takes a node and a direction (C30). **A call
     * rather than a per-node property the sweep applies**: when a thing turns, how fast, and
     * whether it is facing its velocity or its target is a game's decision, and a tree that
     * turned nodes by itself would own it.
     *
     * `direction` is not normalised, because its length is the speed and `floor` is what
     * separates a heading from rounding -- below it the node keeps the yaw it had rather
     * than jittering about a direction that is noise. `forwardOffset` is where the model's
     * own forward sits relative to +Z, in radians, and is subtracted: a rig authored looking
     * down +X passes `pi/2` rather than having a sign flipped somewhere in the caller.
     */
    float turnToward(NodeId id, const glm::vec3& direction, float rate, float dt, float floor = 0.0f,
                     float forwardOffset = 0.0f);

    [[nodiscard]] const glm::vec3& localPosition(NodeId id) const { return nodes[id.index].position; }
    [[nodiscard]] const glm::quat& localRotation(NodeId id) const { return nodes[id.index].rotation; }
    [[nodiscard]] const glm::vec3& localScale(NodeId id) const { return nodes[id.index].scale; }

    /**
     * @brief The node's transform in world space.
     *
     * For a node a body or a character drives, this is the solver's transform exactly --
     * not a recomposition of one. That is deliberate and is what keeps a physics scene
     * rendering the same image it did before there was a tree: a matrix that round-trips
     * through a decomposition is not the matrix it started as, and the golden set
     * compares bytes. The consequence is stated where it costs something:
     * `localPosition` and its two companions are **not** written back for a driven node,
     * because deriving them would be that round trip.
     *
     * Valid as of the last `update()`. A read after a `setLocalPosition` in the same
     * frame sees the *old* value, and that is deliberate rather than an oversight: making
     * it exact would mean recomputing a chain per read, which is the cost the once-a-frame
     * sweep exists to avoid. A caller that needs it now calls `update()` -- or, far more
     * usually, wants the next frame anyway.
     */
    [[nodiscard]] const glm::mat4& worldTransform(NodeId id) const { return nodes[id.index].world; }

    // ----------------------------------------------------------------- attachments

    /**
     * @brief Draw `instance` at this node.
     *
     * @param offset applied between the node's transform and the instance's. Identity for
     *        everything except a mesh driven by a body it does not sit exactly on -- see
     *        `Attachments::instanceOffset`, which explains why this is a matrix here
     *        rather than a child node.
     */
    void attachInstance(NodeId id, InstanceId instance, const glm::mat4& offset = glm::mat4(1.0f));
    void attachBody(NodeId id, BodyId body);
    void attachCharacter(NodeId id, PhysicsCharacterId character);
    void attachSound(NodeId id, SoundId sound);
    void attachLight(NodeId id, uint32_t lightIndex);
    void attachEmitter(NodeId id, uint32_t emitterIndex);

    [[nodiscard]] const Attachments& attachments(NodeId id) const { return nodes[id.index].attached; }

    // ------------------------------------------------------------------ components (C42)
    /**
     * @brief Attach a value of any type to a node, including a type the engine has never
     *        heard of.
     *
     * ```cpp
     * struct Health { int current = 100; };
     * const NodeId fighter = scene.create("player");
     * scene.add<Health>(fighter, {100});
     * if (Health* h = scene.get<Health>(fighter)) h->current -= 10;
     * ```
     *
     * **This is what `Attachments` could not be.** That struct is six typed handle fields,
     * so a node holds exactly one instance, one body and one sound, and nothing a game
     * defines. A game's own per-node data therefore lived in a parallel vector with a
     * `NodeId` in it -- `game/battle_arena`'s `struct Fighter` is that pattern written out,
     * and every phase of its roadmap adds a field to it.
     *
     * **The engine's own six are deliberately *not* moved here.** The per-frame sweep reads
     * `attached` for every node in the scene, and a hash lookup per node per kind is a real
     * cost on the hottest walk in `scene/` bought for API symmetry alone. They keep their
     * dense fields and their `attach*` verbs; what a game gains is somewhere to put
     * everything else. C41's asset-import components are new types and land here.
     *
     * One value per type per node: adding a second replaces the first, which is what a
     * caller doing it means. A node wanting two of something holds a component with a
     * container in it, or two child nodes.
     *
     * Components are destroyed with their node, and with its whole subtree.
     */
    template <class T>
    T& add(NodeId id, T value) {
        auto& map = store<T>();
        auto [it, inserted] = map.insert_or_assign(id.index, std::move(value));
        return it->second;
    }

    /**
     * @brief Install what performs an import. `Engine` calls this; a game never does.
     *
     * A function pointer and an opaque context rather than an interface, which is the same
     * type erasure the component store below uses and for the same reason: the engine defines
     * three base classes and a fourth is the thing to stop. It is also what keeps this header
     * free of `Engine.h` -- importing needs a device, an uploader and the geometry buffers,
     * and none of that belongs in a node tree.
     */
    using Importer = uint32_t (*)(void* context, NodeId node, const std::filesystem::path& path);
    void setImporter(Importer fn, void* context) {
        importer = fn;
        importerContext = context;
    }

    /// The node's component of this type, or null. Null for a node that never had one and
    /// for a stale handle alike -- a caller testing the pointer needs no second validity check.
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
     * A reference rather than a call because a light has no derived state -- the array is
     * re-uploaded every frame regardless -- which is the rule this API states once and
     * follows everywhere. `position` and `direction` are the exception and are overwritten
     * by the next `update()`, because those are the node's to say.
     *
     * The caller must have attached a light and must pass the same array it attached
     * against. An unattached node yields the first light rather than reading past the
     * end, for the reason `settings::row` gives about its own out-of-range case: a bounds
     * bug should be a wrong answer in one place rather than a crash somewhere later.
     */
    [[nodiscard]] gfx::GpuLight& light(NodeId id, std::vector<gfx::GpuLight>& lights) const;

    // ---------------------------------------------------------------------- update

    /**
     * @brief One frame: pull from the solver, sweep the transforms, push to everything.
     *
     * Three phases in one call rather than three calls, because the order between them is
     * the whole correctness argument and a caller free to run them separately is a caller
     * free to get it wrong.
     *
     * Every node's world transform is recomputed, not only the dirty ones. That is the
     * same trade `InstanceTable::endFrame` makes and it is made for the same reason: a
     * multiply per node is cheaper than the bookkeeping that would say which to skip, and
     * getting that bookkeeping wrong leaves exactly the objects that moved behind. What
     * dirtiness *does* decide is the **push** -- an unchanged node writes nothing to its
     * instance, which is what keeps a static scene's instance revision from moving and
     * therefore keeps it from re-uploading every frame.
     */
    void update(const SceneTargets& targets);

    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(nodes.size()); }
    [[nodiscard]] uint32_t liveCount() const { return liveNodes; }
    /// The topological order, parent before child. Exposed for the test that pins it,
    /// which is the property every other guarantee here rests on.
    [[nodiscard]] const std::vector<uint32_t>& order() const { return sorted; }

    void clear();

  private:
    struct Node {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        glm::mat4 world{1.0f};
        uint32_t parentSlot = kNoNode;
        /// The child list, and the reason this is a list rather than a scan. A scene with
        /// ten thousand nodes makes "find my children" a ten-thousand-entry sweep, and
        /// `resort` asks it once per node -- which is a hundred million comparisons per
        /// structural change on a scene that is not large. Three indices per node turn
        /// every structural operation here linear in what it touches. Doubly linked
        /// because unlinking is what reparenting does, and a singly linked list makes
        /// that a walk over the siblings.
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
     * **No base class and no virtual, deliberately.** `engine/` defines exactly three base
     * classes and this is not going to be a fourth -- see CLAUDE.md. `shared_ptr<void>`
     * already carries the right deleter for whatever it was constructed from, which is the
     * whole of what a virtual destructor would have bought; the one operation that has to be
     * callable without knowing `T` is erasing a node's entry, and that is a function pointer.
     */
    struct ComponentStore {
        std::shared_ptr<void> map;
        void (*eraseNode)(void*, uint32_t) = nullptr;
    };

    /// A dense id per component type, assigned on first use. A function-local static in a
    /// template, which is the one place C++ hands out a per-type counter without a registry
    /// somebody has to keep in step.
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

    /// Rebuild `sorted`. Called on every structural change and never per frame.
    ///
    /// There is deliberately no `markSubtreeDirty`. A write marks the node it wrote, and
    /// the sweep propagates -- it already walks parent before child, so a child sees a
    /// dirty parent on the same pass that computes its world transform. A separate
    /// subtree walk per write would be the same work done twice.
    void resort();
    /// Put `slot` into `parentSlot`'s child list, or the root list. Does not touch the
    /// transform: both `setParent` overloads decide that for themselves.
    void link(uint32_t slot, uint32_t parentSlot);
    /// Take `slot` out of whichever list holds it.
    void unlink(uint32_t slot);

    std::vector<Node> nodes;
    /// Head of the list of parentless nodes, threaded through the same sibling links.
    /// A separate head rather than a synthetic root node, because a root's `parentSlot`
    /// being `kNoNode` is what every other part of this reads.
    uint32_t rootList = kNoNode;
    std::vector<uint32_t> freeSlots;
    /// Live slots, parent before child. Rebuilt on a structural change.
    std::vector<uint32_t> sorted;
    uint32_t liveNodes = 0;
    /// Whether `sorted` is stale. Set by a structural change, cleared by `resort`, which
    /// `update` calls -- so a game may make fifty structural changes in a frame and pay
    /// for one sort.
    bool orderStale = false;
    /// What `structureRevision` reports. Moves with `orderStale`, and every write to one
    /// is a write to the other -- they answer the same question to two different readers,
    /// one of which is inside this class and one of which is not.
    uint64_t structure = 1;
};

/**
 * @brief Adding a `Model` imports the asset it names onto the node. See `Model`.
 *
 * A specialisation rather than an overload, because an overload is not a candidate when the
 * template argument is written out: `add<Model>(node, {path})` would silently store the
 * component without importing anything. This catches that spelling and `add(node, Model{...})`
 * alike, which is the whole point of it being the same verb as every other component.
 *
 * The returned component carries the `GltfScene::ModelId` in `id`, or `kNoModel` if nothing
 * arrived -- which includes a `Scene` built outside an `Engine`, since only an `Engine` has
 * the device and the buffers an import needs.
 */
template <>
Model& Scene::add<Model>(NodeId id, Model value);

} // namespace scene
