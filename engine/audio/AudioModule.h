#pragma once

#include "audio/Audio.h"

/**
 * @file engine/audio/AudioModule.h
 * @brief What links audio into a game, and the only header that names both halves.
 *
 * Calling `Engine::audio()` is what pulls `AudioModule.cpp` in and runs the registrar that
 * aims `modules::audio` at a real mixer; a game that never names it opens no device, loads
 * no source and hears nothing.
 *
 * Folding that registrar into `Audio.cpp` gives it `Engine`, which drops it out of
 * `SUBSTRATE_HOSTED_SOURCES` and out of every sanitized unit run.
 */
