#include "ui/Ui.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

/// Everything after `##` identifies the widget and is not drawn. Without it two rows that both
/// say "Reset" are one widget, because the caption *is* the identity -- see `Context::makeId`.
std::string displayText(const std::string& label) {
    const size_t cut = label.find("##");
    return cut == std::string::npos ? label : label.substr(0, cut);
}

bool overlaps(const glm::vec4& a, const glm::vec4& b) {
    return a.x < b.z && a.z > b.x && a.y < b.w && a.w > b.y;
}

bool contains(const glm::vec4& r, const glm::vec2& p) {
    return p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

uint32_t mixColor(uint32_t a, uint32_t b, float t) {
    const float k = clampf(t, 0.0f, 1.0f);
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        const auto ca = static_cast<float>((a >> shift) & 0xFFu);
        const auto cb = static_cast<float>((b >> shift) & 0xFFu);
        const auto mixed = static_cast<uint32_t>(ca + (cb - ca) * k + 0.5f);
        out |= (mixed & 0xFFu) << shift;
    }
    return out;
}

void appendText(std::vector<DrawVertex>& out, const FontMetrics& font, float x, float baselineY,
                const std::string& value, uint32_t rgba) {
    for (char c : value) {
        const Glyph* g = font.glyph(c);
        if (g == nullptr) continue;
        // A zero-area glyph -- the space, in every font -- emits six degenerate vertices the
        // rasteriser then discards. The advance still applies.
        if (g->x1 > g->x0 && g->y1 > g->y0) {
            const float x0 = x + g->x0;
            const float y0 = baselineY + g->y0;
            const float x1 = x + g->x1;
            const float y1 = baselineY + g->y1;
            out.push_back({x0, y0, g->s0, g->t0, rgba});
            out.push_back({x1, y0, g->s1, g->t0, rgba});
            out.push_back({x1, y1, g->s1, g->t1, rgba});
            out.push_back({x0, y0, g->s0, g->t0, rgba});
            out.push_back({x1, y1, g->s1, g->t1, rgba});
            out.push_back({x0, y1, g->s0, g->t1, rgba});
        }
        x += g->advance;
    }
}

void DrawList::reset(const glm::vec4& fullClip) {
    verts.clear();
    cmds.clear();
    clipStack.assign(1, fullClip);
    cmds.push_back({0, 0, fullClip});
}

void DrawList::breakCommand() {
    if (!cmds.empty() && cmds.back().vertexCount == 0) {
        // Retargeted rather than appended, or two clips pushed in a row -- a panel opening a
        // list before drawing anything itself -- leave a zero-vertex draw behind.
        cmds.back().firstVertex = static_cast<uint32_t>(verts.size());
        cmds.back().clip = clipStack.back();
        return;
    }
    cmds.push_back({static_cast<uint32_t>(verts.size()), 0, clipStack.back()});
}

void DrawList::finish() {
    if (!cmds.empty() && cmds.back().vertexCount == 0) cmds.pop_back();
}

void DrawList::pushClip(const glm::vec4& rect) {
    const glm::vec4& top = clipStack.back();
    clipStack.push_back({std::max(rect.x, top.x), std::max(rect.y, top.y), std::min(rect.z, top.z),
                         std::min(rect.w, top.w)});
    breakCommand();
}

void DrawList::popClip() {
    if (clipStack.size() > 1) clipStack.pop_back();
    breakCommand();
}

void DrawList::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba,
                    uint32_t texture) {
    verts.push_back({x0, y0, u0, v0, rgba, texture});
    verts.push_back({x1, y0, u1, v0, rgba, texture});
    verts.push_back({x1, y1, u1, v1, rgba, texture});
    verts.push_back({x0, y0, u0, v0, rgba, texture});
    verts.push_back({x1, y1, u1, v1, rgba, texture});
    verts.push_back({x0, y1, u0, v1, rgba, texture});
    cmds.back().vertexCount += 6;
}

void DrawList::rect(const glm::vec4& r, uint32_t rgba) {
    if (r.z <= r.x || r.w <= r.y) return;
    // Both texcoords are the solid block's centre, so the quad samples coverage 1 throughout
    // and comes out as the vertex colour.
    quad(r.x, r.y, r.z, r.w, whiteU, whiteV, whiteU, whiteV, rgba);
}

