#pragma once

#include <filesystem>

namespace core {

/**
 * @file engine/core/Paths.h
 * @brief Where this binary is, so a package can find what ships beside it.
 *
 * Every lookup that resolves a build-time root writes `executableDir() / root`, which needs
 * no `#ifdef` because `std::filesystem::operator/` *replaces* the left operand when the
 * right one is absolute:
 *
 *     executableDir() / "/abs/path/to/build/debug/shaders"  ->  the absolute path
 *     executableDir() / "shaders"                           ->  beside the binary
 */

/**
 * @brief Directory holding the running executable, as an absolute path.
 *
 * Resolved once and cached, so the answer cannot be corrected after the first call. Falls
 * back to the working directory when the platform lookup fails.
 *
 * @see seedExecutablePath, which must run before the first call in a hosted process.
 */
[[nodiscard]] const std::filesystem::path& executableDir();

/// @brief Offer `argv[0]` as the fallback for `executableDir()`, used only when the
/// platform lookup fails -- `/proc` is not mounted in every container.
///
/// Call it from `main` before anything else asks: `executableDir()` caches on first use and
/// this cannot correct it afterwards. Harmless late, absent, or with nullptr.
void seedExecutablePath(const char* argv0);

} // namespace core
