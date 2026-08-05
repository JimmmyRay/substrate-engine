#pragma once

#include "Engine.h"

#include <glm/glm.hpp>

#include <vector>

/**
 * @file game/demo/DemoWorld.h
 * @brief The demo's own world, built in code against the engine's public surface.
 */

struct DemoWorld {
    /// Sentinel index: no such light.
    static constexpr uint32_t kNoLight = ~0u;

    bool built = false;

    /**
     * @brief The fill light `E` reparents onto the character, into `Renderer::lights`.
     *
     * A fill and not a brazier cone: carrying a brazier's light away leaves a vessel
     * visibly on fire and lighting nothing.
     */
    uint32_t torchLight = kNoLight;

    scene::BodyId platform;
    scene::NodeId platformNode;
    glm::vec3 platformCentre{0.0f};
    /// Half the travel, along Z: the platform slides between `centre - this` and
    /// `centre + this`.
    float platformTravel = 0.0f;
    /// Seconds since the world was built, advanced on the fixed step. Advancing it on the
    /// frame delta instead makes the platform a function of frame rate and a `--locked`
    /// run stops being reproducible.
    float clock = 0.0f;

    /// The character driving the banner's two morph weights.
    scene::AnimatorId banner;

    /**
     * @brief A player: the controller they drive, the node it drives, and the pose it blends.
     *
     * `rig` is the rig the merge appended, not `characterAt(0)` -- Sponza's own morphed props
     * take the low indices, so the imported rig's index moves the moment the scene gains
     * another morph target. It is invalid for a character a file authored, which brings a
     * collider and no skin.
     */
    struct Player {
        scene::PhysicsCharacterId character;
        scene::NodeId node;
        scene::AnimatorId rig;
    };

    /// Everyone playing, in the order they were named. Empty is a legal answer.
    std::vector<Player> players;

    /// Player `p`'s controller, or an invalid handle when there is no such player.
    [[nodiscard]] scene::PhysicsCharacterId playerCharacter(uint32_t p = 0) const {
        return p < players.size() ? players[p].character : scene::PhysicsCharacterId{};
    }
    /// Player `p`'s node, or an invalid handle when there is no such player.
    [[nodiscard]] scene::NodeId playerNode(uint32_t p = 0) const {
        return p < players.size() ? players[p].node : scene::NodeId{};
    }
    /// Player `p`'s rig, or an invalid handle for a character that brought no skin.
    [[nodiscard]] scene::AnimatorId playerRig(uint32_t p = 0) const {
        return p < players.size() ? players[p].rig : scene::AnimatorId{};
    }

    /// The dust a landing throws: a template, copied and re-positioned per impact.
    scene::ParticleEmitter dust;
    bool dustReady = false;
};

/// How much bigger than authored `DemoGame::init` imports Sponza.
constexpr float kDemoWorldScale = 2.0f;

/// Build all of it. Safe to call every frame; it does its work once.
void buildDemoWorld(Engine& e, DemoWorld& world);

/// Drive the kinematic platform. One fixed step's worth.
void stepDemoWorld(Engine& e, DemoWorld& world, float step);

/**
 * @brief A unit cube, as `createMesh` wants it.
 *
 * Twenty-four vertices rather than eight: a cube's normals are per face and a shared corner
 * cannot carry three of them.
 */
[[nodiscard]] scene::MeshData unitCube(uint32_t material, const glm::mat4& transform);
