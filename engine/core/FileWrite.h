#pragma once

#include "core/Logger.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace core {

/**
 * @file engine/core/FileWrite.h
 * @brief Replace a file's contents without ever being able to destroy them (G2).
 *
 * Four callers write a file the program would rather lose than truncate -- a save, the
 * scene cache, the rebound actions, and the settings table -- and all four wrote the same
 * twenty lines: open `path.tmp`, write, remove the temp if the write failed, rename it
 * over `path`, remove it if the rename failed. The Rule of Threes had already fired at the
 * third; the fourth is this file.
 *
 * The pattern is not an economy of typing. Opening the real file with `trunc` empties it
 * before there is any way to know the write will succeed, so a full disk takes the
 * player's save, or their whole settings file, with it. A rename is atomic on every
 * filesystem this runs on: afterwards there is either the old file or the new one, and
 * never half of either.
 *
 * A global function rather than a method, which is the narrowest scope that reaches all
 * four: they span `core::SaveWriter`, `core::input::saveBindings`,
 * `core::settings::Settings` and `scene::writeSceneCache` -- three namespaces, and no one
 * of them is a natural owner.
 */

/**
 * @brief Write `bytes` to `path` by way of `path.tmp`, renamed over it.
 *
 * @param what a noun for the failure messages -- `"save"`, `"scene cache"` -- so a log
 *        line says which of the four this was without the reader tracing the path back.
 * @return false with the reason logged, and with no temp file left behind. `path` is
 *         untouched on every failure path, which is the entire point.
 */
bool writeFileAtomically(const std::filesystem::path& path, const void* bytes, size_t count, LogCategory category,
                         const char* what);

/// Text overload, for the two callers holding a `std::string` of JSON.
inline bool writeFileAtomically(const std::filesystem::path& path, std::string_view text, LogCategory category,
                                const char* what) {
    return writeFileAtomically(path, text.data(), text.size(), category, what);
}

} // namespace core
