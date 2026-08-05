#pragma once

#include "core/Names.h"

#include <cstdint>

/**
 * @file engine/core/AudioBackend.h
 * @brief Where the mix goes. Kept free of miniaudio and `scene/Node.h` so `core/Config.h`
 * can name the three states without pulling either behind it.
 */
namespace core {

/// `Auto` falls back to `Null` when no device opens; `Null` is miniaudio's `noDevice`, so
/// the whole mix still runs and nothing reaches a speaker.
enum class AudioBackend : uint32_t {
    Auto = 0,
    Device = 1,
    Null = 2,
    Count = 3,
};

/// Every spelling `"audio": {"backend": ...}` accepts, canonical first. Dropping one here
/// makes an existing config file fail to parse.
[[nodiscard]] core::Names<AudioBackend> audioBackendNames();

} // namespace core
