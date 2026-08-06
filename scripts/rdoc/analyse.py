#!/usr/bin/env python3
"""Read a RenderDoc capture that has been converted to XML.

    scripts/rdoc/analyse.py passes    <frame.xml>
    scripts/rdoc/analyse.py state     <frame.xml> <chunkIndex>
    scripts/rdoc/analyse.py barriers  <frame.xml>
    scripts/rdoc/analyse.py resources <frame.xml>

Driven by `substrate rdoc`, which does the conversion first. Run it directly only
when you already have the XML.

## Why XML and not RenderDoc's replay API

The replay API is the better tool and it is not available here. RenderDoc's official
Linux build ships an embedded Python 3.6 with the standard library but *not* the
`renderdoc` module that drives a replay, and `qrenderdoc --python` is a silent no-op --
a missing script, a syntactically invalid script and a working one all behave
identically, which is to say the UI opens and nothing runs. Getting the module means
building RenderDoc from source.

So this reads the *record* side rather than the *replay* side. That is a real
limitation and worth stating plainly: it can show every command the engine submitted
and every piece of state bound when it did, but it cannot show what any of it
*produced*. For pixels, use `--capture-target` (any named render target, read straight
out of the engine) and `--debug-view`. For per-pass GPU cost, use `substrate bench`.

The `duration` attribute on each chunk is the CPU time RenderDoc spent recording that
call. It is reported where it is useful for spotting an outlier, and it is never GPU
time -- reading it as GPU cost is the mistake this docstring exists to prevent.
"""

import sys
import xml.etree.ElementTree as ET

# Chunks that end a command buffer's meaningful content. Everything after the frame's
# present is setup for the next one and belongs to no pass.
DRAW_CHUNKS = {"vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect"}
DISPATCH_CHUNKS = {"vkCmdDispatch", "vkCmdDispatchIndirect"}


def load(path):
    """Parse the XML and return the list of chunk elements in record order."""
    try:
        tree = ET.parse(path)
    except (ET.ParseError, OSError) as exc:
        sys.exit("analyse: cannot read %s: %s" % (path, exc))
    return tree.getroot().iter("chunk")


def field(chunk, name):
    """The first descendant with name="<name>", at any depth. Chunks are shallow and
    field names are unique within one, so depth-first-first-match is unambiguous."""
    for el in chunk.iter():
        if el.get("name") == name:
            return el
    return None


def text(chunk, name, default=""):
    el = field(chunk, name)
    if el is None or el.text is None:
        return default
    return el.text.strip()


def enum(chunk, name, default=""):
    """An enum's readable spelling rather than its integer."""
    el = field(chunk, name)
    if el is None:
        return default
    return el.get("string", default)


def resource_names(chunks):
    """ResourceId -> the name the engine gave it.

    Built from our own vkSetDebugUtilsObjectNameEXT calls, which is why
    GpuScope and setObjectName exist: without them every id here is a bare number and
    the rest of this script has nothing to print but integers.
    """
    names = {}
    for c in chunks:
        if c.get("name") == "vkSetDebugUtilsObjectNameEXT":
            obj = text(c, "Object")
            nm = text(c, "ObjectName")
            if obj and nm:
                names[obj] = nm
    return names


def named(names, rid):
    if not rid:
        return "-"
    return "%s(%s)" % (names.get(rid, "?"), rid)


