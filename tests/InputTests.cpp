#include "core/Input.h"

#include "core/Config.h"
#include "core/Logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace core;

namespace fs = std::filesystem;
using namespace input;

/**
 * @file tests/InputTests.cpp
 * @brief The action layer, binding names, capture and the text field (S1).
 *
 * What is worth defending here is not "does W move forward" -- it is the handful of
 * places where the obvious implementation is quietly wrong:
 *
 * - An edge that only compares this frame's held state against last frame's misses a
 *   key tapped and released between two polls. The engine runs at 300 fps against a
 *   1000 Hz keyboard; that gap is real.
 * - A rebind capture that lets the captured key also *fire* the action binds F8 and
 *   toggles SSAO in the same breath.
 * - A backspace that erases one byte cuts a multi-byte character in half and leaves a
 *   string no font can draw.
 * - A save that rewrites the config from the live map deletes every key it did not
 *   parse.
 *
 * Each of those has a test below, and each was checked by breaking the code it covers.
 */

namespace {

/// Silences the warnings the map emits for deliberately bad input. Restores whatever
/// the level was, so a test that wants to see them can still set one.
class Quiet {
  public:
    Quiet() : previous(Logger::level()) { Logger::setLevel(LogLevel::Critical); }
    ~Quiet() { Logger::setLevel(previous); }

  private:
    LogLevel previous;
};

/// A press and its release inside one frame, which is the case `held` alone cannot see.
void tap(InputMap& map, Key key) {
    map.onKey(key, true);
    map.onKey(key, false);
}

GamepadState stickLeft(float x, float y) {
    GamepadState pad;
    pad.connected = true;
    pad.axes[static_cast<int>(PadAxis::LeftX)] = x;
    pad.axes[static_cast<int>(PadAxis::LeftY)] = y;
    return pad;
}

} // namespace

// ============================================================ binding names

TEST(BindingNameTest, KeyMouseButtonAndAxisAllRoundTrip) {
    const char* kNames[] = {"W", "F8", "LeftShift", "GraveAccent", "Num4", "Mouse.Left",
                            "Mouse.Middle", "Pad.A", "Pad.DpadLeft", "Pad.LeftY-", "Pad.RightTrigger+"};
    for (const char* name : kNames) {
        const Binding b = bindingFromName(name);
        EXPECT_NE(b.source, Binding::Source::Unbound) << name;
        EXPECT_EQ(bindingName(b), name) << name;
    }
}

TEST(BindingNameTest, KeyNamesAreCaseInsensitive) {
    EXPECT_EQ(bindingFromName("w").code, static_cast<int>(Key::W));
    EXPECT_EQ(bindingFromName("leftshift").code, static_cast<int>(Key::LeftShift));
    EXPECT_EQ(bindingFromName("mouse.LEFT").source, Binding::Source::Mouse);
}

TEST(BindingNameTest, AnAxisWithoutASignIsNotABinding) {
    Quiet quiet;
    // Half a binding: an axis names a line, and an action needs a direction along it.
    EXPECT_EQ(bindingFromName("Pad.LeftY").source, Binding::Source::Unbound);
    EXPECT_EQ(bindingFromName("Pad.LeftY-").scale, -1.0f);
    EXPECT_EQ(bindingFromName("Pad.LeftY+").scale, 1.0f);
}

TEST(BindingNameTest, UnknownTokensAreDroppedFromAListRatherThanPoisoningIt) {
    Quiet quiet;
    const std::vector<Binding> list = bindingListFromName("W Nonsense Pad.LeftY-");
    ASSERT_EQ(list.size(), 2u) << "the two that parse still bind";
    EXPECT_EQ(bindingListName(list), "W Pad.LeftY-");
}

TEST(BindingNameTest, ExtraWhitespaceParsesTheSame) {
    EXPECT_EQ(bindingListName(bindingListFromName("  W\tSpace  ")), "W Space");
    EXPECT_TRUE(bindingListFromName("   ").empty());
}

// ================================================================== actions

TEST(InputMapTest, DeclareIsIdempotentAndDoesNotResetABinding) {
    InputMap map;
    const ActionId id = map.declare("Jump", "Space");
    map.setBindings(id, "Enter");

    // The second caller must not silently undo the first one's rebind, which is the
    // whole reason this is not "declare, and overwrite what was there".
    EXPECT_EQ(map.declare("Jump", "Space"), id);
    EXPECT_EQ(map.bindingList(id), "Enter");
}

/**
 * Retirement is a tombstone rather than an erase, and every one of these covers a way the
 * erase would have been wrong: an id that quietly means a different action, a player's
 * rebind thrown away by a scheme change, a dead row still competing for a key.
 */
TEST(InputMapTest, ARetiredActionStopsResolvingAndStopsBeingFound) {
    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");
    map.onKey(Key::Space, true);
    map.beginFrame();
    ASSERT_TRUE(map.held(jump));

    map.retire(jump);

    // Before any further frame: a caller that retires an action and then reads it in the
    // same frame has to see it already gone.
    EXPECT_FALSE(map.held(jump));
    EXPECT_FALSE(map.actionLive(jump));
    EXPECT_EQ(map.find("Jump"), kInvalidAction);
    EXPECT_EQ(map.findDeclared("Jump"), jump) << "still declared; only a frame cannot see it";

    map.beginFrame();
    EXPECT_FALSE(map.held(jump)) << "the key is still down and the row must not resolve it";
    EXPECT_EQ(map.value(jump), 0.0f);
    EXPECT_FALSE(map.pressed(jump));
    EXPECT_FALSE(map.released(jump));

    // The id stays valid and keeps naming its own row -- that is the whole difference
    // between this and erasing it.
    EXPECT_EQ(map.actionName(jump), "Jump");
    EXPECT_EQ(map.bindingList(jump), "Space");
    EXPECT_EQ(map.actionCount(), 1u) << "the table size is a loop bound, not a live count";
}

TEST(InputMapTest, RedeclaringARetiredActionRevivesItWithTheSameIdAndTheSameBindings) {
    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");
    map.setBindings(jump, "Enter"); // the player rebound it

    map.retire(jump);
    // A control scheme that comes and goes must not cost the player the edit.
    EXPECT_EQ(map.declare("Jump", "Space"), jump);
    EXPECT_TRUE(map.actionLive(jump));
    EXPECT_EQ(map.bindingList(jump), "Enter");
    EXPECT_EQ(map.find("Jump"), jump);

    map.onKey(Key::Enter, true);
    map.beginFrame();
    EXPECT_TRUE(map.held(jump)) << "a revived row resolves again";
}

TEST(InputMapTest, AnActionDeclaredAfterARetirementGetsAFreshId) {
    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");
    map.retire(jump);
    const ActionId fire = map.declare("Fire", "Mouse.Left");

    EXPECT_NE(fire, jump) << "nothing is reused: the dead row still owns its index";
    EXPECT_EQ(map.actionName(fire), "Fire");
    EXPECT_EQ(map.actionName(jump), "Jump");
    EXPECT_EQ(map.actionCount(), 2u);
}

TEST(InputMapTest, ARetiredActionIsNotCompetingForItsKey) {
    InputMap map;
    const ActionId view = map.declare("View.Albedo", "F2");
    map.declare("Game.Save", "F2");
    ASSERT_EQ(map.conflicts().size(), 1u);

    map.retire(view);
    EXPECT_TRUE(map.conflicts().empty()) << "a row nothing resolves cannot fire on the key";
}

