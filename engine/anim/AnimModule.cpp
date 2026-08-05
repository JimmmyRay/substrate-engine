#include "anim/AnimModule.h"

#include "Engine.h"
#include "Modules.h"

#include <utility>
#include <vector>

namespace anim {

namespace {

/// The engine's one animator and the driver that writes its parameters. File-scope rather
/// than `Engine` members, which would name `anim::SceneAnimator` in `Engine.h` and link
/// animation into every game.
SceneAnimator g_animator;
LocomotionDriver g_locomotion;

/// The renderer's view of `g_animator`, rebuilt by every call below that can move a block.
/// Rebuilding is cheap and getting it wrong is not: a span left over a character that has
/// been created reads freed memory and uploads it as a pose.
std::vector<gfx::SkinCharacter> g_skinCharacters;

void refreshSkinCharacters() {
    g_skinCharacters.clear();
    g_skinCharacters.reserve(g_animator.characterCount());
    for (uint32_t c = 0; c < g_animator.characterCount(); ++c) {
        g_skinCharacters.push_back({g_animator.jointOffset(c), g_animator.weightOffset(c),
                                    g_animator.jointMatrices(c), g_animator.morphWeights(c)});
    }
}

struct Module final : modules::Anim {
    void init(scene::AnimationRig rig) override {
        g_animator.init(std::move(rig));
        refreshSkinCharacters();
    }

    uint32_t merge(const scene::AnimationRig& extra) override {
        const uint32_t skinBase = g_animator.merge(extra);
        refreshSkinCharacters();
        return skinBase;
    }

    scene::AnimatorId create(uint32_t skin) override {
        const scene::AnimatorId id = g_animator.create(skin);
        refreshSkinCharacters();
        return id;
    }

    scene::AnimatorId createMorphed(uint32_t targets) override {
        const scene::AnimatorId id = g_animator.createMorphed(targets);
        refreshSkinCharacters();
        return id;
    }

    void destroy(scene::AnimatorId id) override {
        g_animator.destroy(id);
        refreshSkinCharacters();
    }

    [[nodiscard]] Stats stats() const override {
        Stats s;
        s.characters = g_animator.characterCount();
        s.clips = static_cast<uint32_t>(g_animator.clipCount());
        s.joints = g_animator.totalJoints();
        s.weights = g_animator.totalWeights();
        s.empty = g_animator.empty();
        return s;
    }

    void pairController(uint64_t controller, uint32_t slot) override {
        const scene::AnimatorId rig = g_animator.characterAt(slot);
        if (!rig.valid()) return;
        g_locomotion.pairKeyed(controller, rig);
    }

    void update(float dt) override { g_animator.update(dt); }

    void updateLocomotion(const LocomotionDriver::MotionSlot& motion) override {
        g_locomotion.update(motion, g_animator);
    }

    [[nodiscard]] core::Slot<bool(uint32_t, glm::mat4*)> poses() override {
        return core::Slot<bool(uint32_t, glm::mat4*)>(
            [](void*, uint32_t node, glm::mat4* out) {
                // Resolved per node, because a `ParticleEmitter` and an `AudioSourceDesc`
                // carry a bare node index and nothing saying which rig owns it -- resolving
                // them all against character 0 puts the second character's torch in the
                // first one's hand. Character 0 is still the answer for a node nothing
                // animates, which every character resolves identically.
                const scene::AnimatorId owner = g_animator.characterForNode(node);
                const std::vector<glm::mat4>& world =
                    g_animator.worldTransforms(owner.valid() ? owner : g_animator.characterAt(0));
                if (node >= world.size()) return false;
                *out = world[node];
                return true;
            },
            nullptr);
    }

    [[nodiscard]] std::span<const gfx::SkinCharacter> skinCharacters() const override { return g_skinCharacters; }
};

Module g_module;

/// Assign `modules::anim` from a header instead and any transitive include links animation
/// into a game that never asked for it.
struct Registrar {
    Registrar() { modules::anim = &g_module; }
};

const Registrar g_registrar;

} // namespace

} // namespace anim

// Defining these in Engine.cpp instead links animation into every binary -- Engine.cpp is in
// all of them and this file is not.
::anim::SceneAnimator& Engine::animator() {
    return ::anim::g_animator;
}

::anim::LocomotionDriver& Engine::locomotion() {
    return ::anim::g_locomotion;
}
