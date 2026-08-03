#include "gfx/DebugView.h"

namespace gfx {

namespace {

/// The one list. Order is the enum's, which is also the order `lighting_body.glsl`
/// branches on and the order `advanceDebugView` walks, so the names, the shader and the
/// F-key cycle cannot disagree about what comes after what.
constexpr core::Named<DebugView> kDebugViews[] = {
    {"lit", DebugView::Lit},           {"albedo", DebugView::Albedo},
    {"normal", DebugView::Normal},     {"orm", DebugView::Orm},
    {"depth", DebugView::Depth},       {"emissive", DebugView::Emissive},
    {"ssao", DebugView::Ssao},         {"edges", DebugView::Edges},
};
static_assert(core::namesEveryValue(kDebugViews), "a view reachable from the enum and from no name");

/// `none` is an input convenience for `clamp` and is second, so it parses and is never
/// written back: what a save writes is the canonical spelling.
constexpr core::Named<TonemapOperator> kTonemaps[] = {
    {"aces", TonemapOperator::Aces},
    {"reinhard", TonemapOperator::Reinhard},
    {"clamp", TonemapOperator::Clamp},
    {"none", TonemapOperator::Clamp},
};
static_assert(core::namesEveryValue(kTonemaps), "an operator reachable from the enum and from no name");

} // namespace

core::Names<DebugView> debugViewNames() {
    return kDebugViews;
}

core::Names<TonemapOperator> tonemapNames() {
    return kTonemaps;
}

const char* debugViewKey(DebugView view) {
    return core::nameOf(debugViewNames(), view);
}

DebugView advanceDebugView(DebugView view, int step) {
    const int count = static_cast<int>(DebugView::Count);
    // Both operands are reduced before they are summed, so the sum sits in
    // (-count, 2*count) and the single `+ count` is enough to make the last modulo
    // non-negative. Reducing only one of them is the version that looks right and lets a
    // large negative `step` fall off the front.
    const int index = (static_cast<int>(view) % count + step % count + count) % count;
    return static_cast<DebugView>(index);
}

const char* tonemapKey(TonemapOperator op) {
    return core::nameOf(tonemapNames(), op);
}

} // namespace gfx