// =============================================================== the pointer

TEST(MouseGrabTest, GrabAndReleaseRoundTripAndNothingCounts) {
    mouseGrabReset();
    EXPECT_FALSE(mouseGrabbed());

    mouseGrab();
    EXPECT_TRUE(mouseGrabbed());
    mouseGrab();
    mouseRelease();
    // One boolean, last writer wins. Two grabs and one release is released, because a
    // refcount would leave the pointer captured by a caller that has already let go.
    EXPECT_FALSE(mouseGrabbed());
}

TEST(MouseGrabTest, TheResetIsWhatKeepsAGrabOutOfTheNextTest) {
    // The state is the process's, and this suite links the file it lives in.
    mouseGrab();
    ASSERT_TRUE(mouseGrabbed());
    mouseGrabReset();
    EXPECT_FALSE(mouseGrabbed());
}

TEST(InputMapTest, HeldPressedAndReleasedAreThreeDifferentQuestions) {
    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");

    map.onKey(Key::Space, true);
    map.beginFrame();
    EXPECT_TRUE(map.held(jump));
    EXPECT_TRUE(map.pressed(jump));
    EXPECT_FALSE(map.released(jump));

    map.beginFrame(); // still down, no new event
    EXPECT_TRUE(map.held(jump));
    EXPECT_FALSE(map.pressed(jump)) << "a held key must not re-fire an edge";

    map.onKey(Key::Space, false);
    map.beginFrame();
    EXPECT_FALSE(map.held(jump));
    EXPECT_TRUE(map.released(jump));
}

TEST(InputMapTest, AKeyTappedBetweenTwoFramesStillCounts) {
    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");

    tap(map, Key::Space);
    map.beginFrame();

    // Held is already false again -- the edge flags are the only record that anything
    // happened, and dropping the input would be indistinguishable from a stuck key.
    EXPECT_FALSE(map.held(jump));
    EXPECT_TRUE(map.pressed(jump));
    EXPECT_TRUE(map.released(jump));

    map.beginFrame();
    EXPECT_FALSE(map.pressed(jump)) << "the flags are consumed by the frame that saw them";
}

TEST(InputMapTest, EitherOfTwoBindingsDrivesTheSameAction) {
    InputMap map;
    const ActionId up = map.declare("Up", "E Space");

    map.onKey(Key::E, true);
    map.beginFrame();
    EXPECT_TRUE(map.held(up));

    map.onKey(Key::E, false);
    map.onKey(Key::Space, true);
    map.beginFrame();
    EXPECT_TRUE(map.held(up)) << "one source releasing does not release the action";
    EXPECT_FALSE(map.released(up));
}

TEST(InputMapTest, LoseFocusReleasesEverythingThatWasDown) {
    InputMap map;
    const ActionId walk = map.declare("Walk", "W");

    map.onKey(Key::W, true);
    map.beginFrame();
    ASSERT_TRUE(map.held(walk));

    map.loseFocus();
    map.beginFrame();
    EXPECT_FALSE(map.held(walk));
    EXPECT_TRUE(map.released(walk)) << "the release event never arrives; the focus loss stands in for it";
}

// ================================================================= gamepad

TEST(InputMapTest, AStickDrivesAnActionAnalogueAndTheDeadzoneRescales) {
    InputMap map;
    map.gamepadDeadzone = 0.2f;
    const ActionId forward = map.declare("Forward", "Pad.LeftY-");

    map.setGamepad(stickLeft(0.0f, -0.1f));
    map.beginFrame();
    EXPECT_FLOAT_EQ(map.value(forward), 0.0f) << "inside the deadzone is exactly zero, not almost";

    map.setGamepad(stickLeft(0.0f, -0.6f));
    map.beginFrame();
    // Rescaled: the live part of the range starts at zero rather than at the deadzone,
    // so the first movement past it is a nudge and not a jump.
    EXPECT_FLOAT_EQ(map.value(forward), 0.5f);
    EXPECT_TRUE(map.held(forward));

    map.setGamepad(stickLeft(0.0f, -1.0f));
    map.beginFrame();
    EXPECT_FLOAT_EQ(map.value(forward), 1.0f);
}

TEST(InputMapTest, AnAxisOnlyDrivesTheDirectionItIsBoundTo) {
    InputMap map;
    const ActionId forward = map.declare("Forward", "Pad.LeftY-");
    const ActionId back = map.declare("Back", "Pad.LeftY+");

    map.setGamepad(stickLeft(0.0f, -1.0f));
    map.beginFrame();
    EXPECT_FLOAT_EQ(map.value(forward), 1.0f);
    EXPECT_FLOAT_EQ(map.value(back), 0.0f) << "the opposite direction reads zero, never a negative";
}

TEST(InputMapTest, AStickCrossingTheThresholdIsAPress) {
    InputMap map;
    const ActionId forward = map.declare("Forward", "Pad.LeftY-");

    map.setGamepad(stickLeft(0.0f, -1.0f));
    map.beginFrame();
    EXPECT_TRUE(map.pressed(forward));

    map.setGamepad(stickLeft(0.0f, -1.0f));
    map.beginFrame();
    EXPECT_FALSE(map.pressed(forward));

    map.setGamepad(stickLeft(0.0f, 0.0f));
    map.beginFrame();
    EXPECT_TRUE(map.released(forward));
}

TEST(InputMapTest, ADisconnectedPadContributesNothing) {
    InputMap map;
    const ActionId forward = map.declare("Forward", "Pad.LeftY- W");

    GamepadState pad = stickLeft(0.0f, -1.0f);
    pad.connected = false;
    map.setGamepad(pad);
    map.beginFrame();
    EXPECT_FLOAT_EQ(map.value(forward), 0.0f);
}

// =============================================================== text mode

TEST(InputMapTest, TextModeSuppressesKeysButNotTheMouse) {
    InputMap map;
    const ActionId walk = map.declare("Walk", "W");
    const ActionId orbit = map.declare("Orbit", "Mouse.Left");

    map.setTextMode(true);
    map.onKey(Key::W, true);
    map.onMouseButton(MouseButton::Left, true);
    map.beginFrame();

    EXPECT_FALSE(map.held(walk)) << "typing \"was\" must not walk";
    EXPECT_FALSE(map.pressed(walk));
    EXPECT_TRUE(map.held(orbit)) << "a mouse button types nothing, so it keeps working";
}

TEST(InputMapTest, AnExemptActionSurvivesTextMode) {
    InputMap map;
    const ActionId menu = map.declare("Menu", "Tab");
    map.setTextModeExempt(menu, true);

    map.setTextMode(true);
    map.onKey(Key::Tab, true);
    map.beginFrame();
    EXPECT_TRUE(map.pressed(menu)) << "the key that closes the field cannot be disabled by the field";
}

TEST(InputMapTest, LeavingTextModeDoesNotFireAnEdgeForAKeyHeldThrough) {
    InputMap map;
    const ActionId walk = map.declare("Walk", "W");

    map.setTextMode(true);
    map.onKey(Key::W, true);
    map.beginFrame();
    ASSERT_FALSE(map.held(walk));

    map.setTextMode(false);
    map.beginFrame();
    EXPECT_TRUE(map.held(walk));
    EXPECT_TRUE(map.pressed(walk)) << "held state resumes, and the frame it resumes on is an edge";
}

// ================================================================= capture

