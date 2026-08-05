#pragma once

#include "core/Logger.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace core {

/**
 * @file engine/core/FileWrite.h
 * @brief Replace a file's contents without ever being able to destroy them.
 *
 * Anything the program would rather lose than truncate goes through here. Opening the real
 * file with `trunc` empties it before there is any way to know the write will succeed, so a
 * full disk takes the player's save or their whole settings file with it. A rename is
 * atomic on every filesystem this runs on.
 */

/**
 * @brief Write `bytes` to `path` by way of `path.tmp`, renamed over it.
 *
 * @param what a noun for the failure messages -- `"save"`, `"scene cache"`.
 * @return false with the reason logged, no temp file left behind, and `path` untouched.
 */
bool writeFileAtomically(const std::filesystem::path& path, const void* bytes, size_t count, LogCategory category,
                         const char* what);

/// Text overload, for the two callers holding a `std::string` of JSON.
inline bool writeFileAtomically(const std::filesystem::path& path, std::string_view text, LogCategory category,
                                const char* what) {
    return writeFileAtomically(path, text.data(), text.size(), category, what);
}

} // namespace core
