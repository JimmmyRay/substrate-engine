#include "ui/BindingMenu.h"

#include "core/Logger.h"

#include <algorithm>
#include <cctype>

namespace ui {
namespace {

std::string lowered(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// Widest action name a row will pad to. Past it the bindings column starts late
/// rather than the name being cut: a truncated action name is not a name.
constexpr size_t kNameColumn = 26;

const char* const kHeader = "-- bindings --  Up/Down select  Enter rebind  F5 default  F2 save  Tab close";

} // namespace

void BindingMenu::declareActions(core::input::InputMap& map) {
    toggle = map.declare("Menu.Bindings", "Tab");
    map.setTextModeExempt(toggle, true);
}

void BindingMenu::update(core::input::InputMap& map, core::input::TextInput& text) {
    if (map.pressed(toggle)) {
        openFlag = !openFlag;
        selected = 0;
        firstRow = 0;
        filter.clear();
        filterDisplay.clear();
        filterCursor = 0;
        text.clear();
        status.clear();
        if (!openFlag) map.cancelCapture();
    }

    if (!openFlag) {
        rendered.clear();
        map.setTextMode(false);
        text.setActive(false);
        return;
    }

    // A capture that finished this frame reports before anything else looks at the
    // keyboard, because the control that ended it is still down.
    if (map.captureCompleted() && pending != core::input::kInvalidAction) {
        const std::string list = map.bindingList(pending);
        status = list.empty() ? map.actionName(pending) + " left unbound"
                              : map.actionName(pending) + " -> " + list;
        core::Logger::status(core::LogCategory::Input, "Rebind: %s", status.c_str());
        pending = core::input::kInvalidAction;
    }

    if (map.capturing()) {
        // Nothing else may read the keyboard: whatever is pressed next is the binding,
        // including Tab, Enter and the arrows.
        rebuild(map);
        map.setTextMode(false);
        text.setActive(false);
        return;
    }

    // Enter and Escape arrive through the field rather than as raw keys, because the
    // field is what owns them while it has focus.
    if (text.takeCancelled()) {
        openFlag = false;
        rendered.clear();
        map.setTextMode(false);
        text.setActive(false);
        return;
    }

    if (text.takeSubmitted() && !matches.empty() && selected < matches.size()) {
        pending = matches[selected];
        map.beginCapture(pending, true);
        status.clear();
        rebuild(map);
        map.setTextMode(false);
        text.setActive(false);
        return;
    }

    if (map.keyPressed(core::input::Key::Up) && selected > 0) --selected;
    if (map.keyPressed(core::input::Key::Down)) ++selected;
    // A page moves the window as well as the cursor, so paging down lands on a screen
    // of rows nobody has seen rather than scrolling one line short of it.
    if (map.keyPressed(core::input::Key::PageUp)) {
        selected = selected > visibleRows ? selected - visibleRows : 0;
        firstRow = selected;
    }
    if (map.keyPressed(core::input::Key::PageDown)) {
        selected += visibleRows;
        firstRow = selected;
    }

    if (map.keyPressed(core::input::Key::F5) && selected < matches.size()) {
        const core::input::ActionId id = matches[selected];
        map.resetToDefault(id);
        status = map.actionName(id) + " reset to " + map.bindingList(id);
    }

    if (map.keyPressed(core::input::Key::F2)) {
        status = core::input::saveBindings(map, configPath) ? "saved to " + configPath : "save failed: see the log";
    }

    filterDisplay = text.text();
    filterCursor = text.cursor();
    filter = lowered(filterDisplay);

    rebuild(map);
    map.setTextMode(true);
    text.setActive(true);
}

void BindingMenu::rebuild(const core::input::InputMap& map) {
    // ------------------------------------------------------------- the matches
    matches.clear();
    for (core::input::ActionId id = 0; id < map.actionCount(); ++id) {
        // The count is the table size, retired rows included, so the listing is what asks.
        // Offering to rebind an action nothing resolves is offering a control that does
        // nothing.
        if (!map.actionLive(id)) continue;
        if (!filter.empty() && lowered(map.actionName(id)).find(filter) == std::string::npos) continue;
        matches.push_back(id);
    }

    if (matches.empty()) {
        selected = 0;
        firstRow = 0;
    } else {
        selected = std::min<uint32_t>(selected, static_cast<uint32_t>(matches.size()) - 1);
        const uint32_t rows = std::max<uint32_t>(visibleRows, 1);
        if (selected < firstRow) firstRow = selected;
        if (selected >= firstRow + rows) firstRow = selected - rows + 1;
        const uint32_t maxFirst =
            matches.size() > rows ? static_cast<uint32_t>(matches.size()) - rows : 0;
        firstRow = std::min(firstRow, maxFirst);
    }

    // ---------------------------------------------------------------- the text
    rendered.clear();
    rendered.emplace_back(kHeader);

    std::string filterLine = "filter: ";
    filterLine += filterDisplay.substr(0, std::min(filterCursor, filterDisplay.size()));
    filterLine += '|';
    if (filterCursor < filterDisplay.size()) filterLine += filterDisplay.substr(filterCursor);

    const uint32_t rows = std::max<uint32_t>(visibleRows, 1);
    const uint32_t last = std::min<uint32_t>(firstRow + rows, static_cast<uint32_t>(matches.size()));
    // States the window rather than implying the list ends here, which is the whole
    // difference between paging and silently truncating.
    filterLine += matches.empty() ? "   (no action matches)"
                                  : "   (" + std::to_string(firstRow + 1) + "-" + std::to_string(last) + " of " +
                                        std::to_string(matches.size()) + ")";
    rendered.push_back(filterLine);

    for (uint32_t i = firstRow; i < last; ++i) {
        const core::input::ActionId id = matches[i];
        std::string row = i == selected ? "> " : "  ";
        std::string name = map.actionName(id);
        if (name.size() < kNameColumn) name.append(kNameColumn - name.size(), ' ');
        row += name;
        row += ' ';

        const std::string list = map.bindingList(id);
        row += list.empty() ? "<unbound>" : list;
        if (!map.isDefault(id)) row += "   *";
        rendered.push_back(row);
    }

    if (map.capturing()) {
        rendered.push_back("press a control for " + map.actionName(map.captureAction()) + "  (Escape cancels)");
    } else if (!status.empty()) {
        rendered.push_back(status);
    }
}

} // namespace ui