TEST(InputMapTest, CaptureBindsTheNextKeyAndThatKeyDoesNotAlsoFire) {
    InputMap map;
    const ActionId ssao = map.declare("Toggle.Ssao", "F8");
    const ActionId walk = map.declare("Walk", "W");

    map.beginCapture(ssao);
    EXPECT_TRUE(map.capturing());

    map.onKey(Key::W, true);
    map.beginFrame();

    EXPECT_FALSE(map.capturing());
    EXPECT_TRUE(map.captureCompleted());
    EXPECT_EQ(map.bindingList(ssao), "W") << "replace, not append: the default is gone";
    EXPECT_FALSE(map.pressed(walk)) << "binding W and walking forward in the same keystroke is the bug";
    EXPECT_FALSE(map.pressed(ssao));
}

TEST(InputMapTest, TheFrameAfterACaptureDoesNotSeeAPhantomPress) {
    InputMap map;
    const ActionId ssao = map.declare("Toggle.Ssao", "F8");

    map.beginCapture(ssao);
    map.onKey(Key::W, true);
    map.beginFrame();

    map.beginFrame(); // W is still physically down
    EXPECT_TRUE(map.held(ssao));
    EXPECT_FALSE(map.pressed(ssao)) << "the key was already down when it was bound; that is not a new press";
}

TEST(InputMapTest, CaptureCanAppendAnAlternativeInsteadOfReplacing) {
    InputMap map;
    const ActionId ssao = map.declare("Toggle.Ssao", "F8");

    map.beginCapture(ssao, /*replace=*/false);
    map.onMouseButton(MouseButton::Middle, true);
    map.beginFrame();
    EXPECT_EQ(map.bindingList(ssao), "F8 Mouse.Middle");
}

TEST(InputMapTest, EscapeCancelsACaptureAndBindsNothing) {
    InputMap map;
    const ActionId ssao = map.declare("Toggle.Ssao", "F8");

    map.beginCapture(ssao);
    map.onKey(Key::Escape, true);
    map.beginFrame();

    EXPECT_FALSE(map.capturing());
    EXPECT_TRUE(map.captureCompleted());
    EXPECT_EQ(map.bindingList(ssao), "F8") << "an armed capture that cannot be abandoned eats the next keystroke";
}

TEST(InputMapTest, ARestingStickDoesNotBindItself) {
    InputMap map;
    const ActionId ssao = map.declare("Toggle.Ssao", "F8");

    map.beginCapture(ssao);
    map.setGamepad(stickLeft(0.3f, 0.0f)); // off centre, past the deadzone, not a push
    map.beginFrame();
    EXPECT_TRUE(map.capturing()) << "a worn stick must not bind itself the moment a rebind is armed";

    map.setGamepad(stickLeft(0.9f, 0.0f));
    map.beginFrame();
    EXPECT_FALSE(map.capturing());
    EXPECT_EQ(map.bindingList(ssao), "Pad.LeftX+");
}

TEST(InputMapTest, DefaultsAreRememberedSoAResetIsPossible) {
    InputMap map;
    const ActionId ssao = map.declare("Toggle.Ssao", "F8");
    EXPECT_TRUE(map.isDefault(ssao));

    map.setBindings(ssao, "K");
    EXPECT_FALSE(map.isDefault(ssao));

    map.resetToDefault(ssao);
    EXPECT_TRUE(map.isDefault(ssao));
    EXPECT_EQ(map.bindingList(ssao), "F8");
}

// --------------------------------------------------------- a game's defaults (G8)

TEST(InputMapTest, AGamesDefaultMovesBothListsAndSoStillReadsAsADefault) {
    // The case that exists: the engine's camera declares W, and a game whose character
    // wants it moves the camera off. Doing that with `setBindings` is the next test.
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "W Pad.LeftY-");

    map.setDefaultBindings(forward, "Up");
    EXPECT_EQ(map.bindingList(forward), "Up") << "the live list has to move, or the game shipped nothing";
    EXPECT_EQ(map.defaultBindingList(forward), "Up");
    EXPECT_TRUE(map.isDefault(forward)) << "this is a shipped control scheme, not a user edit";

    // And "reset" now means the game's scheme rather than whatever declared the action.
    map.setBindings(forward, "K");
    map.resetToDefault(forward);
    EXPECT_EQ(map.bindingList(forward), "Up");
}

TEST(InputMapTest, SetBindingsLeavesTheDeclaredDefaultBehindAndThatIsTheDifference) {
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "W");

    map.setBindings(forward, "Up");
    EXPECT_FALSE(map.isDefault(forward)) << "which is what makes the binding menu offer to undo it";
    EXPECT_EQ(map.defaultBindingList(forward), "W");
    map.resetToDefault(forward);
    EXPECT_EQ(map.bindingList(forward), "W") << "the camera lands back on W, which is the bug";
}

TEST(InputMapTest, ADefaultMovedByTheGameIsStillOutOfRangeSafe) {
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "W");
    map.setDefaultBindings(kInvalidAction, "Up");
    map.setDefaultBindings(forward + 7, "Up");
    EXPECT_EQ(map.bindingList(forward), "W");
    EXPECT_EQ(map.actionCount(), 1u);
}

// ---------------------------------------------------------------- pointer

TEST(InputMapTest, TheFirstCursorReportIsAPositionNotAMovement) {
    InputMap map;
    map.onCursorPos(800.0, 450.0);
    map.beginFrame();
    EXPECT_DOUBLE_EQ(map.cursorDeltaX(), 0.0) << "otherwise the view snaps by the pointer's distance from the origin";
    EXPECT_DOUBLE_EQ(map.cursorDeltaY(), 0.0);

    map.onCursorPos(810.0, 445.0);
    map.beginFrame();
    EXPECT_DOUBLE_EQ(map.cursorDeltaX(), 10.0);
    EXPECT_DOUBLE_EQ(map.cursorDeltaY(), -5.0);
}

TEST(InputMapTest, ScrollAccumulatesWithinAFrameAndClearsAfterIt) {
    InputMap map;
    map.onScroll(1.0);
    map.onScroll(2.0);
    map.beginFrame();
    EXPECT_DOUBLE_EQ(map.scrollDelta(), 3.0);

    map.beginFrame();
    EXPECT_DOUBLE_EQ(map.scrollDelta(), 0.0);
}

// =================================================================== UTF-8

TEST(Utf8Test, EncodesEveryLengthAndRejectsWhatCannotBeEncoded) {
    std::string out;
    utf8Encode(out, 'A');
    EXPECT_EQ(out, "A");

    out.clear();
    utf8Encode(out, 0xE9u); // e-acute
    EXPECT_EQ(out.size(), 2u);

    out.clear();
    utf8Encode(out, 0x20ACu); // euro sign
    EXPECT_EQ(out.size(), 3u);

    out.clear();
    utf8Encode(out, 0x1F600u); // outside the BMP
    EXPECT_EQ(out.size(), 4u);

    out.clear();
    utf8Encode(out, 0xD800u); // lone surrogate
    utf8Encode(out, 0x110000u);
    EXPECT_TRUE(out.empty()) << "no replacement character: that would hide a platform bug";
}

TEST(Utf8Test, AMalformedLeadByteStillAdvances) {
    std::string s;
    s += static_cast<char>(0x80); // a continuation byte with nothing to continue
    EXPECT_EQ(utf8SequenceLength(s, 0), 1u) << "a walk that cannot advance is an editor that hangs";
    EXPECT_EQ(utf8SequenceLength(s, 1), 0u);
}

