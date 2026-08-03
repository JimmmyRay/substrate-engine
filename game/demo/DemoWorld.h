#pragma once

#include "Engine.h"

#include <glm/glm.hpp>

#include <vector>

/**
 * @file game/demo/DemoWorld.h
 * @brief The demo's own world, built in code against the engine's public surface (G9).
 *
 * G1b proves the boundary is *reachable* -- a scaffolded game compiles without including
 * anything under `engine/`. This is the other half: a scene assembled out of the verbs the
 * G arc landed, in one place, at the same time. Braziers with fire, smoke and embers on
 * them; a light, a looping crackle and a scorch mark under each; crates, barrels, a ramp
 * and a sliding platform between them covering all three `ColliderMotion` values; a mirror,
 * an emissive orb and the ground they all stand on; and a character imported at runtime and
 * handed a controller the game makes.
 *
 * **Nothing here is authored in a file.** The scene is Sponza and is untouched -- every mesh,
 * material, body, light, emitter, sound, instance and decal below comes from a call. That
 * is the claim the row exists to test, and the awkward parts of making it true are
 * recorded on the card rather than smoothed over here.
 */

/// What the world keeps between frames. Everything else is created once and forgotten,
/// because the engine owns it from the moment the call returns.
struct DemoWorld {
    /// No light at this index; the world did not build, so nothing authored one.
    static constexpr uint32_t kNoLight = ~0u;

    bool built = false;

    // --------------------------------------------------------------------- the lighting
    /**
     * @brief The fill light `E` reparents onto the character, into `Renderer::lights`.
     *
     * The world authors the demo's whole interior light set, so the world is the only
     * thing that knows where each light landed -- `DemoGame` cannot assume an index the
     * way it could when a fallback placement put a known set in first.
     *
     * A *fill* and not a brazier cone, deliberately: carrying a brazier's light away
     * leaves a vessel visibly on fire and lighting nothing.
     */
    uint32_t torchLight = kNoLight;

    // ------------------------------------------------------------------- the platform
    /// The one kinematic body in the tree, and the whole of what `ColliderMotion::Kinematic`
    /// means: moved by something other than the solver, pushing whatever it meets.
    scene::BodyId platform;
    scene::NodeId platformNode;
    glm::vec3 platformCentre{0.0f};
    /// Half the travel, along Z. The platform slides between `centre - travel` and
    /// `centre + travel`.
    float platformTravel = 0.0f;
    /// Seconds since the world was built, advanced on the fixed step so the platform is a
    /// function of the step index -- which is what keeps a `--locked` run reproducible.
    float clock = 0.0f;

    // ------------------------------------------------------------------- the banner (G11)
    /**
     * @brief The character driving the banner's two morph weights.
     *
     * A character with no skeleton, made by `Engine::createMesh` because the mesh carried
     * targets, and the only thing in the demo whose *shape* changes between frames. Held
     * rather than looked up per step: `morphCharacterOf` is a lookup by model id and the
     * step has the handle already.
     */
    scene::AnimatorId banner;

    /**
     * @brief A player: the controller they drive, the node it drives, and the pose it blends.
     *
     * **The game holds this because the engine does not (G17).** `Engine::authoredCharacters`
     * reports what a file declared and stops there; which of those anybody is driving, and
     * whether a controller the game made is a player at all, is a question only the game can
     * answer.
     *
     * `rig` is the rig the merge appended and not `characterAt(0)` -- Sponza's own morphed
     * props take the low indices, so the imported rig is index 2 today and a different number
     * the moment the scene gains another morph target. It is invalid for a character a file
     * authored, which brings a collider and no skin.
     */
    struct Player {
        scene::PhysicsCharacterId character;
        scene::NodeId node;
        scene::AnimatorId rig;
    };

    /**
     * @brief Everyone playing, in the order they were named. Empty is a legal answer.
     *
     * A vector rather than three members, so the demo states the shape a four-player game
     * uses. It fills one: the follow camera, the planner and the torch are each singular, and
     * a second view is [C25](../../docs/kanban/backlog/C25-a-frame-renders-more-than-one-view.md).
     */
    std::vector<Player> players;

    /// Player `p`'s controller, or an invalid handle when there is no such player. The three
    /// accessors exist so a caller that only ever means player zero reads as one token.
    [[nodiscard]] scene::PhysicsCharacterId playerCharacter(uint32_t p = 0) const {
        return p < players.size() ? players[p].character : scene::PhysicsCharacterId{};
    }
    /// Player `p`'s node -- what a game parents something to when it wants it carried.
    [[nodiscard]] scene::NodeId playerNode(uint32_t p = 0) const {
        return p < players.size() ? players[p].node : scene::NodeId{};
    }
    /// Player `p`'s rig, or an invalid handle for a character that brought no skin.
    [[nodiscard]] scene::AnimatorId playerRig(uint32_t p = 0) const {
        return p < players.size() ? players[p].rig : scene::AnimatorId{};
    }

    // ------------------------------------------------------------------------- effects
    /**
     * @brief The dust a landing throws, as a one-shot `spawnEffect` fires at a contact.
     *
     * Held as a template and copied per impact, exactly as `DemoGame::impactSound` is:
     * an emitter is thirty fields and an impact varies its position and its normal, both
     * of which `spawnEffect` takes as arguments.
     */
    scene::ParticleEmitter dust;
    bool dustReady = false;
};

/// How much bigger than authored `DemoGame::init` imports Sponza. Owned here rather than
/// read back off the engine because since C41 it is the *game's* number: nothing loaded a
/// scene at a scale, the game imported an asset at one. A fraction of the building's extent
/// grows with it; a crate is 0.62 m in a cathedral of any size.
constexpr float kDemoWorldScale = 2.0f;

/// Build all of it. Safe to call every frame; it does its work once.
void buildDemoWorld(Engine& e, DemoWorld& world);

/// Drive the kinematic platform. One fixed step's worth.
void stepDemoWorld(Engine& e, DemoWorld& world, float step);

/**
 * @brief A unit cube, as `createMesh` wants it (G4).
 *
 * Twenty-four vertices rather than eight, because a cube's normals are per face and a
 * shared corner cannot carry three of them. This is the smallest honest piece of
 * procedural geometry there is, and what it demonstrates is that a mesh made in code
 * lands in the same buffers, draws through the same pipelines and is freed by the same
 * call as one loaded from a file.
 *
 * Here rather than in `DemoGame.cpp` because it acquired a second caller: the crates, the
 * ramp and the platform are all boxes, and the spawned cube on `F` was the first.
 */
[[nodiscard]] scene::MeshData unitCube(uint32_t material, const glm::mat4& transform);
