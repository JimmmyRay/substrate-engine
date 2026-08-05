#include "core/DebugView.h"

namespace core {

namespace {

/// Must stay in the enum's order: `lighting_body.glsl` branches on it and
/// `advanceDebugView` walks it, so reordering here moves the F-key cycle out of step with
/// the shader.
constexpr core::Named<DebugView> kDebugViews[] = {
    {"lit", DebugView::Lit},           {"albedo", DebugView::Albedo},
    {"normal", DebugView::Normal},     {"orm", DebugView::Orm},
    {"depth", DebugView::Depth},       {"emissive", DebugView::Emissive},
    {"ssao", DebugView::Ssao},         {"edges", DebugView::Edges},
};
static_assert(core::namesEveryValue(kDebugViews), "a view reachable from the enum and from no name");

/// `none` must stay second: the first name for a value is what a save writes back.
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
    // *Both* operands are reduced before the sum, so it sits in (-count, 2*count) and one
    // `+ count` makes the last modulo non-negative. Reducing only one looks right and lets
    // a large negative `step` fall off the front.
    const int index = (static_cast<int>(view) % count + step % count + count) % count;
    return static_cast<DebugView>(index);
}

const char* tonemapKey(TonemapOperator op) {
    return core::nameOf(tonemapNames(), op);
}

} // namespace core
