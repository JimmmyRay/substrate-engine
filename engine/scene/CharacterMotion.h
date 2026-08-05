#pragma once

/**
 * @file engine/scene/CharacterMotion.h
 * @brief What a character controller did during the step just taken.
 *
 * Its own header, and description rather than simulation, because it is the whole of what
 * one module has to tell another: the solver fills it and the animation state machine reads
 * it, and neither may name the other. A field added here that a game could instead compute
 * from an input map does not belong -- see `anim::LocomotionDriver`.
 */
namespace scene {

struct CharacterMotion {
    /// Horizontal, ground-relative, in m/s.
    float speed = 0.0f;
    /// The collider's own top speed, which `speed` is normalised against. Zero for a
    /// controller that was never given one, which reads as "no speed parameter to write".
    float topSpeed = 0.0f;
    bool onGround = false;
    /// The step the controller launched on. **Not derivable from a jump key and the ground
    /// state**: a coyote window and a jump buffer make the two disagree by design.
    bool jumped = false;
};

} // namespace scene