def cmd_passes(chunks, names):
    """The debug-label tree, with the draws and dispatches under each label."""
    depth = 0
    counts = []  # per open label: [draws, dispatches, indices, name]
    printed_any = False
    last_pipeline = None
    first_draw = None

    def flush_draws():
        """Print the accumulated draw run. Individual draws are not listed: a shadow
        cascade is 103 of them against one pipeline, and a screenful of identical lines
        hides the two passes where the count is the interesting part."""
        nonlocal first_draw
        first_draw = None

    for c in chunks:
        kind = c.get("name")
        idx = c.get("chunkIndex")

        if kind == "vkCmdBeginDebugUtilsLabelEXT":
            label = text(c, "pLabelName", "?")
            print("%s%s  [chunk %s]" % ("  " * depth, label, idx))
            counts.append([0, 0, 0, label])
            depth += 1
            last_pipeline = None
            printed_any = True
        elif kind == "vkCmdEndDebugUtilsLabelEXT":
            if not counts:
                continue
            flush_draws()
            depth -= 1
            draws, dispatches, indices, label = counts.pop()
            parts = []
            if draws:
                parts.append("%d draws, %d indices" % (draws, indices))
            if dispatches:
                parts.append("%d dispatches" % dispatches)
            if parts:
                print("%s  -> %s: %s" % ("  " * depth, label, "; ".join(parts)))
            # Roll the totals into the enclosing label, so "Frame" reports the whole
            # command buffer rather than only the calls not inside a nested pass.
            if counts:
                counts[-1][0] += draws
                counts[-1][1] += dispatches
                counts[-1][2] += indices
            last_pipeline = None
        elif kind == "vkCmdBindPipeline":
            pipe = named(names, text(c, "pipeline"))
            # Rebinding the same pipeline every draw is normal and says nothing; only a
            # *change* is worth a line.
            if pipe != last_pipeline:
                print("%sbind %s" % ("  " * depth, pipe))
                last_pipeline = pipe
        elif kind in DRAW_CHUNKS:
            if counts:
                counts[-1][0] += 1
                counts[-1][2] += int(text(c, "indexCount", "0") or 0)
            if first_draw is None:
                first_draw = idx
                # One handle per run, so `state <chunkIndex>` has something to take.
                print("%sdraws from chunk %s" % ("  " * depth, idx))
        elif kind in DISPATCH_CHUNKS:
            if counts:
                counts[-1][1] += 1
            print("%sdispatch %sx%sx%s  [chunk %s]"
                  % ("  " * depth, text(c, "x"), text(c, "y"), text(c, "z"), idx))

    if not printed_any:
        print("no debug labels in this capture -- was it taken with a build that has "
              "the GpuScope labels? (they need VK_EXT_debug_utils, which the capture "
              "layer always provides)")


def cmd_state(chunks, names, target):
    """Everything still bound at chunk `target`.

    A linear walk accumulating the last value of each binding, which is what the
    hardware sees: Vulkan state is a running assignment, so the state at a draw is
    every preceding bind that has not been overwritten.
    """
    state = {}
    label_stack = []
    found = None

    for c in chunks:
        kind = c.get("name")
        idx = c.get("chunkIndex")

        if kind == "vkCmdBeginDebugUtilsLabelEXT":
            label_stack.append(text(c, "pLabelName", "?"))
        elif kind == "vkCmdEndDebugUtilsLabelEXT":
            if label_stack:
                label_stack.pop()
        elif kind == "vkCmdBindPipeline":
            state["pipeline"] = named(names, text(c, "pipeline"))
            state["bindPoint"] = enum(c, "pipelineBindPoint")
        elif kind == "vkCmdBindDescriptorSets":
            sets = field(c, "pDescriptorSets")
            ids = [e.text.strip() for e in sets] if sets is not None else []
            state["descriptorSets"] = "first=%s  [%s]" % (
                text(c, "firstSet"), ", ".join(named(names, i) for i in ids))
            state["pipelineLayout"] = named(names, text(c, "layout"))
        elif kind == "vkCmdBindIndexBuffer":
            state["indexBuffer"] = "%s type=%s" % (named(names, text(c, "buffer")),
                                                  enum(c, "indexType"))
        elif kind == "vkCmdBindVertexBuffers":
            bufs = field(c, "pBuffers")
            ids = [e.text.strip() for e in bufs] if bufs is not None else []
            state["vertexBuffers"] = ", ".join(named(names, i) for i in ids)
        elif kind == "vkCmdSetViewport":
            vp = field(c, "pViewports")
            if vp is not None:
                state["viewport"] = " ".join(
                    "%s=%s" % (e.get("name"), (e.text or "").strip())
                    for e in vp.iter() if e.get("name") in
                    ("x", "y", "width", "height", "minDepth", "maxDepth"))
        elif kind == "vkCmdSetScissor":
            sc = field(c, "pScissors")
            if sc is not None:
                state["scissor"] = " ".join(
                    "%s=%s" % (e.get("name"), (e.text or "").strip())
                    for e in sc.iter() if e.get("name") in ("x", "y", "width", "height"))
        elif kind == "vkCmdBeginRendering":
            state["rendering"] = "begun at chunk %s" % idx

        if idx == target:
            found = (kind, list(label_stack))
            break

    if found is None:
        sys.exit("analyse: no chunk with chunkIndex %s" % target)

    kind, labels = found
    print("chunk %s: %s" % (target, kind))
    print("pass:  %s" % (" / ".join(labels) if labels else "(no label)"))
    for key in ("bindPoint", "pipeline", "pipelineLayout", "descriptorSets",
                "vertexBuffers", "indexBuffer", "viewport", "scissor", "rendering"):
        if key in state:
            print("%-15s %s" % (key + ":", state[key]))


