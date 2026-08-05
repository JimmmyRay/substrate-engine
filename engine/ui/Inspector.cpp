#include "ui/Inspector.h"

#include "core/Format.h"
#include "core/Logger.h"

#include <cstdarg>
#include <utility>

namespace ui {

namespace {

/// Format and hand back a string. Sized by `Logger::vformat`, not by a local buffer -- a fixed
/// 160 bytes truncates silently on a long instance name or a matrix row with its label.
__attribute__((format(SUBSTRATE_PRINTF_FORMAT, 1, 2))) std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string out = core::Logger::vformat(fmt, args);
    va_end(args);
    return out;
}

/// One flag as a `[x] name` row, read-only.
///
/// Four of these seven bits are derived. Make this a checkbox and clearing SKINNED on a skinned
/// mesh does not un-skin it -- it makes the draw fetch bind-pose vertices for a slot the
/// deformation dispatch is still writing.
void flagRow(Context& ui, uint32_t flags, uint32_t bit, const char* name) {
    ui.beginRow(2);
    ui.labelDim(name);
    ui.labelRight((flags & bit) != 0u ? "yes" : "-");
    ui.endRow();
}

} // namespace

std::string instanceCaption(const scene::InstanceTable& instances, uint32_t slot) {
    const scene::GpuInstance& g = instances.slot(slot);
    const uint32_t f = g.meta.z;

    // One character: the column is narrow.
    const char* kind = "  ";
    if ((f & scene::kInstanceDeformed) != 0u) {
        kind = "~ "; // vertices rebuilt every frame
    } else if ((f & scene::kInstanceBlended) != 0u) {
        kind = "= "; // the forward pass draws it
    } else if ((f & scene::kInstanceDynamic) != 0u) {
        kind = "> "; // moves, so 3.4 corrects it
    }

    return format("%s%3u  prim %u  mat %u", kind, slot, g.meta.x, g.meta.y);
}