TEST(Utf8Test, ATruncatedSequenceIsClampedToWhatIsThere) {
    std::string s;
    s += static_cast<char>(0xE2); // claims three bytes
    s += static_cast<char>(0x82);
    EXPECT_EQ(utf8SequenceLength(s, 0), 2u) << "never past the end of the string";
}

// =============================================================== TextInput

TEST(TextInputTest, InactiveFieldsIgnoreEverything) {
    TextInput field;
    field.onChar('a');
    field.onKey(Key::Backspace, true);
    EXPECT_TRUE(field.text().empty());
}

TEST(TextInputTest, CharactersInsertAtTheCursor) {
    TextInput field;
    field.setActive(true);
    for (const char c : std::string("helo")) field.onChar(static_cast<uint32_t>(c));

    field.onKey(Key::Left, true); // between the l and the o
    field.onChar('l');
    EXPECT_EQ(field.text(), "hello");
    EXPECT_EQ(field.cursor(), 4u) << "the cursor follows what was typed, not the end of the string";
}

TEST(TextInputTest, BackspaceDeletesAWholeCodepoint) {
    TextInput field;
    field.setActive(true);
    field.onChar('a');
    field.onChar(0x20ACu); // three bytes
    ASSERT_EQ(field.text().size(), 4u);

    field.onKey(Key::Backspace, true);
    EXPECT_EQ(field.text(), "a") << "one byte at a time would leave a string no font can draw";
    EXPECT_EQ(field.cursor(), 1u);
}

TEST(TextInputTest, ArrowsStepOverWholeCodepointsToo) {
    TextInput field;
    field.setActive(true);
    field.onChar(0x20ACu);
    field.onChar('x');
    ASSERT_EQ(field.cursor(), 4u);

    field.onKey(Key::Left, true);
    EXPECT_EQ(field.cursor(), 3u);
    field.onKey(Key::Left, true);
    EXPECT_EQ(field.cursor(), 0u);
    field.onKey(Key::Left, true);
    EXPECT_EQ(field.cursor(), 0u) << "the start is a wall, not a wrap";

    field.onKey(Key::Right, true);
    EXPECT_EQ(field.cursor(), 3u);
    field.onKey(Key::End, true);
    EXPECT_EQ(field.cursor(), 4u);
    field.onKey(Key::Home, true);
    EXPECT_EQ(field.cursor(), 0u);
    field.onKey(Key::Delete, true);
    EXPECT_EQ(field.text(), "x");
}

TEST(TextInputTest, RepeatWaitsTheDelayAndThenRunsAtTheRate) {
    TextInput field;
    field.repeatDelay = 0.4f;
    field.repeatRate = 0.1f;
    field.setActive(true);
    field.setText("abcdef");

    field.onKey(Key::Backspace, true);
    EXPECT_EQ(field.text(), "abcde") << "the first deletion is the press, not a repeat";

    field.update(0.3f);
    EXPECT_EQ(field.text(), "abcde") << "still inside the delay";

    field.update(0.15f); // 0.45 held: past the delay, one repeat
    EXPECT_EQ(field.text(), "abcd");

    field.update(0.22f); // 0.67 held: the repeats due at 0.5 and 0.6
    EXPECT_EQ(field.text(), "ab");

    field.onKey(Key::Backspace, false);
    field.update(1.0f);
    EXPECT_EQ(field.text(), "ab") << "release stops the repeat";
}

TEST(TextInputTest, OnlyEditingKeysRepeat) {
    TextInput field;
    field.setActive(true);
    field.onKey(Key::Enter, true);
    EXPECT_TRUE(field.takeSubmitted());

    field.update(10.0f);
    EXPECT_FALSE(field.takeSubmitted()) << "a held Enter must not submit sixty times a second";
}

TEST(TextInputTest, SubmitAndCancelAreConsumedByTheReader) {
    TextInput field;
    field.setActive(true);

    field.onKey(Key::Enter, true);
    EXPECT_TRUE(field.takeSubmitted());
    EXPECT_FALSE(field.takeSubmitted()) << "an event that stays true fires its handler every frame after the first";

    field.onKey(Key::Escape, true);
    EXPECT_TRUE(field.takeCancelled());
    EXPECT_FALSE(field.takeCancelled());
}

TEST(TextInputTest, AFullFieldSaysSoRatherThanSwallowingCharacters) {
    TextInput field;
    field.maxBytes = 4;
    field.setActive(true);
    for (const char c : std::string("abcd")) field.onChar(static_cast<uint32_t>(c));
    EXPECT_FALSE(field.overflowed());

    field.onChar('e');
    EXPECT_EQ(field.text(), "abcd");
    EXPECT_TRUE(field.overflowed()) << "a field that stops accepting input silently looks like a broken keyboard";
}

TEST(TextInputTest, AMultiByteCharacterIsRejectedWholeOrNotAtAll) {
    TextInput field;
    field.maxBytes = 4;
    field.setActive(true);
    field.onChar('a');
    field.onChar('b');
    field.onChar(0x20ACu); // three bytes, only two left
    EXPECT_EQ(field.text(), "ab") << "half a codepoint is worse than none";
    EXPECT_TRUE(field.overflowed());
}

TEST(TextInputTest, DeactivatingClearsAHeldRepeat) {
    TextInput field;
    field.repeatDelay = 0.1f;
    field.repeatRate = 0.1f;
    field.setActive(true);
    field.setText("abc");
    field.onKey(Key::Backspace, true);

    field.setActive(false);
    field.setActive(true);
    field.update(5.0f);
    EXPECT_EQ(field.text(), "ab") << "the key press that started the repeat belonged to the last focus";
}

// ================================================== config load and save

namespace {

class BindingFileTest : public ::testing::Test {
  protected:
    fs::path dir;
    fs::path file;

