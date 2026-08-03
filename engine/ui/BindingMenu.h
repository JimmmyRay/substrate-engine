#pragma once

#include "core/Input.h"

#include <string>
#include <vector>

namespace ui {

/**
 * @brief The on-screen rebinder (S1.4), and the engine's first text field (S1.5).
 *
 * Tab opens a list of every declared action with its current bindings. Up/Down move
 * the selection, Enter arms a capture -- the next control pressed becomes the
 * binding -- F5 restores the default, and F2 writes the changed ones to the config
 * file so the rebind survives a restart. Typing filters the list.
 *
 * It produces *lines of text*, not draw calls: the caller hands `lines()` to the
 * renderer's overlay. That keeps every decision here -- what is selected, what the
 * filter matches, what the capture is waiting for -- testable without a device, and
 * it is why this file sits in `SUBSTRATE_HOSTED_SOURCES` despite living under `ui/`.
 *
 * **While the menu is open, the keyboard belongs to it.** That is exactly what text
 * mode means, so the menu's own controls are read as raw keys rather than as bound
 * actions: nothing else can be listening, and menu keys in the binding list would be
 * a list of things you must not rebind. The single exception is the key that opens
 * and closes it, which is an action (`Menu.Bindings`), is rebindable, and is exempt
 * from text mode -- a key that dismisses a text field cannot be disabled by the field
 * being open.
 *
 * **Scale: generalized.** The list is every action the map holds, whatever that
 * number becomes; `visibleRows` pages through it and the header states the range, so
 * a build with two hundred actions shows twelve and says so rather than showing the
 * first twelve as if they were all of them.
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

    /// Config file F2 writes to. The one the run was launched with, so a rebind lands
    /// in the file that produced it rather than in whatever `substrate.json` is next
    /// to the working directory.
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
