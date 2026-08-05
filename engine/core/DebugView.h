#pragma once

#include "core/Names.h"

#include <cstdint>

/**
 * @file engine/core/DebugView.h
 * @brief The two selectors that name a shader variant, in a header with no Vulkan in it.
 *
 * Kept out of `Renderer.h` so the config parser can turn `--debug-view ssao` into a value:
 * `engine/core/Config.cpp` is one of `SUBSTRATE_HOSTED_SOURCES`, which pull in neither
 * Vulkan nor a window and are what `./test.sh tsan` can run.
 *
 * Each enum below has one name list, in the `.cpp` beside it, and both directions come out
 * of it -- see [core/Names.h](Names.h).
 */
namespace core {

/// Must match TONEMAP_OPERATOR in tonemap.frag: these values are the shader's, so
/// reordering here silently selects a different curve.
enum class TonemapOperator : uint32_t {
    Aces = 0,     ///< Narkowicz's filmic approximation. The default.
    Reinhard = 1, ///< For comparison: desaturates highlights where ACES holds hue.
    Clamp = 2,    ///< No curve. What the HDR buffer actually contains, blown out.
    Count = 3,
};

/// Must match the `debugView ==` chain in lighting_body.glsl: these values are the
/// shader's, so reordering here silently mis-maps every `--debug-view` flag.
enum class DebugView : uint32_t {
    Lit = 0,
    Albedo = 1,
    Normal = 2,
    Orm = 3,
    Depth = 4,
    Emissive = 5,
    Ssao = 6,  ///< the blurred AO buffer, white where SSAO is compiled out
    Edges = 7, ///< pixels 3.6 must shade per-sample, in red
    Count = 8,
};

/// Every spelling `--debug-view` accepts, canonical first. `debugViewKey` and every parse
/// derive from it; a second list is one free to disagree.
[[nodiscard]] core::Names<DebugView> debugViewNames();

/// Every spelling `"tonemap"` and `--tonemap` accept, canonical first.
[[nodiscard]] core::Names<TonemapOperator> tonemapNames();

/// The name the config file and `--debug-view` spell it with. `Count` yields nullptr, which
/// is what lets a lookup loop terminate on it rather than on a separate length.
[[nodiscard]] const char* debugViewKey(DebugView view);

/// @brief The view `step` places along the list, wrapping in both directions.
///
/// `step` may be any magnitude and any sign; it is reduced before it is added, so no input
/// can leave the list. `Count` is not a view and is never returned.
[[nodiscard]] DebugView advanceDebugView(DebugView view, int step);

/// The name `"tonemap"` and `--tonemap` spell it with.
[[nodiscard]] const char* tonemapKey(TonemapOperator op);

} // namespace core
