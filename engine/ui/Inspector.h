#pragma once

#include "scene/InstanceTable.h"
#include "scene/Scene.h"
#include "ui/Ui.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file Inspector.h
 * @brief Select an object and edit its properties (5.6).
 *
 * ## What was actually missing, and what it turned out to be
 *
 * 5.6 waited on S6 for four things -- rects, layout, hit testing and text input -- and
 * the roadmap was explicit that once those landed, what remained was 5.6's own: **a way
 * to name the properties of a thing, so a panel can list them.**
 *
 * The answer is a function that names them. `drawInstanceInspector` writes out what an
 * instance has, one widget per property, in the order a reader wants them. There is no
 * property registry, no reflection macro, no `describe<T>()` and no field table -- and
 * that is not an economy, it is the same decision S6 made about the widget tree. A
 * property system is a schema plus a type-erased setter plus a name-to-offset map, which
 * is three abstractions to save writing `ui.slider("X", p.x, ...)`. **The third thing
 * worth inspecting is when to look again**; two is a coincidence, and the second one will
 * be a second function.
 *
 * The distinction from a game's own `drawSettingsPanel` is worth stating because both
 * are panels: that one is the *game's* interface and names a particular set of renderer
 * toggles, so it belongs to the application. This one inspects `InstanceTable`, which is
 * an engine data structure, so it belongs to the engine -- and being hosted, the unit
 * suite reaches it.
 *
 * ## Editing a transform without decomposing it
 *
 * The position sliders write the matrix's **translation column directly** rather than
 * decomposing to TRS and recomposing. That is exact: rotation, scale and any shear
 * survive untouched, and dragging a slider back to where it started restores the matrix
 * bit for bit. A decompose-edit-recompose round trip does neither -- it is lossy for any
 * matrix that was not built as translate*rotate*scale, and lossy *again* on every frame
 * of a drag, so an object slowly deforms while you move it.
 *
 * The cost is stated: rotation and scale are **read-only here**. Making them editable
 * means owning a TRS decomposition per selected object, seeded on selection and written
 * back whole, which is a real design with a real answer and not one this row needs to
 * pick before something asks for it.
 *
 * ## The second thing, and the prediction it settles (G6)
 *
 * `drawNodeInspector` is that second function, written the way the first paragraph above
 * said it would be. G3 gave the engine a scene tree and G6 is how a game *reaches* it --
 * hierarchy, local TRS and the attachment record, listed by a function that names them.
 *
 * **The trigger stands and has not fired.** Two things are inspected now; the registry
 * becomes the right answer at the third, and the sentence above is the test to re-read then
 * rather than a formality. What the second function actually shares with the first is worth
 * recording, because it is less than it looks: a caption cache keyed on a revision, and a
 * selection index that clamps. Both are four lines and neither is the schema a registry
 * would be. What they do *not* share is the interesting half -- an instance is a flat row
 * of a table and a node is a tree, so this list is built by a walk and that one by a scan,
 * and every property below is a different property with a different way of being wrong.
 *
 * ## What a node inspector can write, and the one thing it must admit
 *
 * The instance inspector edits the translation column of a matrix, because decomposing one
 * is lossy. A node stores translation, rotation and scale *as* translation, rotation and
 * scale, so writing one back is exact and there is no round trip to avoid -- which is why
 * position and scale are both editable here and only position is there. Rotation stays a
 * readout for a different reason than the one above: three sliders over a quaternion's
 * components produce an unnormalised quaternion between any two frames of a drag, and Euler
 * angles are a second representation to keep in step with the first.
 *
 * The honest readout this one owes instead is **driven**. `Scene` says a node attached to a
 * dynamic body or a character takes its world transform from the solver verbatim and never
 * writes the local TRS back, so for such a node the numbers below are the ones it was
 * created with and dragging them changes nothing anybody can see. That is not something to
 * hide behind a disabled widget; it is a fact about the node, and it gets a row -- the same
 * call the instance inspector's `visible: gpu-side` row makes.
 */
namespace ui {

/**
 * @brief What the inspector keeps between frames.
 *
 * The same bargain `PanelState` makes: `ui::Context` owns interaction state -- hovered,
 * held, focused -- and this owns the selection, which is the application's. Held by the
 * caller across frames because a selection that reset every frame is not a selection.
 */
struct InspectorState {
    /// Slot index, not an `InstanceId`. A selection is a thing on screen the user is
    /// pointing at, and it has to survive the object under it being destroyed and the
    /// slot reused -- which is exactly what an id is designed to *stop* being valid
    /// across. The list re-derives what is live every frame; a stale index selects
    /// whatever occupies the slot now, which is what a slot list should do.
    uint32_t selected = 0;

