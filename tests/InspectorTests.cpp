/**
 * @file InspectorTests.cpp
 * @brief 5.6's inspector, exercised with no device and no window.
 *
 * The whole point of `ui/` being hosted: a panel is a function that reads state and
 * appends vertices, so a test can call it, look at the vertices, and look at what it
 * wrote back. There is no Vulkan anywhere below.
 */
#include "scene/InstanceTable.h"
#include "ui/Inspector.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace scene;

namespace {

InstanceDesc cube(const glm::mat4& m = glm::mat4(1.0f)) {
    InstanceDesc d;
    d.localMin = glm::vec3(-1.0f);
    d.localMax = glm::vec3(1.0f);
    d.transform = m;
    d.firstIndex = 0;
    d.indexCount = 36;
    d.baseVertex = 0;
    d.vertexCount = 8;
    return d;
}

/// A context with a font that measures something, laid out over a screen-sized area.
/// `FontMetrics` default-constructs to a zero-advance font, which lays every glyph on
/// top of the last -- fine for hit testing, useless for anything that asserts on extent.
ui::FontMetrics testFont() {
    ui::FontMetrics f;
    f.lineSpacing = 16.0f;
    f.ascentPx = 12.0f;
    for (auto& g : f.glyphs) {
        g.advance = 8.0f;
        g.x1 = 7.0f;
        g.y1 = 12.0f;
    }
    return f;
}

} // namespace

TEST(Inspector, ACaptionSaysWhatSortOfThingTheSlotHolds) {
    InstanceTable table;
    InstanceDesc still = cube();
    still.material = 3;
    still.primitive = 7;
    table.create(still);

    InstanceDesc moving = cube();
    moving.dynamic = true;
    table.create(moving);

    InstanceDesc deformed = cube();
    deformed.skin = 0;
    table.create(deformed);

    InstanceDesc glass = cube();
    glass.blended = true;
    table.create(glass);

    // The identity a reader is scanning for is on every row, and the kind is one
    // character in front of it -- a column of "slot 0, slot 1, slot 2" says nothing.
    EXPECT_NE(ui::instanceCaption(table, 0).find("prim 7"), std::string::npos);
    EXPECT_NE(ui::instanceCaption(table, 0).find("mat 3"), std::string::npos);

    EXPECT_EQ(ui::instanceCaption(table, 1)[0], '>'); // moves
    EXPECT_EQ(ui::instanceCaption(table, 2)[0], '~'); // vertices rebuilt
    EXPECT_EQ(ui::instanceCaption(table, 3)[0], '='); // forward pass draws it
    EXPECT_EQ(ui::instanceCaption(table, 0)[0], ' '); // plain opaque geometry
}

TEST(Inspector, TheListSkipsHolesAndMapsBackToSlots) {
    InstanceTable table;
    const InstanceId a = table.create(cube());
    const InstanceId b = table.create(cube());
    table.create(cube());
    table.destroy(b);

    ui::Context ui;
    ui::InspectorState state;
    const ui::FontMetrics font = testFont();

    ui.begin({}, 1600.0f, 900.0f, font, 1.0f);
    ui::drawInstanceInspector(ui, table, state, {0.0f, 0.0f}, {400.0f, 800.0f});
    ui.end();

    // Two live of three slots, and the second entry is slot 2 rather than slot 1 --
    // which is the whole reason the state carries a slot array beside the captions.
    ASSERT_EQ(state.names.size(), 2u);
    ASSERT_EQ(state.slots.size(), 2u);
    EXPECT_EQ(state.slots[0], a.index);
    EXPECT_EQ(state.slots[1], 2u);
}

TEST(Inspector, TheCaptionCacheRebuildsOnlyWhenTheTableMoves) {
    InstanceTable table;
    table.create(cube());

    ui::Context ui;
    ui::InspectorState state;
    const ui::FontMetrics font = testFont();

    const auto draw = [&] {
        ui.begin({}, 1600.0f, 900.0f, font, 1.0f);
        ui::drawInstanceInspector(ui, table, state, {0.0f, 0.0f}, {400.0f, 800.0f});
        ui.end();
    };

    draw();
    const uint64_t first = state.namesRevision;
    EXPECT_NE(first, 0u);

    draw();
    EXPECT_EQ(state.namesRevision, first); // a static table costs no strings

    table.create(cube());
    draw();
    EXPECT_GT(state.namesRevision, first);
    EXPECT_EQ(state.names.size(), 2u);
}