    void SetUp() override {
        dir = fs::temp_directory_path() / "substrate_input_tests";
        fs::remove_all(dir);
        fs::create_directories(dir);
        file = dir / "substrate.json";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void write(const std::string& text) {
        std::ofstream out(file);
        out << text;
    }

    std::string read() {
        std::ifstream in(file);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
};

} // namespace

TEST_F(BindingFileTest, ApplyBindingsOverridesOnlyWhatItNames) {
    Quiet quiet;
    InputMap map;
    const ActionId walk = map.declare("Walk", "W");
    const ActionId jump = map.declare("Jump", "Space");

    const uint32_t applied = applyBindings(map, {{"Jump", "Enter Pad.A"}, {"Fly", "F"}});
    EXPECT_EQ(applied, 1u) << "an action this build does not have is reported, not invented";
    EXPECT_EQ(map.bindingList(walk), "W");
    EXPECT_EQ(map.bindingList(jump), "Enter Pad.A");
}

TEST_F(BindingFileTest, SaveWritesOnlyTheReboundOnes) {
    InputMap map;
    map.declare("Walk", "W");
    const ActionId jump = map.declare("Jump", "Space");
    map.setBindings(jump, "Enter");

    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_NE(text.find("\"Jump\""), std::string::npos);
    // A default that moves in a later build should still reach anyone who never
    // rebound it, which it cannot if every default was frozen into the file.
    EXPECT_EQ(text.find("\"Walk\""), std::string::npos);
}

TEST_F(BindingFileTest, ADefaultTheGameMovedIsNotWrittenToTheFile) {
    // The third symptom the G8 note names, and the one a person only meets months later:
    // a bindings file holding rows nobody ever touched, which freezes those defaults for
    // good because a later build's changes lose to the file.
    InputMap map;
    map.setDefaultBindings(map.declare("Camera.Forward", "W"), "Up");
    map.setBindings(map.declare("Jump", "Space"), "Enter");

    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_NE(text.find("\"Jump\""), std::string::npos) << "the user really did rebind this one";
    EXPECT_EQ(text.find("Camera.Forward"), std::string::npos);
}

TEST_F(BindingFileTest, ARetiredRowIsStillWrittenIfThePlayerReboundIt) {
    // The deliberate exception to "a dead row is skipped", and the one a reasonable
    // implementation gets wrong: rebind the fly camera, switch to another scheme, quit --
    // the edit must survive the row being inactive at the moment the file was written.
    InputMap map;
    const ActionId fly = map.declare("Camera.Fly", "F");
    map.setBindings(fly, "G");
    map.retire(fly);

    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_NE(text.find("Camera.Fly"), std::string::npos) << "a retired rebind is still the player's";
    EXPECT_NE(text.find("\"G\""), std::string::npos);
    EXPECT_TRUE(map.conflicts().empty()) << "written, and still not resolving";
}

TEST_F(BindingFileTest, ARetiredRowAtItsDefaultIsNoMoreWrittenThanALiveOne) {
    InputMap map;
    map.retire(map.declare("Camera.Fly", "F"));
    map.setBindings(map.declare("Jump", "Space"), "Enter");

    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_EQ(text.find("Camera.Fly"), std::string::npos) << "nobody touched it; the default still moves";
    EXPECT_NE(text.find("\"Jump\""), std::string::npos);
}

TEST_F(BindingFileTest, AConfigRebindReachesAnActionRetiredBeforeItWasApplied) {
    // The scheme switched *before* startup finished rather than during play: a game
    // retires the row inside `Game::init`, which runs before `applyBindings`. A lookup
    // that refused the dead row dropped the edit and then wrote nothing for it on the
    // next save, because the row it left behind was its own default.
    Quiet quiet;
    InputMap map;
    const ActionId fly = map.declare("Camera.Fly", "F");
    map.retire(fly);

    // The second row is the case the warning is *for*, and it is in the same call so that
    // the two cannot collapse back into one answer: retired is applied, undeclared is not.
    const uint32_t applied = applyBindings(map, {{"Camera.Fly", "G"}, {"Camera.Nope", "H"}});
    EXPECT_EQ(applied, 1u) << "a retired action is declared, not unknown";
    EXPECT_EQ(map.findDeclared("Camera.Nope"), kInvalidAction) << "no row was invented for it";

    EXPECT_EQ(map.declare("Camera.Fly", "F"), fly) << "revived with the same id";
    EXPECT_EQ(map.bindingList(fly), "G") << "the player's edit, not the default";
    EXPECT_FALSE(map.isDefault(fly));
}

TEST_F(BindingFileTest, AConfigRebindWaitsForAnActionNothingHasDeclaredYet) {
    // The other half of the case above, and the one no lookup can answer: the row has
    // never been declared at all. A camera declares its actions when it is *installed*,
    // and `applyBindings` runs once, right after `Game::init` -- so a game holding three
    // cameras and installing one has two thirds of its `Camera.*` rows undeclared at the
    // moment the file is read, every time.
    Quiet quiet;
    InputMap map;
    map.declare("Ui.Click", "Mouse.Left");

    const uint32_t applied = applyBindings(map, {{"Camera.Forward", "G"}, {"Camera.Nope", "H"}});
    EXPECT_EQ(applied, 0u) << "nothing is declared yet to receive either row";
    EXPECT_EQ(map.findDeclared("Camera.Forward"), kInvalidAction) << "no row is invented for it either";

    const ActionId forward = map.declare("Camera.Forward", "W Pad.LeftY-");
    EXPECT_EQ(map.bindingList(forward), "G") << "the player's edit, not the default";
    EXPECT_FALSE(map.isDefault(forward)) << "and the next save has to write it";
}

TEST_F(BindingFileTest, AHeldRebindIsTakenOnceAndNotReplayedOverALaterEdit) {
    Quiet quiet;
    InputMap map;
    applyBindings(map, {{"Camera.Forward", "G"}});

    const ActionId forward = map.declare("Camera.Forward", "W");
    ASSERT_EQ(map.bindingList(forward), "G");

    // The scheme comes and goes, and the player moves the row again in between. The
    // revival has to hand back what they have *now*; a store that kept the config row
    // would replay the file over the edit that replaced it.
    map.setBindings(forward, "T");
    map.retire(forward);
    EXPECT_EQ(map.declare("Camera.Forward", "W"), forward) << "revived with the same id";
    EXPECT_EQ(map.bindingList(forward), "T");
}

TEST_F(BindingFileTest, ARowNothingEverDeclaresIsHeldAndNeverWritten) {
    // The bound on the store, from the other end: a row waiting for a declaration is not
    // an action, so nothing walks it for display, for conflicts or for the file. Saving
    // one would invent an action out of a typo and freeze it into the config for good.
    Quiet quiet;
    InputMap map;
    map.setBindings(map.declare("Jump", "Space"), "Enter");
    applyBindings(map, {{"Camera.Nope", "H"}});

    EXPECT_EQ(map.actionCount(), 1u);
    EXPECT_TRUE(map.conflicts().empty());
    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_NE(text.find("\"Jump\""), std::string::npos);
    EXPECT_EQ(text.find("Camera.Nope"), std::string::npos);

    // Still held, and that is what the run's last word about it is read off: a name no
    // declaration ever claimed is the unknown action the warning always meant.
    ASSERT_EQ(map.parkedBindings().size(), 1u);
    EXPECT_EQ(map.parkedBindings().front().first, "Camera.Nope");
}

TEST_F(BindingFileTest, ASecondConfigTableReplacesWhatTheFirstWasHolding) {
    Quiet quiet;
    InputMap map;
    applyBindings(map, {{"Camera.Forward", "G"}});
    applyBindings(map, {{"Camera.Forward", "T"}});
    EXPECT_EQ(map.bindingList(map.declare("Camera.Forward", "W")), "T") << "the last table read is the file";
}

TEST_F(BindingFileTest, SavePreservesKeysItNeverParsed) {
    write(R"({"window": {"width": 1234}, "input": {"gamepadDeadzone": 0.25}})");

    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");
    map.setBindings(jump, "Enter");
    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_NE(text.find("1234"), std::string::npos) << "a save that drops settings it does not understand is a bug";
    EXPECT_NE(text.find("0.25"), std::string::npos);
    EXPECT_NE(text.find("Enter"), std::string::npos);
}

TEST_F(BindingFileTest, SaveReplacesTheOldBindingsRatherThanMergingWithThem) {
    write(R"({"input": {"bindings": {"Jump": ["Enter"], "Walk": ["K"]}}})");

    InputMap map;
    map.declare("Walk", "W"); // back to its default this run
    const ActionId jump = map.declare("Jump", "Space");
    map.setBindings(jump, "Tab");
    ASSERT_TRUE(saveBindings(map, file.string()));

    const std::string text = read();
    EXPECT_NE(text.find("Tab"), std::string::npos);
    EXPECT_EQ(text.find("\"K\""), std::string::npos) << "the live map is the truth once it has been saved";
}

TEST_F(BindingFileTest, SaveRefusesToOverwriteAFileItCannotParse) {
    Quiet quiet;
    write("{ this is not json");

    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");
    map.setBindings(jump, "Enter");

    EXPECT_FALSE(saveBindings(map, file.string()));
    EXPECT_EQ(read(), "{ this is not json") << "a broken config still holds settings; replacing it loses all of them";
}

TEST_F(BindingFileTest, SaveCreatesAFileThatDoesNotExistYet) {
    InputMap map;
    const ActionId jump = map.declare("Jump", "Space");
    map.setBindings(jump, "Enter");

    ASSERT_TRUE(saveBindings(map, file.string()));
    EXPECT_TRUE(fs::exists(file));
    EXPECT_NE(read().find("Enter"), std::string::npos);
}

TEST_F(BindingFileTest, ASavedFileLoadsBackIntoTheSameBindings) {
    InputMap first;
    first.declare("Walk", "W");
    const ActionId jump = first.declare("Jump", "Space");
    first.setBindings(jump, "Enter Pad.A");
    ASSERT_TRUE(saveBindings(first, file.string()));

    // The round trip is the point of S1.2: a rebind has to survive a restart, and the
    // restart is a fresh map declared from the same defaults.
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(file));