void DrawList::image(const glm::vec4& r, uint32_t texture, uint32_t rgba) {
    if (r.z <= r.x || r.w <= r.y) return;
    // No command break: the texture index rides on the vertex, so a panel mixing text and
    // images is still one draw per clip rectangle rather than one per switch.
    quad(r.x, r.y, r.z, r.w, 0.0f, 0.0f, 1.0f, 1.0f, rgba, texture);
}

void DrawList::rectOutline(const glm::vec4& r, uint32_t rgba, float thickness) {
    if (r.z <= r.x || r.w <= r.y || thickness <= 0.0f) return;
    const float t = std::min(thickness, std::min(r.z - r.x, r.w - r.y) * 0.5f);
    rect({r.x, r.y, r.z, r.y + t}, rgba);
    rect({r.x, r.w - t, r.z, r.w}, rgba);
    rect({r.x, r.y + t, r.x + t, r.w - t}, rgba);
    rect({r.z - t, r.y + t, r.z, r.w - t}, rgba);
}

void DrawList::text(const FontMetrics& font, float x, float baselineY, const std::string& value, uint32_t rgba) {
    const size_t before = verts.size();
    appendText(verts, font, x, baselineY, value, rgba);
    cmds.back().vertexCount += static_cast<uint32_t>(verts.size() - before);
}

void Context::begin(const InputState& in, float width, float height, const FontMetrics& fontMetrics, float scale) {
    input = in;
    font = &fontMetrics;
    screen = {width, height};
    dpiScale = scale > 0.0f ? scale : 1.0f;

    // Every widget below must read `metrics`, never `theme`: a distance taken from `theme` is
    // one that never got the DPI scale, and it looks right at 100%.
    metrics = theme;
    metrics.padding *= dpiScale;
    metrics.spacing *= dpiScale;
    metrics.rowHeight = std::max(theme.rowHeight * dpiScale, fontMetrics.lineHeight() + 4.0f * dpiScale);
    metrics.border = std::max(1.0f, theme.border * dpiScale);
    metrics.scrollWidth *= dpiScale;
    metrics.titleHeight = std::max(theme.titleHeight * dpiScale, fontMetrics.lineHeight() + 4.0f * dpiScale);

    drawList.reset({0.0f, 0.0f, width, height});
    drawList.whiteU = fontMetrics.whiteU;
    drawList.whiteV = fontMetrics.whiteV;

    frames.clear();
    Frame root;
    root.bounds = {0.0f, 0.0f, width, height};
    root.cursor = {0.0f, 0.0f};
    root.id = kNoId;
    frames.push_back(root);

    hot = kNoId;
    pointerCaptured = false;
    focusOrder.clear();
    focusStep = in.tab ? (in.shift ? -1 : 1) : 0;
    caretBlink += in.dt;
}

void Context::end() {
    applyFocusChange();

    // After every widget has observed the release. Clear it in `begin` instead and a button
    // released between two frames never sees the edge that fires it.
    if (!input.mouseDown) active = kNoId;

    // A click on no widget drops focus, which is what makes clicking the background -- or the
    // gap between two rows -- commit and dismiss a text field.
    if (input.mousePressed && hot == kNoId) {
        focus = kNoId;
        focusIsText = false;
    }

    if (!wantsKeyboard() && textEditing != kNoId && text != nullptr) {
        text->setActive(false);
        textEditing = kNoId;
    }

    while (frames.size() > 1) frames.pop_back();
    drawList.finish();
}