TEST(Inspector, ASelectionPastTheEndClampsRatherThanResetting) {
    InstanceTable table;
    const InstanceId a = table.create(cube());
    table.create(cube());
    table.create(cube());

    ui::Context ui;
    ui::InspectorState state;
    const ui::FontMetrics font = testFont();
    state.selected = 2;

    table.destroy(a);
    table.destroy(table.idAt(1));

    ui.begin({}, 1600.0f, 900.0f, font, 1.0f);
    ui::drawInstanceInspector(ui, table, state, {0.0f, 0.0f}, {400.0f, 800.0f});
    ui.end();

    // One live instance left, so the only legal index is 0 -- and it got there by
    // clamping. Resetting to zero happens to give the same answer here; the distinction
    // shows up with more survivors, and clamping is what keeps the cursor near where the
    // user's attention already was.
    ASSERT_EQ(state.names.size(), 1u);
    EXPECT_EQ(state.selected, 0u);
}

TEST(Inspector, AnEmptyTableDrawsAPanelAndSelectsNothing) {
    InstanceTable table;
    ui::Context ui;
    ui::InspectorState state;
    const ui::FontMetrics font = testFont();

    ui.begin({}, 1600.0f, 900.0f, font, 1.0f);
    // The failure this pins is an index into an empty vector, which is what every
    // "clamp the selection" line below the list would do without the early out above it.
    EXPECT_FALSE(ui::drawInstanceInspector(ui, table, state, {0.0f, 0.0f}, {400.0f, 800.0f}));
    ui.end();

    EXPECT_TRUE(state.names.empty());
}

TEST(Inspector, EditingPositionLeavesRotationAndScaleExactlyAlone) {
    InstanceTable table;

    // Deliberately not a plain translation: a rotation and a non-uniform scale are what a
    // decompose-and-recompose round trip would quietly damage.
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 4.0f, 5.0f));
    m = glm::rotate(m, 0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
    m = glm::scale(m, glm::vec3(2.0f, 0.5f, 1.5f));
    const InstanceId id = table.create(cube(m));

    // The write the inspector performs, isolated from the widget that triggers it: the
    // translation column only.
    glm::mat4 next = table.transform(id);
    next[3] = glm::vec4(9.0f, -1.0f, 2.0f, next[3].w);
    table.setTransform(id, next);

    const glm::mat4& after = table.transform(id);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 4; ++row) {
            EXPECT_FLOAT_EQ(after[col][row], m[col][row]) << "column " << col << " row " << row;
        }
    }
    EXPECT_FLOAT_EQ(after[3].x, 9.0f);
    EXPECT_FLOAT_EQ(after[3].y, -1.0f);
    EXPECT_FLOAT_EQ(after[3].z, 2.0f);
    EXPECT_FLOAT_EQ(after[3].w, m[3].w);
}

TEST(Inspector, MovingAnObjectRefreshesTheBoundsTheInspectorReadsBack) {
    InstanceTable table;
    const InstanceId id = table.create(cube());

    glm::mat4 next = table.transform(id);
    next[3] = glm::vec4(10.0f, 0.0f, 0.0f, 1.0f);
    table.setTransform(id, next);

    // Through setTransform rather than by writing the array, which is 4.1b's property
    // (iii) doing the work it was named for: the world bounds the inspector prints -- and
    // the cull dispatch tests -- are refreshed because the mutation went through the
    // call that refreshes them.
    const GpuInstanceBounds& b = table.slotBounds(id.index);
    EXPECT_FLOAT_EQ(b.worldMin.x, 9.0f);
    EXPECT_FLOAT_EQ(b.worldMax.x, 11.0f);
}

// ============================================================ the node inspector
//
// The second panel, and the checks the card it came from never had. `golden-11` proves the
// image did not move and `scaffold` proves a game links; neither can say whether the list
// names the right node, in the right place, with the right things hanging off it.