    InputMap second;
    second.declare("Walk", "W");
    const ActionId jump2 = second.declare("Jump", "Space");
    applyBindings(second, cfg.input.bindings);

    EXPECT_EQ(second.bindingList(jump2), "Enter Pad.A");
    EXPECT_TRUE(second.isDefault(second.find("Walk")));
}

// ========================================================== collisions between actions

/**
 * `declare` deduplicates by name, which is silent about two actions reaching for the same
 * key. That gap let the demo bind a save onto F2 while the debug views already owned it,
 * and nothing anywhere said so -- one press did both. These cover the query that turned it
 * into a startup line.
 */
TEST(InputMapTest, TwoActionsOnOneKeyAreReported) {
    InputMap map;
    const ActionId view = map.declare("View.Albedo", "F2");
    const ActionId save = map.declare("Game.Save", "F2");

    const auto found = map.conflicts();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].a, view);
    EXPECT_EQ(found[0].b, save);
    EXPECT_EQ(bindingName(found[0].binding), "F2");
}

TEST(InputMapTest, DistinctBindingsDoNotCollide) {
    InputMap map;
    map.declare("View.Albedo", "F2");
    map.declare("Game.Save", "F7");
    map.declare("Player.Jump", "Space Pad.A");
    EXPECT_TRUE(map.conflicts().empty());
}

TEST(InputMapTest, UnboundActionsDoNotCollideWithEachOther) {
    // Three actions with nothing bound is a normal state -- a game that declares an
    // action it has not given a default to -- and reporting it as three collisions would
    // make the check noise that gets ignored.
    InputMap map;
    map.declare("A");
    map.declare("B");
    map.declare("C");
    EXPECT_TRUE(map.conflicts().empty());
}

TEST(InputMapTest, OnlyTheSharedBindingIsReportedNotTheWholeAction) {
    InputMap map;
    map.declare("Walk", "W Up");
    map.declare("Menu.Up", "Up");

    const auto found = map.conflicts();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(bindingName(found[0].binding), "Up");
}

TEST(InputMapTest, AThreeWayCollisionIsThreePairs) {
    // Pairs rather than groups, so a caller can print one line per pair without deciding
    // which member is the incumbent.
    InputMap map;
    map.declare("A", "F2");
    map.declare("B", "F2");
    map.declare("C", "F2");
    EXPECT_EQ(map.conflicts().size(), 3u);
}

TEST(InputMapTest, ARebindCanCreateACollisionAndIsSeen) {
    // Which is why Engine checks after applying the config rather than after declaring:
    // a config file is as able to create a collision as to resolve one.
    InputMap map;
    map.declare("View.Albedo", "F2");
    const ActionId save = map.declare("Game.Save", "F7");
    EXPECT_TRUE(map.conflicts().empty());

    map.setBindings(save, "F2");
    EXPECT_EQ(map.conflicts().size(), 1u);
}

TEST(InputMapTest, ARebindCanResolveOneToo) {
    InputMap map;
    map.declare("View.Albedo", "F2");
    const ActionId save = map.declare("Game.Save", "F2");
    ASSERT_EQ(map.conflicts().size(), 1u);

    map.setBindings(save, "F7");
    EXPECT_TRUE(map.conflicts().empty());
}

TEST(InputMapTest, AnAxisCollidesOnlyWithItsOwnDirection) {
    // "an axis without a direction is half a binding" -- so Pad.LeftY+ and Pad.LeftY- are
    // two bindings, and an engine that treated them as one would report every stick as
    // conflicting with itself.
    InputMap map;
    map.declare("Forward", "Pad.LeftY+");
    map.declare("Back", "Pad.LeftY-");
    EXPECT_TRUE(map.conflicts().empty());

    map.declare("Accelerate", "Pad.LeftY+");
    EXPECT_EQ(map.conflicts().size(), 1u);
}

TEST(InputMapTest, APointerExemptActionDoesNotCollideWithASuppressedOne) {
    // Two actions on Mouse.Left where one is pointer-exempt are correct: pointer mode
    // suppresses the other exactly when the UI can be clicked, so the two are never live
    // together. The query has to agree with `resolve` here or it reports a game that put
    // anything of its own on Mouse.Left as broken, every run, until nobody reads it.
    // `Camera.Orbit` was this pair until it moved to Mouse.Middle; the shape outlives it.
    InputMap map;
    map.declare("Camera.Orbit", "Mouse.Left Mouse.Right");
    const ActionId click = map.declare("Ui.Click", "Mouse.Left");
    ASSERT_EQ(map.conflicts().size(), 1u);

    map.setPointerModeExempt(click, true);
    EXPECT_TRUE(map.conflicts().empty());
}

TEST(InputMapTest, TheExemptionCoversOnlyTheMouse) {
    // Pointer mode suppresses mouse bindings and nothing else -- "a panel taking the mouse
    // should not stop WASD" -- so an exemption must not quietly excuse a keyboard clash.
    InputMap map;
    map.declare("Camera.Orbit", "Mouse.Left F2");
    const ActionId click = map.declare("Ui.Click", "Mouse.Left F2");
    map.setPointerModeExempt(click, true);

    const auto found = map.conflicts();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(bindingName(found[0].binding), "F2");
}

// ============================================================= scripted input (C16)

/**
 * A scripted press has to be a *press*, not a shortcut around one. Every test below is
 * shaped by the thing that would make all of them worthless: an implementation that set
 * the action's `pressed` flag directly would satisfy "the game saw it" and prove nothing
 * about the 49-row binding table this exists to drive. So the interesting ones check the
 * path rather than the outcome -- that a rebind moves what the script fires, that the raw
 * key behind the binding is the one that went down, and that text mode suppresses a
 * scripted key exactly as it suppresses a typed one.
 */

namespace {

/// A stand-in for a game's frame loop: feed the script in where `Engine::beginFrame` feeds
/// it, resolve, and report the frames on which a consumer reading `pressed()` saw `id`.
std::vector<uint64_t> framesPressed(const Script& script, InputMap& map, ActionId id, uint64_t frames) {
    std::vector<uint64_t> seen;
    for (uint64_t frame = 0; frame < frames; ++frame) {
        script.apply(map, frame);
        map.beginFrame();
        if (map.pressed(id)) seen.push_back(frame);
    }
    return seen;
}

} // namespace