Id Context::makeId(const std::string& label) const {
    // FNV-1a, seeded with the enclosing frame's id.
    uint32_t h = 2166136261u ^ frames.back().id;
    for (char c : label) {
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h == kNoId ? 1u : h;
}

bool Context::hitTest(const glm::vec4& r) const { return contains(r, input.mouse); }

Context::PanelState& Context::panelState(Id id) {
    for (PanelState& s : panels) {
        if (s.id == id) return s;
    }
    // Linear over one entry per panel and per scrolling list ever opened -- single digits in
    // every caller so far, and the scan it looks like for one that opens hundreds.
    panels.push_back({id, 0.0f, 0.0f});
    return panels.back();
}

bool Context::behave(Id id, const glm::vec4& r, bool& pressed, bool& released) {
    pressed = false;
    released = false;

    focusOrder.push_back({id, false});

    // Clipped out entirely -- scrolled past the end of a list -- means not interactive. Test
    // the rectangle alone and an off-screen row answers a click that landed on what covers it.
    const glm::vec4& clip = drawList.clip();
    const bool visible = overlaps(r, clip);
    const bool over = visible && hitTest(r) && contains(clip, input.mouse);

    if (over) hot = id;

    if (over && input.mousePressed && active == kNoId) {
        active = id;
        pressed = true;
        // A click anywhere takes the keyboard away from wherever it was, which is what
        // makes clicking outside a text field commit it.
        focus = id;
        focusIsText = false;
    }
    if (active == id && input.mouseReleased) {
        released = over;
        active = kNoId;
    }
    return over;
}

uint32_t Context::stateColor(Id id, bool hovered) const {
    if (active == id) return metrics.widgetActive;
    if (hovered) return metrics.widgetHover;
    return metrics.widgetBackground;
}

void Context::drawCaption(const glm::vec4& r, const std::string& value, uint32_t rgba) {
    if (font == nullptr) return;
    // Vertically centred on the row: a caption and a checkbox in one row have different heights
    // and both must look placed.
    const float baseline = r.y + (r.w - r.y - font->lineHeight()) * 0.5f + font->ascent();
    drawList.text(*font, r.x + metrics.padding, baseline, value, rgba);
}

void Context::applyFocusChange() {
    if (focusStep == 0 || focusOrder.empty()) return;

    size_t index = 0;
    bool found = false;
    for (size_t i = 0; i < focusOrder.size(); ++i) {
        if (focusOrder[i].id == focus) {
            index = i;
            found = true;
            break;
        }
    }

    const size_t count = focusOrder.size();
    // Tab from nothing focused lands on the first widget, and Shift-Tab on the last,
    // which is what makes the keyboard reach a panel that was never clicked.
    size_t next = 0;
    if (!found) {
        next = focusStep > 0 ? 0 : count - 1;
    } else {
        next = focusStep > 0 ? (index + 1) % count : (index + count - 1) % count;
    }

    focus = focusOrder[next].id;
    focusIsText = focusOrder[next].isText;
    focusStep = 0;
}


bool Context::beginPanel(const std::string& title, const glm::vec2& pos, const glm::vec2& size) {
    const Id id = makeId(title);
    PanelState& state = panelState(id);

    const glm::vec4 outer{pos.x, pos.y, pos.x + size.x, pos.y + size.y};
    const bool hovered = hitTest(outer);
    // Decided from the whole panel rather than per widget: the gaps between widgets are still
    // the panel, and a drag crossing one must not reach the camera.
    if (hovered) pointerCaptured = true;

    drawList.rect(outer, metrics.panelBackground);
    drawList.rectOutline(outer, metrics.panelBorder, metrics.border);

    float contentTop = outer.y + metrics.border;
    const std::string shown = displayText(title);
    if (!shown.empty()) {
        const glm::vec4 bar{outer.x + metrics.border, contentTop, outer.z - metrics.border,
                            contentTop + metrics.titleHeight};
        drawList.rect(bar, metrics.titleBackground);
        drawCaption(bar, shown, metrics.text);
        contentTop = bar.w;
    }

    const float viewTop = contentTop + metrics.padding;
    const float viewBottom = outer.w - metrics.padding;
    const float viewHeight = std::max(viewBottom - viewTop, 0.0f);
    float contentRight = outer.z - metrics.padding;

    // Sized from what *last* frame measured; the content is laid out after the bar is drawn.
    // Removing the lag means laying the panel out twice.
    const bool scrolls = state.contentHeight > viewHeight + 0.5f;
    if (scrolls) {
        contentRight -= metrics.scrollWidth + metrics.padding;
        const float maxScroll = state.contentHeight - viewHeight;
        if (hovered && input.scroll != 0.0f) {
            state.scroll = clampf(state.scroll - input.scroll * metrics.rowHeight * 3.0f, 0.0f, maxScroll);
        }
        state.scroll = clampf(state.scroll, 0.0f, maxScroll);

        const glm::vec4 track{outer.z - metrics.padding - metrics.scrollWidth, viewTop, outer.z - metrics.padding,
                              viewBottom};
        drawList.rect(track, metrics.widgetBackground);
        const float fraction = viewHeight / state.contentHeight;
        const float thumbHeight = std::max(viewHeight * fraction, metrics.rowHeight);
        const float travel = viewHeight - thumbHeight;
        const float thumbTop = viewTop + travel * (maxScroll > 0.0f ? state.scroll / maxScroll : 0.0f);
        drawList.rect({track.x, thumbTop, track.z, thumbTop + thumbHeight}, metrics.scrollThumb);
    } else {
        state.scroll = 0.0f;
    }

    drawList.pushClip({outer.x + metrics.border, contentTop, outer.z - metrics.border, outer.w - metrics.border});

    Frame f;
    f.bounds = {outer.x + metrics.padding, viewTop, contentRight, viewBottom};
    f.cursor = {f.bounds.x, f.bounds.y - state.scroll};
    f.id = id;
    frames.push_back(f);
    return true;
}

void Context::endPanel() {
    if (frames.size() <= 1) return;
    const Frame f = frames.back();
    frames.pop_back();

    PanelState& state = panelState(f.id);
    // What the scrollbar above will use next frame. The cursor started at
    // `bounds.y - scroll`, so this is the laid-out height regardless of where it scrolled.
    state.contentHeight = f.cursor.y - (f.bounds.y - state.scroll);

    drawList.popClip();
}


void Context::beginRow(uint32_t columns) {
    Frame row = frames.back();
    row.columns = std::max(columns, 1u);
    row.column = 0;
    row.rowHeight = 0.0f;
    frames.push_back(row);
}

void Context::endRow() {
    if (frames.size() <= 1) return;
    const Frame row = frames.back();
    frames.pop_back();
    float y = row.cursor.y;
    // A row left part-filled still occupies its height. Drop this and a `beginRow(3)` holding
    // two widgets has the next thing drawn on top of them.
    if (row.column != 0) y += row.rowHeight + metrics.spacing;
    frames.back().cursor.y = y;
}

void Context::spacing(float pixels) {
    frames.back().cursor.y += pixels >= 0.0f ? pixels : metrics.spacing;
}

void Context::separator() {
    Frame& f = frames.back();
    f.cursor.y += metrics.spacing;
    drawList.rect({f.bounds.x, f.cursor.y, f.bounds.z, f.cursor.y + metrics.border}, metrics.panelBorder);
    f.cursor.y += metrics.border + metrics.spacing;
}

void Context::image(gfx::ImageId id, float height, float aspect, uint32_t rgba) {
    // The one place a handle becomes a slot, so the one place a destroyed or never-issued one
    // is caught; `slot()` answers the fallback, which draws the font atlas.
    const uint32_t texture = imageTable != nullptr ? imageTable->slot(id) : gfx::ImageTable::kFallbackSlot;

    const float h = height > 0.0f ? height * dpiScale : metrics.rowHeight;
    const glm::vec4 row = allocate(h);

    // An aspect wider than the container is clamped, never allowed to overflow: a panel
    // clipping its own logo reads as a layout bug rather than as a number the caller chose.
    const float full = row.z - row.x;
    const float width = aspect > 0.0f ? std::min(h * aspect, full) : full;
    drawList.image({row.x, row.y, row.x + width, row.w}, texture, rgba);
}

glm::vec4 Context::allocate(float height) {
    Frame& f = frames.back();
    const float h = height > 0.0f ? height : metrics.rowHeight;

    if (f.columns == 0) {
        const glm::vec4 r{f.cursor.x, f.cursor.y, f.bounds.z, f.cursor.y + h};
        f.cursor.y += h + metrics.spacing;
        return r;
    }

    const float total = f.bounds.z - f.bounds.x;
    const float columnWidth = (total - metrics.spacing * static_cast<float>(f.columns - 1)) /
                              static_cast<float>(f.columns);
    const float x = f.bounds.x + static_cast<float>(f.column) * (columnWidth + metrics.spacing);
    const glm::vec4 r{x, f.cursor.y, x + columnWidth, f.cursor.y + h};

    f.rowHeight = std::max(f.rowHeight, h);
    ++f.column;
    if (f.column >= f.columns) {
        f.column = 0;
        f.cursor.y += f.rowHeight + metrics.spacing;
        f.rowHeight = 0.0f;
    }
    return r;
}


void Context::label(const std::string& value) { drawCaption(allocate(), displayText(value), metrics.text); }

void Context::labelDim(const std::string& value) { drawCaption(allocate(), displayText(value), metrics.textDim); }

void Context::labelRight(const std::string& value) {
    const glm::vec4 r = allocate();
    if (font == nullptr) return;
    const std::string shown = displayText(value);
    const float baseline = r.y + (r.w - r.y - font->lineHeight()) * 0.5f + font->ascent();
    drawList.text(*font, r.z - metrics.padding - font->measure(shown), baseline, shown, metrics.text);
}

bool Context::button(const std::string& caption) {
    const Id id = makeId(caption);
    const glm::vec4 r = allocate();
    bool pressed = false;
    bool released = false;
    const bool over = behave(id, r, pressed, released);

    drawList.rect(r, stateColor(id, over));
    drawList.rectOutline(r, focus == id ? metrics.accent : metrics.widgetBorder, metrics.border);

    if (font != nullptr) {
        const std::string shown = displayText(caption);
        const float baseline = r.y + (r.w - r.y - font->lineHeight()) * 0.5f + font->ascent();
        drawList.text(*font, r.x + (r.z - r.x - font->measure(shown)) * 0.5f, baseline, shown, metrics.text);
    }

    if (focus == id && input.enter) return true;
    return released;
}

bool Context::checkbox(const std::string& caption, bool& value) {
    const Id id = makeId(caption);
    const glm::vec4 r = allocate();
    bool pressed = false;
    bool released = false;
    const bool over = behave(id, r, pressed, released);

    const float side = std::min(r.w - r.y, metrics.rowHeight) - 2.0f * metrics.border;
    const glm::vec4 box{r.x + metrics.padding, r.y + (r.w - r.y - side) * 0.5f, r.x + metrics.padding + side,
                        r.y + (r.w - r.y + side) * 0.5f};

    drawList.rect(box, stateColor(id, over));
    drawList.rectOutline(box, focus == id ? metrics.accent : metrics.widgetBorder, metrics.border);
    if (value) {
        const float inset = side * 0.25f;
        drawList.rect({box.x + inset, box.y + inset, box.z - inset, box.w - inset}, metrics.accent);
    }

    if (font != nullptr) {
        const std::string shown = displayText(caption);
        const float baseline = r.y + (r.w - r.y - font->lineHeight()) * 0.5f + font->ascent();
        drawList.text(*font, box.z + metrics.padding, baseline, shown, metrics.text);
    }

    const bool toggled = released || (focus == id && input.enter);
    if (toggled) value = !value;
    return toggled;
}

bool Context::slider(const std::string& caption, float& value, float min, float max) {
    const Id id = makeId(caption);
    const glm::vec4 r = allocate();
    bool pressed = false;
    bool released = false;
    const bool over = behave(id, r, pressed, released);

    const float span = max - min;
    const float before = value;
    if (active == id && span != 0.0f) {
        // Absolute pointer position, never accumulated deltas: a drag that leaves the track
        // and comes back must land where the pointer is, not where the sum got to.
        const float t = clampf((input.mouse.x - r.x) / std::max(r.z - r.x, 1.0f), 0.0f, 1.0f);
        value = min + t * span;
    }
    // Twentieths -- a step somebody can aim with, and the only way a slider reached by Tab
    // can be moved at all.
    if (focus == id && span != 0.0f) {
        if (input.up) value = clampf(value + span * 0.05f, min, max);
        if (input.down) value = clampf(value - span * 0.05f, min, max);
    }
    value = clampf(value, std::min(min, max), std::max(min, max));

    drawList.rect(r, metrics.widgetBackground);
    const float fill = span != 0.0f ? clampf((value - min) / span, 0.0f, 1.0f) : 0.0f;
    drawList.rect({r.x, r.y, r.x + (r.z - r.x) * fill, r.w}, mixColor(metrics.accent, metrics.widgetActive, 0.35f));
    drawList.rectOutline(r, focus == id ? metrics.accent : metrics.widgetBorder, metrics.border);
    if (over || active == id) drawList.rectOutline(r, metrics.widgetHover, metrics.border);

    if (font != nullptr) {
        const float baseline = r.y + (r.w - r.y - font->lineHeight()) * 0.5f + font->ascent();
        drawList.text(*font, r.x + metrics.padding, baseline, displayText(caption), metrics.text);
        char shown[32];
        std::snprintf(shown, sizeof(shown), "%.3f", static_cast<double>(value));
        drawList.text(*font, r.z - metrics.padding - font->measure(shown), baseline, shown, metrics.textDim);
    }

    return value != before;
}

bool Context::sliderInt(const std::string& caption, int& value, int min, int max) {
    float asFloat = static_cast<float>(value);
    const int before = value;
    slider(caption, asFloat, static_cast<float>(min), static_cast<float>(max));
    value = static_cast<int>(std::lround(asFloat));
    return value != before;
}

bool Context::textField(const std::string& caption, std::string& value) {
    const Id id = makeId(caption);
    const glm::vec4 r = allocate();
    bool pressed = false;
    bool released = false;
    const bool over = behave(id, r, pressed, released);
    // `behave` records every widget as non-text and a field must correct it here: Tab
    // traversal reads `focusIsText` off this list, not off the widget it is about to reach.
    if (!focusOrder.empty()) focusOrder.back().isText = true;

    if (pressed) focusIsText = true;

    const bool focused = focus == id;
    bool changed = false;

    if (focused && text != nullptr) {
        focusIsText = true;
        if (textEditing != id) {
            // Taken exactly once. Seed on every focused frame instead and each one overwrites
            // what the user just typed.
            textEditing = id;
            textSeed = value;
            text->setActive(true);
            text->setText(value);
            caretBlink = 0.0f;
        }
        if (text->takeCancelled()) {
            value = textSeed;
            changed = true;
            focus = kNoId;
            focusIsText = false;
            text->setActive(false);
            textEditing = kNoId;
        } else {
            const bool submitted = text->takeSubmitted();
            if (text->text() != value) {
                // Write-through, not commit-on-Enter: gate it on Return and half the edits an
                // inspector makes silently do nothing.
                value = text->text();
                changed = true;
            }
            if (submitted) {
                focus = kNoId;
                focusIsText = false;
                text->setActive(false);
                textEditing = kNoId;
            }
        }
    } else if (textEditing == id) {
        // Focus left without Enter or Escape -- a click elsewhere. That commits.
        if (text != nullptr) {
            if (text->text() != value) {
                value = text->text();
                changed = true;
            }
            text->setActive(false);
        }
        textEditing = kNoId;
    }

    const float split = (r.z - r.x) * 0.4f;
    const glm::vec4 box{r.x + split, r.y, r.z, r.w};

    drawList.rect(box, focused ? metrics.widgetActive : stateColor(id, over));
    drawList.rectOutline(box, focused ? metrics.accent : metrics.widgetBorder, metrics.border);

    if (font != nullptr) {
        const float baseline = r.y + (r.w - r.y - font->lineHeight()) * 0.5f + font->ascent();
        drawList.text(*font, r.x + metrics.padding, baseline, displayText(caption), metrics.text);

        // Clipped to the box, or a string longer than the field runs across the panel. The
        // scroll below is what keeps the caret inside that clip.
        drawList.pushClip({box.x + metrics.border, box.y, box.z - metrics.border, box.w});
        const float inner = box.z - box.x - 2.0f * metrics.padding;
        const float caretX = focused && text != nullptr ? font->measurePrefix(value, text->cursor()) : 0.0f;
        const float shift = std::max(0.0f, caretX - inner);
        drawList.text(*font, box.x + metrics.padding - shift, baseline, value, metrics.text);

        // Half a second on, half a second off.
        if (focused && std::fmod(caretBlink, 1.0f) < 0.5f) {
            const float x = box.x + metrics.padding - shift + caretX;
            drawList.rect({x, box.y + metrics.border * 2.0f, x + metrics.border, box.w - metrics.border * 2.0f},
                          metrics.text);
        }
        drawList.popClip();
    }

    if (focused && text == nullptr) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            core::Logger::warn(core::LogCategory::Render,
                         "UI: a text field took focus but no core::input::TextInput was given to the context -- "
                         "call setTextInput() at startup, or the field cannot be typed into");
        }
    }

    return changed;
}

