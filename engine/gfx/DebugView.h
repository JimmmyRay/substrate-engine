#pragma once

#include "core/Names.h"

#include <cstdint>

/**
 * @file engine/gfx/DebugView.h
 * @brief The two selectors that name a shader variant, in a header with no Vulkan in it.
 *
 * They live apart from `Renderer.h` for one reason: the config parser has to turn
 * `--debug-view ssao` and `"tonemap": "reinhard"` into these values, and
 * `engine/core/Config.cpp` is one of `SUBSTRATE_HOSTED_SOURCES` -- the translation units
 * that pull in neither Vulkan nor a window, which is what lets `./test.sh tsan` run at all.
 *
 * Before this header existed the parser returned bare integers instead, and the cost of
 * that was stated rather than hypothetical: a ten-branch chain returning `0..9` that never
 * named the enum, so reordering `DebugView` would have silently mis-mapped every
 * `--debug-view` flag with nothing to catch it.
 *
 * Each enum below has **one** name list (D12), in the `.cpp` beside it, and both
 * directions come out of it -- see [core/Names.h](../core/Names.h). The alias table that
 * used to sit in `Config.cpp` beside `tonemapKey` is gone, because two lists of one
 * operator's spellings are two lists free to disagree.
 */
namespace gfx {

/// Must match TONEMAP_OPERATOR in tonemap.frag. One selector rather than three
/// booleans: the operators are mutually exclusive, so this is one variant each
/// instead of eight combinations, five of which mean nothing.
enum class TonemapOperator : uint32_t {
    Aces = 0,     ///< Narkowicz's filmic approximation. The default.
    Reinhard = 1, ///< For comparison: desaturates highlights where ACES holds hue.
    Clamp = 2,    ///< No curve. What the HDR buffer actually contains, blown out.
    Count = 3,
};

/// Must match the `debugView ==` chain in lighting_body.glsl. Every render target the
/// renderer produces is reachable from here, which is what makes frame capture (5.2)
/// able to screenshot "any attachment" while only ever reading back one image.
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

/// Every spelling `--debug-view` accepts, canonical first. The one list; `debugViewKey`
/// and every parse are derived from it.
[[nodiscard]] core::Names<DebugView> debugViewNames();

/// Every spelling `"tonemap"` and `--tonemap` accept, canonical first. `none` is here
/// beside `clamp` rather than in a second table in the config parser.
[[nodiscard]] core::Names<TonemapOperator> tonemapNames();

/// The name the config file and `--debug-view` spell it with -- `nameOf(debugViewNames(),
/// view)`, kept as a name because three call sites read better for it. `Count` yields
/// nullptr, which is what lets a lookup loop terminate on it rather than on a separate
/// length.
[[nodiscard]] const char* debugViewKey(DebugView view);

/**
 * @brief The view `step` places along the list, wrapping in both directions.
 *
 * Here rather than in `Renderer` because this is the half worth pinning and `Renderer.cpp`
 * needs a device to link. The order it walks is the enum's, which is also the order
 * `lighting_body.glsl` branches on and the order `--debug-view <name>` resolves against --
 * so a test over this is a test over the thing four golden cases already photograph.
 *
 * `step` may be any magnitude and any sign; it is reduced before it is added, so no input
 * can leave the list. `Count` is not a view and is never returned.
 */
[[nodiscard]] DebugView advanceDebugView(DebugView view, int step);

/// The name `"tonemap"` and `--tonemap` spell it with.
[[nodiscard]] const char* tonemapKey(TonemapOperator op);

} // namespace gfx
