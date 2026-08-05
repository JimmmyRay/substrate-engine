#pragma once

#include "anim/Locomotion.h"
#include "anim/SceneAnimator.h"

/**
 * @file engine/anim/AnimModule.h
 * @brief What links animation into a game, and the only header that names both halves.
 *
 * Calling `Engine::animator()` or `Engine::locomotion()` is what pulls `AnimModule.cpp` in
 * and runs the registrar that aims `modules::anim` at a real animator; a game that names
 * neither steps no clip, and every instance draws its bind pose.
 *
 * Folding that registrar into `SceneAnimator.cpp` gives it `Engine`, which would leave the
 * animator out of every link that has no device -- the unit suite, the baker and the sim
 * loop -- and so out of every sanitized run.
 */