bool Context::list(const std::string& caption, const std::vector<std::string>& items, uint32_t& selected,
                   float height) {
    const Id id = makeId(caption);
    const glm::vec4 r = allocate(height > 0.0f ? height : metrics.rowHeight * 6.0f);
    PanelState& state = panelState(id);

    bool pressed = false;
    bool released = false;
    const bool over = behave(id, r, pressed, released);

    drawList.rect(r, metrics.widgetBackground);
    drawList.rectOutline(r, focus == id ? metrics.accent : metrics.widgetBorder, metrics.border);

    const float rowHeight = metrics.rowHeight;
    const float viewHeight = r.w - r.y - 2.0f * metrics.border;
    const float contentHeight = static_cast<float>(items.size()) * rowHeight;
    const bool scrolls = contentHeight > viewHeight + 0.5f;
    const float maxScroll = std::max(0.0f, contentHeight - viewHeight);
    float right = r.z - metrics.border;

    if (scrolls) {
        right -= metrics.scrollWidth;
        if (over && input.scroll != 0.0f) state.scroll -= input.scroll * rowHeight * 3.0f;
    }
    state.scroll = clampf(state.scroll, 0.0f, maxScroll);
    state.contentHeight = contentHeight;

    const uint32_t before = selected;

    if (focus == id && !items.empty()) {
        if (input.down && selected + 1 < items.size()) ++selected;
        if (input.up && selected > 0) --selected;
    }

    drawList.pushClip({r.x + metrics.border, r.y + metrics.border, right, r.w - metrics.border});

    for (size_t i = 0; i < items.size(); ++i) {
        const float top = r.y + metrics.border + static_cast<float>(i) * rowHeight - state.scroll;
        const glm::vec4 row{r.x + metrics.border, top, right, top + rowHeight};
        // Cheap reject before the hit test and the six vertices: a thousand-row list draws the
        // twenty that are visible.
        if (row.w < r.y || row.y > r.w) continue;

        const bool rowOver = hitTest(row) && contains(drawList.clip(), input.mouse);
        if (rowOver && input.mousePressed) {
            selected = static_cast<uint32_t>(i);
            focus = id;
            focusIsText = false;
        }

        if (selected == i) {
            drawList.rect(row, mixColor(metrics.accent, metrics.widgetBackground, 0.55f));
        } else if (rowOver) {
            drawList.rect(row, metrics.widgetHover);
        }
        drawCaption(row, displayText(items[i]), metrics.text);
    }

    drawList.popClip();

    if (scrolls) {
        const glm::vec4 track{right, r.y + metrics.border, r.z - metrics.border, r.w - metrics.border};
        drawList.rect(track, mixColor(metrics.widgetBackground, 0x00000000u, 0.25f));
        const float thumbHeight = std::max(viewHeight * (viewHeight / contentHeight), rowHeight);
        const float travel = viewHeight - thumbHeight;
        const float top = track.y + travel * (maxScroll > 0.0f ? state.scroll / maxScroll : 0.0f);
        drawList.rect({track.x, top, track.z, top + thumbHeight}, metrics.scrollThumb);
    }

    // Scrolls to the selection after a keyboard move, so Up and Down cannot walk it off screen.
    if (focus == id && selected < items.size()) {
        const float top = static_cast<float>(selected) * rowHeight;
        if (top < state.scroll) state.scroll = top;
        if (top + rowHeight > state.scroll + viewHeight) state.scroll = top + rowHeight - viewHeight;
        state.scroll = clampf(state.scroll, 0.0f, maxScroll);
    }

    return selected != before;
}

} // namespace ui
