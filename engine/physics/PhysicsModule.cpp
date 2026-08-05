#include "physics/PhysicsModule.h"

#include "Engine.h"
#include "Modules.h"

#include <vector>

namespace physics {

namespace {

/**
 * The engine's one world and the cloth solved inside it. File-scope rather than `Engine`
 * members, which would name `physics::PhysicsWorld` in `Engine.h` and link the solver into
 * every game.
 *
 * **Declaration order is the teardown order and it is load-bearing.** A `ClothSystem` holds
 * soft bodies the world owns, so it has to be destroyed first, which is what declaring it
 * second does. That is also why the pair is one module: file-scope objects in *different*
 * translation units are destroyed in an unspecified order, so splitting them would put this
 * guarantee in the hands of the link order.
 */
PhysicsWorld g_world;
ClothSystem g_cloth;

/// The renderer's view of `g_cloth`, rebuilt by every call that can move a mesh. A span left
/// over a cloth that has since been placed copies freed memory into the vertex buffer, which
/// draws a plausible surface rather than crashing.
std::vector<gfx::DeformedMesh> g_clothMeshes;

void refreshClothMeshes() {
    g_clothMeshes.clear();
    g_clothMeshes.reserve(g_cloth.count());
    for (uint32_t c = 0; c < g_cloth.count(); ++c) {
        g_clothMeshes.push_back({g_cloth.at(c).instance, g_cloth.at(c).vertices});
    }
}

struct Module final : modules::Physics {
    void init(const scene::PhysicsConfig& cfg, uint32_t expectedBodies) override {
        // The cloth bookkeeping names soft bodies `init` is about to drop, so it goes with
        // them. Leaving it puts stale indices into the next scene's world.
        g_cloth.clear();
        refreshClothMeshes();
        g_world.init(cfg, expectedBodies);
    }

    void setDebugContacts(bool on) override { g_world.debugContacts = on; }

    scene::BodyId createBody(const scene::ColliderDesc& desc) override { return g_world.createBody(desc); }

    scene::PhysicsCharacterId createCharacter(const scene::ColliderDesc& desc) override {
        return g_world.createCharacter(desc);
    }

    void finalize() override { g_world.finalize(); }

    void step(float dt) override { g_world.step(dt); }

    [[nodiscard]] Stats stats() const override {
        Stats s;
        s.bodies = g_world.bodyCount();
        s.characters = g_world.characterCount();
        s.capacity = g_world.bodyCapacity();
        s.refused = g_world.refusedBodies();
        s.cloths = g_cloth.count();
        s.clothVertices = g_cloth.vertexCount();
        s.empty = g_world.empty();
        return s;
    }

    [[nodiscard]] glm::mat4 restTransform(scene::BodyId body, scene::PhysicsCharacterId character) const override {
        if (character.valid()) return g_world.characterTransform(character, 0.0f);
        return g_world.bodyTransform(body, 0.0f);
    }

    void drawDebug(std::vector<gfx::DebugLineVertex>& out, const glm::vec3& cameraPosition) override {
        g_world.drawDebug(out, cameraPosition);
    }

    [[nodiscard]] core::Slot<bool(scene::BodyId, float, glm::mat4*)> bodyPoses() override {
        return core::Slot<bool(scene::BodyId, float, glm::mat4*)>(
            [](void*, scene::BodyId id, float alpha, glm::mat4* out) {
                // Kinematic is excluded here and written back below instead: a body something
                // outside the solver moves is one the tree owns, and reading it here as well
                // makes the two fight over it a frame at a time.
                if (!g_world.bodyMoves(id) || g_world.bodyKinematic(id)) return false;
                *out = g_world.bodyTransform(id, alpha);
                return true;
            },
            nullptr);
    }

    [[nodiscard]] core::Slot<bool(scene::PhysicsCharacterId, float, glm::mat4*)> characterPoses() override {
        return core::Slot<bool(scene::PhysicsCharacterId, float, glm::mat4*)>(
            [](void*, scene::PhysicsCharacterId id, float alpha, glm::mat4* out) {
                *out = g_world.characterTransform(id, alpha);
                return true;
            },
            nullptr);
    }

    [[nodiscard]] core::Slot<void(scene::BodyId, const glm::mat4&)> kinematicBodies() override {
        return core::Slot<void(scene::BodyId, const glm::mat4&)>(
            [](void*, scene::BodyId id, const glm::mat4& transform) {
                if (!g_world.bodyKinematic(id)) return;
                g_world.setBodyTransform(id, transform);
            },
            nullptr);
    }

    [[nodiscard]] core::Slot<bool(uint64_t, scene::CharacterMotion*)> characterMotion() override {
        return characterMotionSource(g_world);
    }

    [[nodiscard]] core::Slot<bool(const glm::vec3&, const glm::vec3&, scene::BodyId)> segmentBlocked() override {
        return core::Slot<bool(const glm::vec3&, const glm::vec3&, scene::BodyId)>::bind<&PhysicsWorld::segmentBlocked>(
            &g_world);
    }

    bool addCloth(uint32_t instance, uint32_t primitive, const scene::ClothDesc& desc) override {
        const bool placed = g_cloth.add(g_world, instance, primitive, desc);
        refreshClothMeshes();
        return placed;
    }

    void updateCloth() override { g_cloth.update(g_world); }

    [[nodiscard]] std::span<const gfx::DeformedMesh> clothMeshes() const override { return g_clothMeshes; }
};

Module g_module;

/// Assign `modules::physics` from a header instead and any transitive include links the solver
/// into a game that never asked for it.
struct Registrar {
    Registrar() { modules::physics = &g_module; }
};

const Registrar g_registrar;

} // namespace

PhysicsWorld& world() {
    return g_world;
}

} // namespace physics

// Defining this in Engine.cpp instead links physics into every binary -- Engine.cpp is in all
// of them and this file is not.
::physics::PhysicsWorld& Engine::physics() {
    return ::physics::g_world;
}