def cmd_barriers(chunks, names):
    """Every image barrier, attributed to the pass that recorded it."""
    label_stack = []
    total = 0

    for c in chunks:
        kind = c.get("name")
        if kind == "vkCmdBeginDebugUtilsLabelEXT":
            label_stack.append(text(c, "pLabelName", "?"))
            continue
        if kind == "vkCmdEndDebugUtilsLabelEXT":
            if label_stack:
                label_stack.pop()
            continue
        if kind != "vkCmdPipelineBarrier2":
            continue

        arr = field(c, "pImageMemoryBarriers")
        if arr is None:
            continue
        where = " / ".join(label_stack) if label_stack else "(no label)"
        for b in arr.findall("struct"):
            total += 1
            img = None
            old = new = src_stage = dst_stage = src_acc = dst_acc = ""
            for el in b:
                nm = el.get("name")
                if nm == "image":
                    img = (el.text or "").strip()
                elif nm == "oldLayout":
                    old = el.get("string", "")
                elif nm == "newLayout":
                    new = el.get("string", "")
                elif nm == "srcStageMask":
                    src_stage = el.get("string", "")
                elif nm == "dstStageMask":
                    dst_stage = el.get("string", "")
                elif nm == "srcAccessMask":
                    src_acc = el.get("string", "")
                elif nm == "dstAccessMask":
                    dst_acc = el.get("string", "")
            print("[chunk %s] %s" % (c.get("chunkIndex"), where))
            print("    %s" % named(names, img))
            print("    layout %s -> %s" % (short(old), short(new)))
            print("    stage  %s -> %s" % (short(src_stage), short(dst_stage)))
            print("    access %s -> %s" % (short(src_acc), short(dst_acc)))

    print("\n%d image barriers" % total)


def short(vk_enum):
    """Strip the shared prefix off a Vulkan enum so a barrier fits on one line."""
    for prefix in ("VK_IMAGE_LAYOUT_", "VK_PIPELINE_STAGE_2_", "VK_ACCESS_2_"):
        vk_enum = vk_enum.replace(prefix, "")
    return vk_enum or "0"


def cmd_resources(chunks, names):
    for rid, name in sorted(names.items(), key=lambda kv: kv[1]):
        print("%-8s %s" % (rid, name))
    print("\n%d named objects" % len(names))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[0] + "\n\n" +
                 "\n".join(l for l in __doc__.splitlines() if l.startswith("    scripts")))

    command, path = sys.argv[1], sys.argv[2]

    # Materialised rather than streamed: the whole point is repeated lookups over the
    # same chunks, and a converted Substrate frame is single-digit megabytes.
    chunks = list(load(path))
    names = resource_names(chunks)

    if command == "passes":
        cmd_passes(chunks, names)
    elif command == "state":
        if len(sys.argv) < 4:
            sys.exit("analyse: state needs a chunkIndex (find one with `passes`)")
        cmd_state(chunks, names, sys.argv[3])
    elif command == "barriers":
        cmd_barriers(chunks, names)
    elif command == "resources":
        cmd_resources(chunks, names)
    else:
        sys.exit("analyse: unknown command '%s' (passes|state|barriers|resources)" % command)


if __name__ == "__main__":
    main()