bool drawInstanceInspector(Context& ui, scene::InstanceTable& instances, InspectorState& state, const glm::vec2& pos,
                           const glm::vec2& size) {
    if (!ui.beginPanel("Inspector", pos, size)) return false;

    // Rebuilt on revision, not per frame: this is a `std::string` per live instance, and the
    // table is static in almost every frame of almost every scene.
    if (state.namesRevision != instances.revision()) {
        state.namesRevision = instances.revision();
        state.names.clear();
        state.slots.clear();
        for (uint32_t s = 0; s < instances.slotCount(); ++s) {
            if ((instances.slot(s).meta.z & scene::kInstanceLive) == 0u) continue;
            state.names.push_back(instanceCaption(instances, s));
            state.slots.push_back(s);
        }
    }

    ui.beginRow(2);
    ui.labelDim(format("%u live", instances.liveCount()));
    ui.labelRight(format("%u dynamic", instances.dynamicCount()));
    ui.endRow();

    if (state.names.empty()) {
        ui.labelDim("Nothing in the table");
        ui.endPanel();
        return false;
    }

    // Clamped, never reset: dropping the selection to zero when the table shrinks scrolls the
    // user back to the top for a reason they cannot see.
    if (state.selected >= state.names.size()) state.selected = static_cast<uint32_t>(state.names.size() - 1);

    ui.list("Instances", state.names, state.selected, ui.scaled().rowHeight * 8.0f);

    const uint32_t slot = state.slots[state.selected];
    const scene::GpuInstance& g = instances.slot(slot);
    const uint32_t flags = g.meta.z;

    ui.separator();
    ui.labelDim("Identity");
    ui.beginRow(2);
    ui.labelDim("slot");
    ui.labelRight(format("%u", slot));
    ui.endRow();
    ui.beginRow(2);
    ui.labelDim("primitive");
    ui.labelRight(format("%u", g.meta.x));
    ui.endRow();
    ui.beginRow(2);
    ui.labelDim("material");
    ui.labelRight(format("%u", g.meta.y));
    ui.endRow();
    ui.beginRow(2);
    ui.labelDim("character");
    // UINT32_MAX is "none". Printed raw it is a sentinel escaping into the interface.
    ui.labelRight(g.meta.w == 0xFFFFFFFFu ? "-" : format("%u", g.meta.w));
    ui.endRow();

    ui.separator();
    ui.labelDim("Flags");
    flagRow(ui, flags, scene::kInstanceLive, "live");
    flagRow(ui, flags, scene::kInstanceBlended, "blended");
    flagRow(ui, flags, scene::kInstanceDynamic, "dynamic");
    // Not a `flagRow`: `kInstanceVisible` is written by the cull dispatch into the GPU's copy
    // and never read back, so the CPU-side bit is clear for everything, always. Shown as a flag
    // it reads as "nothing is on screen".
    ui.beginRow(2);
    ui.labelDim("visible");
    ui.labelRight("gpu-side");
    ui.endRow();
    flagRow(ui, flags, scene::kInstanceSkinned, "skinned");
    flagRow(ui, flags, scene::kInstanceMorphed, "morphed");
    flagRow(ui, flags, scene::kInstanceMasked, "masked");

    // Read out of the matrix and written straight back into it. Cache the value between the
    // two and it goes stale against anything else that writes the transform.
    ui.separator();
    ui.labelDim("Position");

    const glm::mat4 m = instances.transform(slot);
    glm::vec3 p(m[3]);
    const glm::vec3 origin = p;

    bool moved = false;
    moved |= ui.slider("X", p.x, origin.x - state.reach, origin.x + state.reach);
    moved |= ui.slider("Y", p.y, origin.y - state.reach, origin.y + state.reach);
    moved |= ui.slider("Z", p.z, origin.z - state.reach, origin.z + state.reach);
    ui.slider("Reach", state.reach, 0.1f, 100.0f);

    if (moved) {
        // The translation column only; touching the other three turns this into the lossy
        // decompose-and-recompose round trip the header rules out.
        glm::mat4 next = m;
        next[3] = glm::vec4(p, m[3].w);
        instances.setTransform(instances.idAt(slot), next);
    }

    ui.beginRow(2);
    ui.labelDim("rotation");
    ui.labelRight("read-only");
    ui.endRow();

    ui.separator();
    ui.labelDim("World bounds");
    const scene::GpuInstanceBounds& b = instances.slotBounds(slot);
    ui.labelDim(format("min %.2f %.2f %.2f", static_cast<double>(b.worldMin.x), static_cast<double>(b.worldMin.y),
                       static_cast<double>(b.worldMin.z)));
    ui.labelDim(format("max %.2f %.2f %.2f", static_cast<double>(b.worldMax.x), static_cast<double>(b.worldMax.y),
                       static_cast<double>(b.worldMax.z)));

    const auto& r = instances.drawRanges()[slot];
    ui.beginRow(2);
    ui.labelDim("triangles");
    ui.labelRight(format("%u", r.indexCount / 3));
    ui.endRow();

    ui.endPanel();
    return moved;
}

namespace {

/// One handle as a `name  index` row, or a dash. Printed raw, the sentinel escapes into the
/// interface as `4294967295`.
void handleRow(Context& ui, const char* name, bool present, uint32_t index) {
    ui.beginRow(2);
    ui.labelDim(name);
    ui.labelRight(present ? format("%u", index) : "-");
    ui.endRow();
}

} // namespace

