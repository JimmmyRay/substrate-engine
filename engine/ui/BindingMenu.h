#pragma once

#include "core/Input.h"

#include <string>
#include <vector>

namespace ui {

/**
 * @brief The on-screen rebinder, and the engine's first text field.
 *
 * Up/Down move the selection, Enter arms a capture -- the next control pressed becomes the
 * binding -- F5 restores the default, F2 writes the changed rows to the config file, and typing
 * filters the list.
 *
 * Produces lines of text, not draw calls. Reach for a device from here and this file leaves
 * `SUBSTRATE_HOSTED_SOURCES` and stops being testable without one.
 *
 * While the menu is open the keyboard belongs to it, so its own controls are read as raw keys
 * rather than bound actions -- bind them and they become a list of things a player must not
 * rebind. `Menu.Bindings` is the exception and is exempt from text mode: a key that dismisses a
 * text field cannot be disabled by the field being open.
 */
class BindingMenu {
  public:
    void declareActions(core::input::InputMap& map);

    /**
     * @brief Run one frame of the menu.
     *
     * Call after `InputMap::beginFrame` and before anything that acts on actions.
     * Sets text mode and the field's focus for the *next* frame, which is the frame
     * the suppression takes effect on -- one frame of latency on the frame the menu
     * opens, and none after.
     */
    void update(core::input::InputMap& map, core::input::TextInput& text);

    [[nodiscard]] bool open() const { return openFlag; }
    /// Empty when closed, which is what tells the renderer there is nothing to draw.
    [[nodiscard]] const std::vector<std::string>& lines() const { return rendered; }

    /// Config file F2 writes to. Must be the one the run was launched with, or a rebind lands
    /// in whatever `substrate.json` sits beside the working directory.
    std::string configPath = "substrate.json";
    /// Action rows on screen at once. The rest are a page away, not missing.
    uint32_t visibleRows = 12;

  private:
    void rebuild(const core::input::InputMap& map);

    core::input::ActionId toggle = core::input::kInvalidAction;

    core::input::ActionId pending = core::input::kInvalidAction; ///< action a capture was armed for

    bool openFlag = false;
    uint32_t selected = 0;      ///< index into `matches`
    uint32_t firstRow = 0;      ///< top of the visible window, also into `matches`
    std::string filter;         ///< lower-cased copy of the text field, for matching
    std::string filterDisplay;  ///< the field verbatim, for drawing
    size_t filterCursor = 0;    ///< byte offset of the caret within filterDisplay
    std::string status;         ///< last thing that happened, shown under the list

    std::vector<core::input::ActionId> matches;
    std::vector<std::string> rendered;
};

} // namespace ui