namespace {

/// Draw the node panel once, the way a frame does.
bool drawNodes(ui::Context& ui, Scene& scene, ui::NodeInspectorState& state, const ui::FontMetrics& font) {
    ui.begin({}, 1600.0f, 900.0f, font, 1.0f);
    const bool edited = ui::drawNodeInspector(ui, scene, state, {0.0f, 0.0f}, {400.0f, 800.0f});
    ui.end();
    return edited;
}

/// Where a node sits in the listing, or -1.
int rowOf(const ui::NodeInspectorState& state, NodeId id) {
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        if (state.nodes[i] == id) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

TEST(NodeInspector, TheListingIsDepthFirstSoAChildSitsUnderItsParent) {
    Scene scene;
    const NodeId level = scene.create("level");
    const NodeId torch = scene.create("torch", level);
    scene.create("flame", torch);
    const NodeId player = scene.create("player");
    scene.create("hand", player);

    ui::Context ui;
    ui::NodeInspectorState state;
    drawNodes(ui, scene, state, testFont());

    ASSERT_EQ(state.names.size(), 5u);
    ASSERT_EQ(state.nodes.size(), 5u);

    // Every node appears once, and each one directly follows its parent or a sibling of
    // its parent -- which is what a person means by a tree listing. `Scene::order()` is
    // breadth-first and would put both roots first, which reads as two flat lists.
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        const NodeId parent = scene.parent(state.nodes[i]);
        if (!parent.valid()) continue;
        const int at = rowOf(state, parent);
        ASSERT_GE(at, 0);
        EXPECT_LT(static_cast<size_t>(at), i) << "a child listed above its parent";
        EXPECT_EQ(state.depths[i], state.depths[static_cast<size_t>(at)] + 1u);
    }

    // The one ordering the walk does promise: a subtree is contiguous. `flame` is directly
    // under `torch`, not after `player`.
    const int torchRow = rowOf(state, torch);
    ASSERT_GE(torchRow, 0);
    EXPECT_EQ(state.depths[static_cast<size_t>(torchRow) + 1], 2u);
    EXPECT_NE(state.names[static_cast<size_t>(torchRow) + 1].find("flame"), std::string::npos);
}

TEST(NodeInspector, ACaptionNamesTheNodeAndWhatHangsOffIt) {
    Scene scene;
    const NodeId lamp = scene.create("lamp");
    scene.attachLight(lamp, 3);
    scene.attachSound(lamp, SoundId{2u, 1u});

    // Every letter, in the order `Attachments` declares them, so the caption and the
    // detail pane below it read the same way round.
    EXPECT_NE(ui::nodeCaption(scene, lamp, 0).find("lamp"), std::string::npos);
    EXPECT_NE(ui::nodeCaption(scene, lamp, 0).find("SL"), std::string::npos);

    const NodeId bare = scene.create("bare");
    EXPECT_EQ(ui::nodeCaption(scene, bare, 0), "bare");

    // Indent is what makes a flat list read as a tree.
    EXPECT_EQ(ui::nodeCaption(scene, bare, 2), "    bare");

    // A node created with no name is a blank row nobody can aim a selection at, so it
    // gets one. `Scene::create` takes whatever it is handed.
    const NodeId unnamed = scene.create("");
    EXPECT_EQ(ui::nodeCaption(scene, unnamed, 0), "(unnamed)");

    // And a stale handle is a dash rather than a read of a slot somebody else now owns.
    scene.destroy(bare);
    EXPECT_EQ(ui::nodeCaption(scene, bare, 0), "-");
}

TEST(NodeInspector, TheListingRebuildsWhenTheTreeMovesAndNotWhenANodeDoes) {
    Scene scene;
    const NodeId a = scene.create("a");

    ui::Context ui;
    ui::NodeInspectorState state;
    const ui::FontMetrics font = testFont();

    drawNodes(ui, scene, state, font);
    const uint64_t first = state.structureRevision;
    EXPECT_NE(first, 0u);

    // A whole animated scene costs no strings, which is the reason the counter is called
    // structure rather than revision.
    scene.setLocalPosition(a, {5.0f, 0.0f, 0.0f});
    scene.update({});
    drawNodes(ui, scene, state, font);
    EXPECT_EQ(state.structureRevision, first);
    EXPECT_EQ(state.names.size(), 1u);

    scene.create("b", a);
    drawNodes(ui, scene, state, font);
    EXPECT_GT(state.structureRevision, first);
    ASSERT_EQ(state.names.size(), 2u);
    EXPECT_EQ(state.depths[1], 1u);
}

TEST(NodeInspector, ASelectionPastTheEndClampsRatherThanResetting) {
    Scene scene;
    scene.create("a");
    const NodeId b = scene.create("b");
    scene.create("c");

    ui::Context ui;
    ui::NodeInspectorState state;
    state.selected = 2;

    scene.destroy(b);
    drawNodes(ui, scene, state, testFont());

    ASSERT_EQ(state.names.size(), 2u);
    EXPECT_EQ(state.selected, 1u);
}

TEST(NodeInspector, AnEmptySceneDrawsAPanelAndSelectsNothing) {
    Scene scene;
    ui::Context ui;
    ui::NodeInspectorState state;

    // The failure this pins is an index into an empty vector, which is what every line
    // below the list would do without the early out above it.
    EXPECT_FALSE(drawNodes(ui, scene, state, testFont()));
    EXPECT_TRUE(state.names.empty());

    // And a scene emptied under an open panel is the same case arriving late.
    const NodeId a = scene.create("a");
    drawNodes(ui, scene, state, testFont());
    ASSERT_EQ(state.names.size(), 1u);
    scene.destroy(a);
    EXPECT_FALSE(drawNodes(ui, scene, state, testFont()));
    EXPECT_TRUE(state.names.empty());
}

TEST(NodeInspector, TheDetailPaneReadsTheSelectedNodeAndNotTheFirstOne) {
    Scene scene;
    const NodeId first = scene.create("first");
    const NodeId second = scene.create("second", first);
    scene.attachInstance(second, InstanceId{4u, 1u}, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f)));
    scene.setLocalPosition(second, {1.0f, 2.0f, 3.0f});
    scene.update({});

    ui::Context ui;
    ui::NodeInspectorState state;
    drawNodes(ui, scene, state, testFont());

    // What the panel resolves the selection to. The whole listing is one indirection --
    // row to `NodeId` -- and getting it wrong draws a plausible panel about the wrong
    // object, which is the failure a byte-identical image could never show.
    const int row = rowOf(state, second);
    ASSERT_GE(row, 0);
    state.selected = static_cast<uint32_t>(row);
    drawNodes(ui, scene, state, testFont());

    const NodeId shown = state.nodes[state.selected];
    EXPECT_TRUE(shown == second);
    EXPECT_EQ(scene.name(shown), "second");
    EXPECT_TRUE(scene.parent(shown) == first);
    EXPECT_TRUE(scene.attachments(shown).hasOffset);
    EXPECT_FLOAT_EQ(scene.localPosition(shown).x, 1.0f);
}