    /// Item captions for the list widget, rebuilt only when the table's revision moves.
    /// Cached rather than rebuilt per frame because a caption is a `std::string` per live
    /// instance and the table is static in almost every frame of almost every scene.
    std::vector<std::string> names;
    /// Slot behind each entry of `names`, so a selection in list order resolves back to
    /// the table. Holes make these two differ, which is the whole reason it exists.
    std::vector<uint32_t> slots;
    /// Revision `names` was built from. `0` is "never", which no live table reports.
    uint64_t namesRevision = 0;

    /// How far an object may be dragged from where it sits, in metres. A slider needs a
    /// range and a scene has no natural one, so this is the range *around* the current
    /// value rather than an absolute extent -- which means it works the same in a
    /// one-metre scene and a Sponza-sized one, and needs no bounds query to set up.
    float reach = 10.0f;
};

/**
 * @brief One frame of the instance inspector.
 *
 * @return true when the selected instance's transform was edited this frame, so a caller
 *         that has to react -- re-seeding a physics body from it, say -- can, without
 *         diffing the matrix itself.
 *
 * Takes the table by mutable reference because editing is the point. Everything it writes
 * goes through `setTransform`, which is 4.1b's property (iii) -- the one that has to hold
 * for entt to ever sit beside this table -- being used rather than merely asserted.
 */
bool drawInstanceInspector(Context& ui, scene::InstanceTable& instances, InspectorState& state, const glm::vec2& pos,
                           const glm::vec2& size);

/// The caption for one slot, exposed for the test that pins it. Live, blended and
/// deformed instances read differently at a glance, which is the whole job of a list
/// entry -- a column of `slot 0`, `slot 1`, `slot 2` tells a reader nothing.
[[nodiscard]] std::string instanceCaption(const scene::InstanceTable& instances, uint32_t slot);

// --------------------------------------------------------------------------- the tree

/**
 * @brief What the node inspector keeps between frames.
 *
 * Deliberately not `InspectorState` with two more vectors on it. A game may open one panel,
 * the other, both or neither, and a shared struct makes the caption cache of the one that
 * is closed a cost the one that is open pays -- which is the whole thing the revision check
 * exists to avoid.
 */
struct NodeInspectorState {
    /// Index into `names`, which is the *listing*, not a slot and not a `NodeId`.
    ///
    /// It is neither of those for the reason `InspectorState::selected` is a slot rather
    /// than an id, taken one step further: a selection is a row a person is pointing at,
    /// and the row is what has to survive. A `NodeId` goes stale the moment the node is
    /// destroyed and leaves the panel empty; a slot survives that but not a reparent, which
    /// reorders the listing without destroying anything. The row index survives both, and
    /// what it selects afterwards is whatever is now written where the user was looking.
    uint32_t selected = 0;

    /// Captions in listing order, rebuilt only when the tree's structure moves.
    std::vector<std::string> names;
    /// The node behind each caption. Held rather than re-derived because the walk that
    /// produced the order is the only thing that knows it.
    std::vector<scene::NodeId> nodes;
    /// Depth behind each caption, so the detail pane can say it without walking up.
    std::vector<uint32_t> depths;
    /// `Scene::structureRevision` the three vectors were built from. `0` is "never", which
    /// no scene reports.
    uint64_t structureRevision = 0;

    /// How far a node may be dragged from where it sits, in metres. The same range-around-
    /// the-current-value trick `InspectorState::reach` uses, and for the same reason: a
    /// scene has no natural extent to slide within.
    float reach = 10.0f;
};

/**
 * @brief One frame of the scene-tree inspector.
 *
 * @return true when the selected node's local transform was edited this frame, so a caller
 *         that has to react can, without diffing it.
 *
 * Takes the scene by mutable reference because editing is the point, and writes every edit
 * through `setLocalPosition` / `setLocalScale` rather than into anything of its own -- so
 * the dirty bit, the sweep and the push all happen exactly as they do for a game's own
 * write. There is no shadow copy of a transform here to go stale.
 */
bool drawNodeInspector(Context& ui, scene::Scene& scene, NodeInspectorState& state, const glm::vec2& pos,
                       const glm::vec2& size);

/**
 * @brief The caption for one node, exposed for the test that pins it.
 *
 * `depth` is leading indent, which is what makes a flat list read as a tree. The letters
 * after the name are the attachment record in one glance -- `M` mesh, `B` body, `C`
 * character, `S` sound, `L` light, `E` emitter -- because "which of these forty nodes is
 * the one with the light on it" is the question an inspector is opened to answer.
 */
[[nodiscard]] std::string nodeCaption(const scene::Scene& scene, scene::NodeId id, uint32_t depth);

} // namespace ui