TEST(ScriptTest, ATapReachesTheGameAsOnePressOnTheFrameItNamed) {
    // The whole of what C6 could not do: press the key a save is bound to and watch a
    // game read it. On that frame, and -- the half that is easy to get wrong -- on no
    // other, because a level that stayed down would fire `pressed` once and `held`
    // forever.
    InputMap map;
    const ActionId save = map.declare("Game.Save", "F7");

    Script script;
    ASSERT_TRUE(script.parse("3:Game.Save"));

    EXPECT_EQ(framesPressed(script, map, save, 8), std::vector<uint64_t>{3});
}

TEST(ScriptTest, TheEventArrivesAsTheKeyTheBindingNames) {
    // The strongest available statement that this goes through the map rather than past
    // it: the raw key is down. A feed that wrote the action state would leave every key
    // exactly as it found it.
    InputMap map;
    const ActionId save = map.declare("Game.Save", "F7");
    map.setBindings(save, "Enter");

    Script script;
    ASSERT_TRUE(script.parse("0:Game.Save+"));
    script.apply(map, 0);
    map.beginFrame();

    EXPECT_TRUE(map.keyDown(Key::Enter)) << "the script follows the binding, not the action name";
    EXPECT_FALSE(map.keyDown(Key::F7)) << "and not the binding it was declared with, once rebound";
    EXPECT_TRUE(map.pressed(save));
}

TEST(ScriptTest, AnExplicitEdgePairHoldsForExactlyTheFramesBetweenThem) {
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "W");

    Script script;
    ASSERT_TRUE(script.parse("2:Camera.Forward+, 5:Camera.Forward-"));

    std::string held;
    std::vector<uint64_t> releases;
    for (uint64_t frame = 0; frame < 8; ++frame) {
        script.apply(map, frame);
        map.beginFrame();
        held += map.held(forward) ? 'H' : '.';
        if (map.released(forward)) releases.push_back(frame);
    }

    EXPECT_EQ(held, "..HHH...") << "the release frame is not a held frame -- the up arrives before the resolve";
    EXPECT_EQ(releases, std::vector<uint64_t>{5});
}

TEST(ScriptTest, TextModeSuppressesAScriptedKeyJustAsItDoesATypedOne) {
    // If this ever passes with the key firing, the feed has grown a private path into the
    // map and none of the other tests here mean anything.
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "W");
    map.setTextMode(true);

    Script script;
    ASSERT_TRUE(script.parse("0:Camera.Forward+,1:Camera.Forward-"));
    EXPECT_TRUE(framesPressed(script, map, forward, 3).empty());
}

TEST(ScriptTest, AMouseBoundActionGoesInAsAButton) {
    InputMap map;
    const ActionId click = map.declare("Ui.Click", "Mouse.Left");

    Script script;
    script.add(0, "Ui.Click", true);
    script.apply(map, 0);
    map.beginFrame();

    EXPECT_TRUE(map.held(click));
    EXPECT_TRUE(map.pressed(click));
}

TEST(ScriptTest, APadBoundActionIsDrivenToFullTravelInTheBindingsOwnDirection) {
    // `Pad.LeftY-` is one axis with two meanings, so pressing the action bound to the
    // negative half means driving the stick to -1 and letting `resolve` multiply the
    // scale back out. Full travel rather than something just past the digital threshold:
    // the action carries a float, and half a stick would be half the speed.
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "Pad.LeftY-");
    const ActionId back = map.declare("Camera.Back", "Pad.LeftY+");

    Script script;
    script.add(1, "Camera.Forward", true);

    script.apply(map, 1);
    map.beginFrame();
    EXPECT_TRUE(map.held(forward));
    EXPECT_FLOAT_EQ(map.value(forward), 1.0f);
    EXPECT_FALSE(map.held(back)) << "the opposite half of the same axis must read as nothing";
}

TEST(ScriptTest, APadStateIsRebuiltFromEveryEarlierStepRatherThanCarried) {
    // A gamepad is set as a whole state, so pressing a second button has to restate the
    // first one. Replaying from the top is how that is true without `apply` keeping
    // anything between calls -- which is also why a frame may be applied out of order or
    // twice and still be the same frame.
    InputMap map;
    const ActionId fast = map.declare("Camera.Fast", "Pad.RightBumper");
    const ActionId jump = map.declare("Player.Jump", "Pad.A");

    Script script;
    ASSERT_TRUE(script.parse("1:Camera.Fast+,3:Player.Jump+"));

    script.apply(map, 3); // straight to frame 3; nothing before it was ever applied
    map.beginFrame();
    EXPECT_TRUE(map.held(fast));
    EXPECT_TRUE(map.held(jump));
}

TEST(ScriptTest, AnEmptyScriptTouchesNothing) {
    // What twelve byte-identical golden cases actually prove, said once somewhere it can
    // be checked in a millisecond: with no script the feed is not merely quiet, it does
    // not write to the map at all -- including over a device that is genuinely there.
    InputMap map;
    const ActionId forward = map.declare("Camera.Forward", "W Pad.LeftY-");
    map.setGamepad(stickLeft(0.0f, -1.0f));

    const Script script;
    EXPECT_TRUE(script.empty());
    EXPECT_EQ(script.lastFrame(), 0u);

    script.apply(map, 0);
    map.beginFrame();
    EXPECT_TRUE(map.gamepadConnected());
    EXPECT_TRUE(map.held(forward));
}

TEST(ScriptTest, AnActionWithNoBindingAtAllIsSkipped) {
    InputMap map;
    const ActionId save = map.declare("Game.Save", "F7");
    map.clearBindings(save);

    Script script;
    ASSERT_TRUE(script.parse("1:Game.Save"));
    EXPECT_TRUE(framesPressed(script, map, save, 3).empty()) << "there is no device to stand in for";
}

TEST(ScriptTest, CommasAndWhitespaceBothSeparateAndABareActionIsTwoSteps) {
    Script script;
    ASSERT_TRUE(script.parse("  1:Camera.Forward+ ,2:Game.Save,\n30:Camera.Forward-  "));

    ASSERT_EQ(script.steps().size(), 4u);
    EXPECT_EQ(script.steps()[0].frame, 1u);
    EXPECT_TRUE(script.steps()[0].down);
    EXPECT_EQ(script.steps()[1].action, "Game.Save");
    EXPECT_TRUE(script.steps()[1].down);
    EXPECT_EQ(script.steps()[2].frame, 2u);
    EXPECT_FALSE(script.steps()[2].down) << "a tap is a down and an up on the same frame";
    EXPECT_EQ(script.lastFrame(), 30u);
}

TEST(ScriptTest, AMalformedStepLeavesTheWholeScriptAsItWas) {
    // All or nothing, because a script is test infrastructure: one that loaded its first
    // two steps and dropped the third would report a result against half a scenario, and
    // the half it ran is the half nobody looks at.
    Quiet quiet;
    Script script;
    ASSERT_TRUE(script.parse("1:Game.Save"));

    for (const char* bad : {"1:Game.Save,2:Game.Load,notaframe:Game.Load", "3Game.Save", "4:", "5:+", "6:Game.Save,:"}) {
        EXPECT_FALSE(script.parse(bad)) << bad;
        ASSERT_EQ(script.steps().size(), 2u) << bad;
        EXPECT_EQ(script.steps()[0].action, "Game.Save") << bad;
    }
}