TEST(NodeInspector, EditingAPositionGoesThroughTheSceneAndLeavesRotationAlone) {
    Scene scene;
    const NodeId node = scene.create("node");
    const glm::quat spin = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
    scene.setLocalRotation(node, spin);
    scene.setLocalScale(node, glm::vec3(2.0f, 0.5f, 1.5f));

    // The write the panel performs, isolated from the widget that triggers it. A node
    // stores TRS as TRS, so this is exact -- there is no decompose-and-recompose round
    // trip to damage the other two, which is why all three are writable here and only the
    // translation column is in the instance panel.
    scene.setLocalPosition(node, {9.0f, -1.0f, 2.0f});

    EXPECT_FLOAT_EQ(scene.localPosition(node).x, 9.0f);
    EXPECT_FLOAT_EQ(scene.localPosition(node).y, -1.0f);
    EXPECT_FLOAT_EQ(scene.localPosition(node).z, 2.0f);
    EXPECT_FLOAT_EQ(scene.localRotation(node).x, spin.x);
    EXPECT_FLOAT_EQ(scene.localRotation(node).w, spin.w);
    EXPECT_FLOAT_EQ(scene.localScale(node).y, 0.5f);

    // And it reached the sweep, because it went through the call that sets the dirty bit
    // rather than into a copy of its own.
    scene.update({});
    EXPECT_FLOAT_EQ(scene.worldTransform(node)[3].x, 9.0f);
}

TEST(NodeInspector, ADrivenNodeIsReportedAsDrivenRatherThanQuietlyIgnored) {
    Scene scene;
    const NodeId plain = scene.create("plain");
    const NodeId rigid = scene.create("rigid");
    scene.attachBody(rigid, BodyId{3u, 1u});
    const NodeId walker = scene.create("walker");
    scene.attachCharacter(walker, PhysicsCharacterId{1u, 1u});

    // The panel's `driven` row is this predicate, and it is the honest half of the
    // transform section: `Scene` takes a solver's matrix verbatim and never writes the
    // local TRS back, so for these two the sliders move a number nothing reads.
    const auto driven = [&](NodeId id) {
        const Attachments& a = scene.attachments(id);
        return a.body.valid() || a.character.valid();
    };
    EXPECT_FALSE(driven(plain));
    EXPECT_TRUE(driven(rigid));
    EXPECT_TRUE(driven(walker));

    // Drawn with each selected in turn, because a panel that reads a body index off a node
    // with no body is the way this goes wrong.
    ui::Context ui;
    ui::NodeInspectorState state;
    for (uint32_t row = 0; row < 3; ++row) {
        state.selected = row;
        drawNodes(ui, scene, state, testFont());
    }
    EXPECT_EQ(state.names.size(), 3u);
}
