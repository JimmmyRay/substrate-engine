#pragma once

#include "scene/InstanceTable.h"
#include "scene/Scene.h"
#include "ui/Ui.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file Inspector.h
 * @brief Select an object and edit its properties.
 *
 * The instance inspector writes a matrix's translation column directly. Decompose to TRS and
 * recompose instead and the round trip is lossy for any matrix not built as
 * translate*rotate*scale, and lossy again on every frame of a drag, so the object deforms while
 * it is moved -- which is why rotation and scale are readouts there and editable on a node,
 * whose transform is stored as TRS already. Rotation stays a readout in both: three sliders
 * over a quaternion's components leave it unnormalised between any two frames of a drag.
 *
 * A node attached to a dynamic body or a character takes its world transform from the solver
 * and never writes its local TRS back, so those sliders move numbers nothing reads. The panel
 * says so in a row rather than disabling the widget.
 */
namespace ui {

/// @brief What the inspector keeps between frames. The caller must hold it across frames; a
/// selection that resets each frame is not a selection.
struct InspectorState {
    /// Slot index, not an `InstanceId`. Make it an id and the selection empties the moment the
    /// object under it is destroyed; a slot instead keeps selecting whatever occupies it now.
    uint32_t selected = 0;

    /// Item captions for the list widget, rebuilt only when the table's revision moves.
    std::vector<std::string> names;
    /// Slot behind each entry of `names`. Holes make the two orders differ, which is why the
    /// selection cannot be indexed straight into the table.
    std::vector<uint32_t> slots;
    /// Revision `names` was built from. `0` is "never", which no live table reports.
    uint64_t namesRevision = 0;

    /// How far an object may be dragged from where it sits, in metres. A range *around* the
    /// current value, not an absolute extent, so it needs no bounds query and behaves the same
    /// in a one-metre scene and a Sponza-sized one.
    float reach = 10.0f;
};

/**
 * @brief One frame of the instance inspector.
 *
 * @return true when the selected instance's transform was edited this frame, so a caller
 *         that has to react -- re-seeding a physics body from it, say -- can, without
 *         diffing the matrix itself.
 *
 * Every edit goes through `setTransform` rather than into the matrix, so the table's revision
 * moves and whatever caches off it invalidates.
 */
bool drawInstanceInspector(Context& ui, scene::InstanceTable& instances, InspectorState& state, const glm::vec2& pos,
                           const glm::vec2& size);

/// The caption for one slot, exposed for the test that pins it.
[[nodiscard]] std::string instanceCaption(const scene::InstanceTable& instances, uint32_t slot);

/// @brief What the node inspector keeps between frames. Merging it into `InspectorState` makes
/// a game with one panel open pay for the closed one's caption cache.
struct NodeInspectorState {
    /// Index into `names` -- the listing -- and neither a slot nor a `NodeId`. A `NodeId` goes
    /// stale when the node is destroyed and a slot survives that but not a reparent, which
    /// reorders the listing without destroying anything; the row index survives both.
    uint32_t selected = 0;

    /// Captions in listing order, rebuilt only when the tree's structure moves.
    std::vector<std::string> names;
    /// The node behind each caption. Held rather than re-derived: the walk that produced the
    /// order is the only thing that knows it.
    std::vector<scene::NodeId> nodes;
    /// Depth behind each caption, so the detail pane can say it without walking up.
    std::vector<uint32_t> depths;
    /// `Scene::structureRevision` the three vectors were built from. `0` is "never", which no
    /// scene reports.
    uint64_t structureRevision = 0;

    /// How far a node may be dragged from where it sits, in metres. A range around the current
    /// value, like `InspectorState::reach`.
    float reach = 10.0f;
};

/**
 * @brief One frame of the scene-tree inspector.
 *
 * @return true when the selected node's local transform was edited this frame, so a caller
 *         that has to react can, without diffing it.
 *
 * Every edit goes through `setLocalPosition` / `setLocalScale`, so the dirty bit, the sweep and
 * the push happen as they do for a game's own write. Hold a transform here instead and it goes
 * stale against the solver.
 */
bool drawNodeInspector(Context& ui, scene::Scene& scene, NodeInspectorState& state, const glm::vec2& pos,
                       const glm::vec2& size);

/**
 * @brief The caption for one node, exposed for the test that pins it.
 *
 * `depth` is leading indent. The letters after the name are the attachment record: `M` mesh,
 * `B` body, `C` character, `S` sound, `L` light, `E` emitter.
 */
[[nodiscard]] std::string nodeCaption(const scene::Scene& scene, scene::NodeId id, uint32_t depth);

} // namespace ui
