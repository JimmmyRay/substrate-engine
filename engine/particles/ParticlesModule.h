#pragma once

#include "particles/ParticleSystem.h"

/**
 * @file engine/particles/ParticlesModule.h
 * @brief What links particles into a game, and the only header that names both halves.
 *
 * Calling `Engine::particles()` is what pulls `ParticlesModule.cpp` in and runs the
 * registrar that aims `modules::particles` at a real system; a game that never names it
 * steps and draws nothing.
 *
 * Folding that registrar into `ParticleSystem.cpp` gives it `Engine`, which drops it out
 * of `SUBSTRATE_HOSTED_SOURCES` and out of every sanitized unit run.
 */