std::string nodeCaption(const scene::Scene& scene, scene::NodeId id, uint32_t depth) {
    if (!scene.valid(id)) return "-";

    // In the order `Attachments` declares them, so this row and the detail pane agree.
    const scene::Attachments& a = scene.attachments(id);
    std::string marks;
    if (a.instance.valid()) marks += 'M';
    if (a.body.valid()) marks += 'B';
    if (a.character.valid()) marks += 'C';
    if (a.sound.valid()) marks += 'S';
    if (a.light != scene::kNoAttachment) marks += 'L';
    if (a.emitter != scene::kNoAttachment) marks += 'E';

    // Capped: uncapped, a chain twenty deep pushes every name off the right-hand edge.
    const std::string indent(2u * (depth < 8u ? depth : 8u), ' ');

    // A node may have no name -- `Scene::create` takes whatever it is given -- and an empty
    // caption is a blank row a selection cannot be aimed at.
    const std::string& name = scene.name(id);
    return format("%s%s%s%s", indent.c_str(), name.empty() ? "(unnamed)" : name.c_str(), marks.empty() ? "" : "  ",
                  marks.c_str());
}

bool drawNodeInspector(Context& ui, scene::Scene& scene, NodeInspectorState& state, const glm::vec2& pos,
                       const glm::vec2& size) {
    if (!ui.beginPanel("Scene", pos, size)) return false;

    // Keyed on the *structure* revision, not on any transform counter: a caption is a name and
    // an attachment record, so a scene animating every node rebuilds nothing.
    if (state.structureRevision != scene.structureRevision()) {
        state.structureRevision = scene.structureRevision();
        state.names.clear();
        state.nodes.clear();
        state.depths.clear();

        // Depth-first pre-order, so a child is written directly under its parent.
        // `Scene::order()` is breadth-first and correct for the sweep it exists for; used here
        // it puts every root at the top and every leaf at the bottom, beside nothing.
        //
        // The push-then-reverse lands siblings in creation order because `Scene` links at the
        // head. Nothing promises that and nothing here depends on it.
        std::vector<std::pair<scene::NodeId, uint32_t>> stack;
        for (scene::NodeId r = scene.firstRoot(); r.valid(); r = scene.nextSibling(r)) stack.emplace_back(r, 0u);
        while (!stack.empty()) {
            const scene::NodeId id = stack.back().first;
            const uint32_t depth = stack.back().second;
            stack.pop_back();

            state.names.push_back(nodeCaption(scene, id, depth));
            state.nodes.push_back(id);
            state.depths.push_back(depth);

            for (scene::NodeId c = scene.firstChild(id); c.valid(); c = scene.nextSibling(c)) {
                stack.emplace_back(c, depth + 1u);
            }
        }
    }

    ui.beginRow(2);
    ui.labelDim(format("%u live", scene.liveCount()));
    // Slots minus live is the free list. A scene leaking nodes grows this without the live
    // count moving, which is what the leak looks like from here.
    ui.labelRight(format("%u slots", scene.slotCount()));
    ui.endRow();

    if (state.names.empty()) {
        ui.labelDim("No nodes in the scene");
        ui.endPanel();
        return false;
    }

    // Clamped, never reset, for the reason the instance list gives.
    if (state.selected >= state.names.size()) state.selected = static_cast<uint32_t>(state.names.size() - 1);

    ui.list("Nodes", state.names, state.selected, ui.scaled().rowHeight * 8.0f);

    const scene::NodeId id = state.nodes[state.selected];
    // The listing is a frame old whenever a node was destroyed without the structure counter
    // moving, so it can name a handle that no longer resolves.
    if (!scene.valid(id)) {
        ui.separator();
        ui.labelDim("That node is gone");
        ui.endPanel();
        return false;
    }

    const scene::Attachments& a = scene.attachments(id);

    ui.separator();
    ui.labelDim("Identity");
    ui.beginRow(2);
    ui.labelDim("name");
    ui.labelRight(scene.name(id).empty() ? "-" : scene.name(id));
    ui.endRow();
    ui.beginRow(2);
    ui.labelDim("slot");
    ui.labelRight(format("%u", id.index));
    ui.endRow();
    ui.beginRow(2);
    // The generation is what says a handle went stale.
    ui.labelDim("generation");
    ui.labelRight(format("%u", id.generation));
    ui.endRow();

    ui.separator();
    ui.labelDim("Hierarchy");
    const scene::NodeId up = scene.parent(id);
    ui.beginRow(2);
    ui.labelDim("parent");
    ui.labelRight(up.valid() ? (scene.name(up).empty() ? format("slot %u", up.index) : scene.name(up)) : "root");
    ui.endRow();
    uint32_t children = 0;
    for (scene::NodeId c = scene.firstChild(id); c.valid(); c = scene.nextSibling(c)) ++children;
    ui.beginRow(2);
    ui.labelDim("children");
    ui.labelRight(format("%u", children));
    ui.endRow();
    ui.beginRow(2);
    ui.labelDim("depth");
    ui.labelRight(format("%u", state.depths[state.selected]));
    ui.endRow();

    ui.separator();
    ui.labelDim("Local transform");

    // A node the solver drives takes its world transform verbatim and never writes its local
    // TRS back, so the sliders below move a value nothing reads. Drop this row and a user drags
    // a number with no effect and no explanation.
    const bool driven = a.body.valid() || a.character.valid();
    ui.beginRow(2);
    ui.labelDim("driven");
    ui.labelRight(driven ? "solver" : "-");
    ui.endRow();

    glm::vec3 p = scene.localPosition(id);
    const glm::vec3 origin = p;
    bool edited = false;
    edited |= ui.slider("X", p.x, origin.x - state.reach, origin.x + state.reach);
    edited |= ui.slider("Y", p.y, origin.y - state.reach, origin.y + state.reach);
    edited |= ui.slider("Z", p.z, origin.z - state.reach, origin.z + state.reach);
    if (edited) scene.setLocalPosition(id, p);
    ui.slider("Reach", state.reach, 0.1f, 100.0f);

    glm::vec3 s = scene.localScale(id);
    bool scaled = false;
    // Floored above zero, not at it: a slider that reaches zero makes the singular matrix
    // `Scene`'s decompose has no answer for.
    scaled |= ui.slider("Scale X", s.x, 0.01f, 10.0f);
    scaled |= ui.slider("Scale Y", s.y, 0.01f, 10.0f);
    scaled |= ui.slider("Scale Z", s.z, 0.01f, 10.0f);
    if (scaled) scene.setLocalScale(id, s);

    // Printed as the four stored components, never as angles: a derived readout is a second
    // representation to keep in step with the first.
    const glm::quat q = scene.localRotation(id);
    ui.labelDim(format("rot %.3f %.3f %.3f %.3f", static_cast<double>(q.x), static_cast<double>(q.y),
                       static_cast<double>(q.z), static_cast<double>(q.w)));

    ui.separator();
    ui.labelDim("World");
    // As of the last `update()`: a read in the same frame as a write sees the old value, so
    // this is one frame behind an edit made with the slider above it.
    const glm::vec3 w(scene.worldTransform(id)[3]);
    ui.labelDim(format("pos %.2f %.2f %.2f", static_cast<double>(w.x), static_cast<double>(w.y),
                       static_cast<double>(w.z)));

    ui.separator();
    ui.labelDim("Attachments");
    handleRow(ui, "instance", a.instance.valid(), a.instance.index);
    handleRow(ui, "body", a.body.valid(), a.body.index);
    handleRow(ui, "character", a.character.valid(), a.character.index);
    handleRow(ui, "sound", a.sound.valid(), a.sound.index);
    handleRow(ui, "light", a.light != scene::kNoAttachment, a.light);
    handleRow(ui, "emitter", a.emitter != scene::kNoAttachment, a.emitter);
    ui.beginRow(2);
    // Not an index, and the usual answer when a mesh draws somewhere its node is not.
    ui.labelDim("offset");
    ui.labelRight(a.hasOffset ? "yes" : "-");
    ui.endRow();

    ui.endPanel();
    return edited || scaled;
}

} // namespace ui
