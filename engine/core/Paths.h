#pragma once

#include <filesystem>

namespace core {

/**
 * @file engine/core/Paths.h
 * @brief Where this binary is, so a package can find what ships beside it.
 *
 * The engine already answers "where does this come from" twice -- `readShaderBinary` for
 * SPIR-V and `Resources` for assets -- and both answer it with an absolute directory baked
 * in at build time, each behind a relative fallback. That works exactly as long as the
 * binary never leaves the tree it was built in.
 *
 * A packaged build is the case where it does. Configured with the relative fallbacks, both
 * lookups become `executableDir() / "shaders"` and `executableDir() / "assets"`, and
 * because `std::filesystem::operator/` *replaces* the left operand when the right one is
 * absolute, the very same expression is a no-op in a development build:
 *
 *     executableDir() / "/abs/path/to/build/debug/shaders"  ->  the absolute path
 *     executableDir() / "shaders"                           ->  beside the binary
 *
 * So there is one expression, no `#ifdef`, no runtime flag, and shader hot reload in the
 * dev tree is untouched.
 *
 * Not a class and not a namespace: one stateless function, which is the narrowest scope
 * that reaches all of its callers. They span `gfx::readShaderBinary`, `Renderer`'s two
 * reload paths, `Resources` and `Engine::init` -- four functions in three classes across
 * two namespaces, so a local function or a private method cannot reach them, and no one of
 * those is a natural owner for a public one.
 */

/**
 * @brief Directory holding the running executable, as an absolute path.
 *
 * Resolved once and cached; every call after the first is a reference read. Falls back to
 * the current working directory if the platform lookup fails, which keeps a wrong answer
 * to "where am I" from becoming a crash.
 *
 * @see seedExecutablePath, which should run before the first call in a hosted process.
 */
[[nodiscard]] const std::filesystem::path& executableDir();

/**
 * @brief Offer `argv[0]` as the fallback for `executableDir()`.
 *
 * Only used when the platform lookup fails, and it can fail: `/proc` is not mounted in
 * every container. Call it from `main` before anything else asks, because the answer is
 * cached on first use and this cannot retroactively correct it.
 *
 * Harmless to call late, harmless not to call at all, and harmless to call with nullptr.
 */
void seedExecutablePath(const char* argv0);

} // namespace core
