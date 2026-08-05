#pragma once

#include "core/Names.h"

#include <cstdint>

/**
 * @file engine/scene/AudioBackend.h
 * @brief Where the mix goes, in a header with no miniaudio and no `scene/Node.h` in it.
 *
 * It sits apart from `Audio.h` for the reason [gfx/DebugView.h](../gfx/DebugView.h) sits
 * apart from `Renderer.h`, stated there: the config parser has to turn `"backend": "null"`
 * into this value, and `core/Config.h` should not pull a scene graph and rapidjson behind
 * it to name three states.
 *
 * The names used to be written twice -- three string comparisons in `AudioEngine::init`
 * with their own *"unknown backend -- using auto"* warning, and nothing at all on the
 * config side, so `"backend": "devcie"` opened a device anyway and said so much later. One
 * list (D12), one refusal, at the door.
 */
namespace core {

/// `auto` opens a real device and falls back to the device-less mix if that fails, which
/// is what makes a headless or a sound-cardless machine run the whole audio path rather
/// than a different one. `null` is miniaudio's `noDevice`: every decoder, filter and bus
/// still runs and nothing reaches a speaker.
enum class AudioBackend : uint32_t {
    Auto = 0,
    Device = 1,
    Null = 2,
    Count = 3,
};

/// Every spelling `"audio": {"backend": ...}` accepts, canonical first.
[[nodiscard]] core::Names<AudioBackend> audioBackendNames();

} // namespace core