TEST(ScriptTest, UnknownActionsAreReportedOnceAndFireNothing) {
    InputMap map;
    map.declare("Game.Save", "F7");

    Script script;
    ASSERT_TRUE(script.parse("1:Game.Save,2:Game.Lode,3:Game.Lode"));

    const std::vector<std::string> missing = script.unknownActions(map);
    ASSERT_EQ(missing.size(), 1u) << "a tap is two steps; a name written twice is still one typo";
    EXPECT_EQ(missing[0], "Game.Lode");
}

// ============================================ a device keeps its identity (C26)

namespace {

GamepadState padButton(PadButton button) {
    GamepadState pad;
    pad.connected = true;
    pad.buttons[static_cast<int>(button)] = true;
    return pad;
}

} // namespace

/**
 * **The row's claim, and the shape local co-op needs.** Every connected pad used to be
 * folded into one state before the map saw it -- any button on any pad read as pressed and
 * each axis took the largest magnitude across all of them -- so the loop enumerated the
 * devices and then threw away which one had acted. Two players deflecting opposite ways
 * was not merely hard to express, it had no expression at all.
 */
TEST(InputPlayers, TwoPadsDeflectingOppositeWaysDriveTwoPlayers) {
    InputMap map;
    const ActionId forward = map.declare("Player.Forward", "W Pad.LeftY-");
    ASSERT_NE(forward, kInvalidAction);

    map.setPlayerCount(2);
    map.setPlayerDevices(0, {true, 1u << 0});
    map.setPlayerDevices(1, {false, 1u << 1});

    // Pad 0 pushes the stick forward, pad 1 pulls it back. Merged, the larger magnitude
    // wins and both players read whatever that one did.
    map.setGamepad(stickLeft(0.0f, -1.0f), 0);
    map.setGamepad(stickLeft(0.0f, 1.0f), 1);
    map.beginFrame();

    EXPECT_NEAR(map.value(forward, 0), 1.0f, 1e-4f);
    EXPECT_TRUE(map.held(forward, 0));
    EXPECT_NEAR(map.value(forward, 1), 0.0f, 1e-4f);
    EXPECT_FALSE(map.held(forward, 1));

    // And the keyboard is player one's alone, which is the half a device index on the
    // binding could not have said: two people share a keyboard all the time.
    map.onKey(Key::W, true);
    map.setGamepad(stickLeft(0.0f, 0.0f), 0);
    map.beginFrame();
    EXPECT_TRUE(map.held(forward, 0));
    EXPECT_FALSE(map.held(forward, 1));
}

TEST(InputPlayers, AButtonEdgeBelongsToThePadThatPressedIt) {
    InputMap map;
    const ActionId jump = map.declare("Player.Jump", "Space Pad.A");
    map.setPlayerCount(2);
    map.setPlayerDevices(0, {true, 1u << 0});
    map.setPlayerDevices(1, {false, 1u << 1});

    map.setGamepad(GamepadState{}, 0);
    map.setGamepad(padButton(PadButton::A), 1);
    map.beginFrame();

    EXPECT_FALSE(map.pressed(jump, 0));
    EXPECT_TRUE(map.pressed(jump, 1));
    // Held on the next frame, pressed on neither: an edge is once.
    map.beginFrame();
    EXPECT_TRUE(map.held(jump, 1));
    EXPECT_FALSE(map.pressed(jump, 1));
}

/**
 * The second-order defect in the same lines, and the one that was costing a real game
 * something today: the merge took the largest magnitude across every pad *before* the map
 * applied its deadzone, so an idle second pad's stick drift -- which is under the deadzone
 * and therefore nothing -- became the first player's movement whenever their own stick was
 * centred.
 *
 * The arm that would have failed is the second: 0.08 is under the 0.15 deadzone and used to
 * survive the max, because the max ran first.
 */
TEST(InputPlayers, AnIdlePadsDriftIsDeadzonedBeforeItCanReachAnotherPad) {
    InputMap map;
    const ActionId forward = map.declare("Player.Forward", "Pad.LeftY-");
    // One player holding every pad, which is the single-player default and exactly what
    // the merge used to produce.
    map.setGamepad(stickLeft(0.0f, 0.0f), 0);
    map.setGamepad(stickLeft(0.0f, -0.08f), 1);
    map.beginFrame();
    EXPECT_NEAR(map.value(forward), 0.0f, 1e-4f) << "an idle pad's drift reached the action";

    // A real deflection on the second pad still arrives, so this is a deadzone and not a
    // pad that was switched off.
    map.setGamepad(stickLeft(0.0f, -1.0f), 1);
    map.beginFrame();
    EXPECT_NEAR(map.value(forward), 1.0f, 1e-4f);
}

TEST(InputPlayers, OnePlayerHoldingEverythingIsTheDefaultAndIsWhatTheMergeDid) {
    InputMap map;
    const ActionId forward = map.declare("Player.Forward", "W Pad.LeftY-");
    EXPECT_EQ(map.playerCount(), 1u);
    EXPECT_TRUE(map.playerDevices(0).keyboard);

    // Any pad, which is what "the merged pad" meant.
    map.setGamepad(stickLeft(0.0f, -1.0f), 3);
    map.beginFrame();
    EXPECT_TRUE(map.held(forward));
    EXPECT_TRUE(map.gamepadConnected());
    EXPECT_TRUE(map.gamepadConnected(3));
    EXPECT_FALSE(map.gamepadConnected(0));
}

TEST(InputPlayers, APlayerPastTheCountPressesNothingRatherThanReadingPlayerZero) {
    InputMap map;
    const ActionId forward = map.declare("Player.Forward", "W");
    map.onKey(Key::W, true);
    map.beginFrame();

    ASSERT_TRUE(map.held(forward, 0));
    EXPECT_FALSE(map.held(forward, 7)) << "an absent player read another player's hands";
    EXPECT_FALSE(map.pressed(forward, 7));
    EXPECT_NEAR(map.value(forward, 7), 0.0f, 1e-6f);
}

TEST(InputScriptTest, AStepNamesThePadItDrives) {
    InputMap map;
    const ActionId forward = map.declare("Player.Forward", "Pad.LeftY-");
    map.setPlayerCount(2);
    map.setPlayerDevices(0, {true, 1u << 0});
    map.setPlayerDevices(1, {false, 1u << 1});

    Script script;
    ASSERT_TRUE(script.parse("5:Player.Forward+@1"));
    ASSERT_EQ(script.steps().size(), 1u);
    EXPECT_EQ(script.steps()[0].pad, 1u);

    script.apply(map, 5);
    map.beginFrame();
    EXPECT_FALSE(map.held(forward, 0)) << "the script drove a pad nobody named";
    EXPECT_TRUE(map.held(forward, 1));
}

TEST(InputScriptTest, APadSelectorThatNamesNoPadIsRefusedWholesale) {
    Script script;
    ASSERT_TRUE(script.parse("5:Player.Forward+"));
    EXPECT_FALSE(script.parse("5:Player.Forward+@"));
    EXPECT_FALSE(script.parse("5:Player.Forward+@99"));
    // All or nothing: the script that parsed is still the one loaded.
    ASSERT_EQ(script.steps().size(), 1u);
    EXPECT_EQ(script.steps()[0].pad, 0u);
}
