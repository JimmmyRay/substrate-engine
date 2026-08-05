#include "core/Input.h"

#include "core/FileWrite.h"
#include "core/Logger.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace core {

namespace input {
namespace {

/// One row of a code table. The tables are generated from the same X-lists as the enums,
/// so a hand-written row here could go missing a name where a generated one cannot.
struct CodeName {
    int code;
    const char* name;
};

#define SUBSTRATE_CODE_ROW(name, value, glfwMacro) {(value), #name},

const CodeName kKeyNames[] = {SUBSTRATE_KEY_LIST(SUBSTRATE_CODE_ROW)};
const CodeName kMouseNames[] = {SUBSTRATE_MOUSE_LIST(SUBSTRATE_CODE_ROW)};
const CodeName kPadButtonNames[] = {SUBSTRATE_PAD_BUTTON_LIST(SUBSTRATE_CODE_ROW)};
const CodeName kPadAxisNames[] = {SUBSTRATE_PAD_AXIS_LIST(SUBSTRATE_CODE_ROW)};

#undef SUBSTRATE_CODE_ROW

template <size_t N> const char* nameOf(const CodeName (&table)[N], int code) {
    for (const CodeName& row : table) {
        if (row.code == code) return row.name;
    }
    return nullptr;
}

bool equalNoCase(std::string_view a, const char* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(a[i]);
        const auto rhs = static_cast<unsigned char>(b[i]);
        if (rhs == '\0') return false;
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return b[i] == '\0';
}

template <size_t N> int codeOf(const CodeName (&table)[N], std::string_view name) {
    for (const CodeName& row : table) {
        if (equalNoCase(name, row.name)) return row.code;
    }
    return -1;
}

/// Rescales the live part of the range, so the first movement past the deadzone starts from
/// zero rather than jumping to it.
float applyDeadzone(float v, float deadzone) {
    if (deadzone <= 0.0f || deadzone >= 1.0f) return v;
    const float magnitude = std::fabs(v);
    if (magnitude <= deadzone) return 0.0f;
    const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    return v < 0.0f ? -scaled : scaled;
}

/// How far an axis must travel before a capture accepts it as a deliberate press. Well past
/// the deadzone: any closer and a stick resting off-centre binds itself the moment a rebind
/// is armed.
constexpr float kCaptureAxisThreshold = 0.6f;

const std::string kEmptyString;
const std::vector<Binding> kNoBindings;

} // namespace

std::string bindingName(const Binding& b) {
    switch (b.source) {
    case Binding::Source::Key:
        if (const char* n = nameOf(kKeyNames, b.code); n != nullptr) return n;
        return {};
    case Binding::Source::Mouse:
        if (const char* n = nameOf(kMouseNames, b.code); n != nullptr) return std::string("Mouse.") + n;
        return {};
    case Binding::Source::PadButton:
        if (const char* n = nameOf(kPadButtonNames, b.code); n != nullptr) return std::string("Pad.") + n;
        return {};
    case Binding::Source::PadAxis:
        if (const char* n = nameOf(kPadAxisNames, b.code); n != nullptr) {
            return std::string("Pad.") + n + (b.scale < 0.0f ? '-' : '+');
        }
        return {};
    case Binding::Source::Unbound: break;
    }
    return {};
}

Binding bindingFromName(std::string_view text) {
    Binding b;
    if (text.empty()) return b;

    constexpr std::string_view kMousePrefix = "Mouse.";
    constexpr std::string_view kPadPrefix = "Pad.";

    if (text.size() > kMousePrefix.size() && equalNoCase(text.substr(0, kMousePrefix.size()), "Mouse.")) {
        const int code = codeOf(kMouseNames, text.substr(kMousePrefix.size()));
        if (code >= 0) {
            b.source = Binding::Source::Mouse;
            b.code = code;
        }
        return b;
    }

    if (text.size() > kPadPrefix.size() && equalNoCase(text.substr(0, kPadPrefix.size()), "Pad.")) {
        std::string_view rest = text.substr(kPadPrefix.size());
        const char sign = rest.back();
        if (sign == '+' || sign == '-') {
            const int code = codeOf(kPadAxisNames, rest.substr(0, rest.size() - 1));
            if (code >= 0) {
                b.source = Binding::Source::PadAxis;
                b.code = code;
                b.scale = sign == '-' ? -1.0f : 1.0f;
            }
            return b;
        }
        const int code = codeOf(kPadButtonNames, rest);
        if (code >= 0) {
            b.source = Binding::Source::PadButton;
            b.code = code;
        }
        return b;
    }

    if (const int code = codeOf(kKeyNames, text); code >= 0) {
        b.source = Binding::Source::Key;
        b.code = code;
    }
    return b;
}

std::string bindingListName(const std::vector<Binding>& bindings) {
    std::string out;
    for (const Binding& b : bindings) {
        const std::string name = bindingName(b);
        if (name.empty()) continue;
        if (!out.empty()) out += ' ';
        out += name;
    }
    return out;
}

std::vector<Binding> bindingListFromName(std::string_view text) {
    std::vector<Binding> out;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        const size_t start = at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\t') ++at;
        if (at == start) break;

        const std::string_view token = text.substr(start, at - start);
        const Binding b = bindingFromName(token);
        if (b.source == Binding::Source::Unbound) {
            Logger::warn(LogCategory::Input, "Unknown binding \"%.*s\"; ignored", static_cast<int>(token.size()),
                         token.data());
            continue;
        }
        out.push_back(b);
    }
    return out;
}

ActionId InputMap::declare(std::string name, std::string_view defaultBindings) {
    // `findDeclared`, not `find`: `find` cannot see a retired row, and re-declaring one is
    // what revives it with the same id and the bindings the player left on it.
    if (const ActionId existing = findDeclared(name); existing != kInvalidAction) {
        actions[existing].live = true;
        return existing;
    }

    Action a;
    a.name = std::move(name);
    a.defaults = std::string(defaultBindings);
    a.bindings = bindingListFromName(defaultBindings);
    actions.push_back(std::move(a));
    const ActionId id = static_cast<ActionId>(actions.size() - 1);

    // Erased as it is taken. Keeping the row replays the file over a later rebind every
    // time a scheme is retired and revived, because reviving *is* re-declaring. Only the
    // live list moves, so `defaults` still holds what the code shipped.
    for (auto row = parked.begin(); row != parked.end(); ++row) {
        if (row->first != actions[id].name) continue;
        actions[id].bindings = bindingListFromName(row->second);
        parked.erase(row);
        break;
    }
    return id;
}

void InputMap::setParkedBindings(std::vector<std::pair<std::string, std::string>> rows) {
    parked = std::move(rows);
}

std::vector<InputMap::Conflict> InputMap::conflicts() const {
    // Quadratic, and safely so only while this runs once at startup over a table of tens.
    std::vector<Conflict> out;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (!actions[i].live) continue; // A row out of circulation competes for nothing.
        for (const Binding& b : actions[i].bindings) {
            if (b.source == Binding::Source::Unbound) continue;
            for (size_t j = i + 1; j < actions.size(); ++j) {
                if (!actions[j].live) continue;
                // A mouse binding with exactly one side pointer-mode exempt is not a
                // collision: `resolve` already returns zero for the non-exempt one whenever
                // the exempt one can fire. Dropping this makes the query disagree with the
                // resolver, and reports `Ui.Click` against every game action on Mouse.Left.
                if (b.source == Binding::Source::Mouse &&
                    actions[i].pointerExempt != actions[j].pointerExempt) {
                    continue;
                }
                for (const Binding& other : actions[j].bindings) {
                    if (other == b) {
                        out.push_back({static_cast<ActionId>(i), static_cast<ActionId>(j), b});
                        break; // One report per pair per binding, not per duplicate within.
                    }
                }
            }
        }
    }
    return out;
}

ActionId InputMap::find(std::string_view name) const {
    const ActionId id = findDeclared(name);
    return actionLive(id) ? id : kInvalidAction;
}

ActionId InputMap::findDeclared(std::string_view name) const {
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].name == name) return static_cast<ActionId>(i);
    }
    return kInvalidAction;
}

void InputMap::retire(ActionId id) {
    if (id >= actions.size()) return;
    actions[id].live = false;
    // Zeroed now rather than left to `beginFrame`, which skips dead rows: a read before the
    // next frame would still see the action held, and a row revived later would fire an
    // edge out of the state it was retired holding.
    for (ActionState& s : actions[id].state) s = ActionState{};
}

bool InputMap::actionLive(ActionId id) const { return id < actions.size() && actions[id].live; }

const std::string& InputMap::actionName(ActionId id) const {
    return id < actions.size() ? actions[id].name : kEmptyString;
}

void InputMap::bind(ActionId id, const Binding& b) {
    if (id >= actions.size() || b.source == Binding::Source::Unbound) return;
    std::vector<Binding>& list = actions[id].bindings;
    if (std::find(list.begin(), list.end(), b) != list.end()) return;
    list.push_back(b);
}

void InputMap::setBindings(ActionId id, std::string_view list) {
    if (id >= actions.size()) return;
    actions[id].bindings = bindingListFromName(list);
}

void InputMap::setDefaultBindings(ActionId id, std::string_view list) {
    if (id >= actions.size()) return;
    actions[id].defaults = std::string(list);
    actions[id].bindings = bindingListFromName(actions[id].defaults);
}

void InputMap::clearBindings(ActionId id) {
    if (id < actions.size()) actions[id].bindings.clear();
}

const std::vector<Binding>& InputMap::bindings(ActionId id) const {
    return id < actions.size() ? actions[id].bindings : kNoBindings;
}

std::string InputMap::bindingList(ActionId id) const { return bindingListName(bindings(id)); }

const std::string& InputMap::defaultBindingList(ActionId id) const {
    return id < actions.size() ? actions[id].defaults : kEmptyString;
}

bool InputMap::isDefault(ActionId id) const {
    if (id >= actions.size()) return true;
    return actions[id].bindings == bindingListFromName(actions[id].defaults);
}

void InputMap::resetToDefault(ActionId id) {
    if (id < actions.size()) actions[id].bindings = bindingListFromName(actions[id].defaults);
}


void InputMap::onKey(Key key, bool down) {
    const int code = static_cast<int>(key);
    if (code < 0 || code >= kKeyCodeCount) return;
    keys[code] = down;
    if (down) keyPressedEvents[code] = true;
}

void InputMap::onMouseButton(MouseButton button, bool down) {
    const int code = static_cast<int>(button);
    if (code < 0 || code >= kMouseButtonCount) return;
    mouse[code] = down;
    if (down) mousePressedEvents[code] = true;
}

void InputMap::onCursorPos(double x, double y) {
    if (!cursorSeen) {
        // The first report is a position, not a movement. Without seeding the previous
        // frame, the pointer's distance from the window origin becomes one enormous delta.
        cursorPrevFrame[0] = x;
        cursorPrevFrame[1] = y;
        cursorSeen = true;
    }
    cursor[0] = x;
    cursor[1] = y;
}

void InputMap::onScroll(double yoffset) { scrollAccum += yoffset; }

void InputMap::setGamepad(const GamepadState& pad, uint32_t index) {
    if (index >= kMaxPads) return;
    if (index >= pads.size()) pads.resize(index + 1);
    pads[index] = pad;
}

void InputMap::setPlayerCount(uint32_t count) {
    // At least one, always: a map with no players resolves nothing, and every query in the
    // engine goes quiet with no error anywhere.
    const size_t want = std::max<size_t>(1, count);
    // Player 0's devices are kept and the new players get none -- see the header.
    players.resize(want);
    for (Action& a : actions) a.state.resize(want);
}

void InputMap::setPlayerDevices(uint32_t player, PlayerDevices devices) {
    if (player < players.size()) players[player] = devices;
}

PlayerDevices InputMap::playerDevices(uint32_t player) const {
    return player < players.size() ? players[player] : PlayerDevices{};
}

bool InputMap::gamepadConnected() const {
    return std::any_of(pads.begin(), pads.end(), [](const GamepadState& p) { return p.connected; });
}

bool InputMap::gamepadConnected(uint32_t pad) const { return pad < pads.size() && pads[pad].connected; }

void InputMap::loseFocus() {
    // No synthetic release events: `beginFrame`'s level test already reports an action that
    // now resolves to nothing as a release.
    for (bool& down : keys) down = false;
    for (bool& down : mouse) down = false;
    for (GamepadState& pad : pads) pad = GamepadState{};
}

float InputMap::resolve(const Binding& b, const PlayerDevices& devices, bool textExempt,
                        bool pointerExempt) const {
    switch (b.source) {
    case Binding::Source::Key:
        if (!devices.keyboard) return 0.0f;
        if (textModeOn && !textExempt) return 0.0f;
        return keys[b.code] ? 1.0f : 0.0f;
    case Binding::Source::Mouse:
        if (!devices.keyboard) return 0.0f;
        if (pointerMode && !pointerExempt) return 0.0f;
        return mouse[b.code] ? 1.0f : 0.0f;
    case Binding::Source::PadButton: {
        for (uint32_t i = 0; i < pads.size(); ++i) {
            if (holdsPad(devices, i) && pads[i].connected && pads[i].buttons[b.code]) return 1.0f;
        }
        return 0.0f;
    }
    case Binding::Source::PadAxis: {
        // The deadzone is applied per pad, *before* the pads are combined. Combining first
        // lets an idle second pad's stick drift -- under the deadzone, and therefore
        // nothing -- become the first pad's movement whenever the first pad is centred.
        float best = 0.0f;
        for (uint32_t i = 0; i < pads.size(); ++i) {
            if (!holdsPad(devices, i) || !pads[i].connected) continue;
            const float v = std::clamp(applyDeadzone(pads[i].axes[b.code], gamepadDeadzone) * b.scale, 0.0f, 1.0f);
            best = std::max(best, v);
        }
        return best;
    }
    case Binding::Source::Unbound: break;
    }
    return 0.0f;
}

bool InputMap::edgePressed(const Binding& b, const PlayerDevices& devices, bool textExempt,
                           bool pointerExempt) const {
    switch (b.source) {
    case Binding::Source::Key:
        return devices.keyboard && (!textModeOn || textExempt) && keyPressedFrame[b.code];
    case Binding::Source::Mouse:
        return devices.keyboard && (!pointerMode || pointerExempt) && mousePressedFrame[b.code];
    case Binding::Source::PadButton: {
        for (uint32_t i = 0; i < pads.size(); ++i) {
            if (!holdsPad(devices, i) || !pads[i].connected) continue;
            const bool before = i < padsLast.size() && padsLast[i].buttons[b.code];
            if (pads[i].buttons[b.code] && !before) return true;
        }
        return false;
    }
    case Binding::Source::PadAxis: {
        // Per pad, so the edge belongs to the stick that crossed. Taking the maximum across
        // pads first lets two players deflecting opposite ways cancel into an edge neither
        // of them made.
        for (uint32_t i = 0; i < pads.size(); ++i) {
            if (!holdsPad(devices, i) || !pads[i].connected) continue;
            const float now = std::clamp(applyDeadzone(pads[i].axes[b.code], gamepadDeadzone) * b.scale, 0.0f, 1.0f);
            const float wasAxis = i < padsLast.size() ? padsLast[i].axes[b.code] : 0.0f;
            const float before = std::clamp(applyDeadzone(wasAxis, gamepadDeadzone) * b.scale, 0.0f, 1.0f);
            if (now >= kDigitalThreshold && before < kDigitalThreshold) return true;
        }
        return false;
    }
    case Binding::Source::Unbound: break;
    }
    return false;
}

bool InputMap::serviceCapture() {
    // Escape first, and never bindable: an armed capture that cannot be abandoned eats the
    // next keystroke whatever the user meant by it.
    if (keyPressedFrame[static_cast<int>(Key::Escape)]) {
        captureTarget = kInvalidAction;
        return true;
    }

    Binding found;
    for (int i = 0; i < kKeyCodeCount && found.source == Binding::Source::Unbound; ++i) {
        if (keyPressedFrame[i]) found = Binding{Binding::Source::Key, i, 1.0f};
    }
    for (int i = 0; i < kMouseButtonCount && found.source == Binding::Source::Unbound; ++i) {
        if (mousePressedFrame[i]) found = Binding{Binding::Source::Mouse, i, 1.0f};
    }
    // Any pad, and the binding produced names none of them: `Pad.A` is the A button of
    // whichever pad the player holds. Recording the pad index here would make one saved
    // profile stop working for a second player on a second pad.
    for (uint32_t p = 0; p < pads.size() && found.source == Binding::Source::Unbound; ++p) {
        if (!pads[p].connected) continue;
        const GamepadState before = p < padsLast.size() ? padsLast[p] : GamepadState{};
        for (int i = 0; i < kPadButtonCount && found.source == Binding::Source::Unbound; ++i) {
            if (pads[p].buttons[i] && !before.buttons[i]) found = Binding{Binding::Source::PadButton, i, 1.0f};
        }
        for (int i = 0; i < kPadAxisCount && found.source == Binding::Source::Unbound; ++i) {
            const float v = applyDeadzone(pads[p].axes[i], gamepadDeadzone);
            if (std::fabs(v) < kCaptureAxisThreshold) continue;
            found = Binding{Binding::Source::PadAxis, i, v < 0.0f ? -1.0f : 1.0f};
        }
    }

    if (found.source == Binding::Source::Unbound) return false;

    if (captureReplace) clearBindings(captureTarget);
    bind(captureTarget, found);
    captureTarget = kInvalidAction;
    return true;
}

void InputMap::beginFrame() {
    // The snapshot comes first: everything below, and everything the caller asks
    // afterwards, reads this frame's edges rather than the accumulator.
    for (int i = 0; i < kKeyCodeCount; ++i) {
        keyPressedFrame[i] = keyPressedEvents[i];
        keyPressedEvents[i] = false;
    }
    for (int i = 0; i < kMouseButtonCount; ++i) {
        mousePressedFrame[i] = mousePressedEvents[i];
        mousePressedEvents[i] = false;
    }

    // Armed on entry, not after servicing, or the control that ends a capture also fires
    // the action it was just bound to.
    const bool suppressed = capturing();
    captureDone = suppressed && serviceCapture();

    if (cursorSeen) {
        cursorDelta[0] = cursor[0] - cursorPrevFrame[0];
        cursorDelta[1] = cursor[1] - cursorPrevFrame[1];
        cursorPrevFrame[0] = cursor[0];
        cursorPrevFrame[1] = cursor[1];
    }
    scroll = scrollAccum;
    scrollAccum = 0.0;

    for (Action& a : actions) {
        // Sized here rather than at `declare`, so an action declared before
        // `setPlayerCount` and one declared after it answer the same number of players.
        if (a.state.size() != players.size()) a.state.resize(players.size());
        // Resized *before* the liveness test, or a revived action answers a stale player
        // count.
        if (!a.live) continue;

        for (size_t p = 0; p < players.size(); ++p) {
            ActionState& s = a.state[p];
            float raw = 0.0f;
            bool anyPressed = false;
            for (const Binding& b : a.bindings) {
                raw = std::max(raw, resolve(b, players[p], a.textExempt, a.pointerExempt));
                anyPressed = anyPressed || edgePressed(b, players[p], a.textExempt, a.pointerExempt);
            }
            const bool rawHeld = raw >= kDigitalThreshold;

            if (suppressed) {
                s.value = 0.0f;
                s.held = false;
                s.pressed = false;
                s.released = false;
            } else {
                // A tap -- down and back up inside one frame -- fires both edges, because
                // the level test sees nothing at either end. The `!rawHeld && !heldLast`
                // guard keeps this about the *action*: without it, releasing one of two
                // bound keys while the other stays down reads as a release.
                //
                // The stated limit: a control released and pressed again inside one frame
                // while the action was already held reports no new edge.
                const bool tapped = anyPressed && !rawHeld && !s.heldLast;

                s.value = raw;
                s.held = rawHeld;
                s.pressed = (rawHeld && !s.heldLast) || tapped;
                s.released = (!rawHeld && s.heldLast) || tapped;
            }
            // Outside the `suppressed` branch on purpose: without tracking through a
            // suppressed frame, releasing the key that ended a capture reads as a press on
            // the frame after it.
            s.heldLast = rawHeld;
        }
    }

    padsLast = pads;
}

// A player past the end and a retired action both answer as an unbound action does, which
// is what makes shrinking the player count safe. `actionLive` is tested here as well as
// zeroed by `retire`, so a read between the retirement and the next frame sees it gone.
float InputMap::value(ActionId id, uint32_t player) const {
    return actionLive(id) && player < actions[id].state.size() ? actions[id].state[player].value : 0.0f;
}
bool InputMap::held(ActionId id, uint32_t player) const {
    return actionLive(id) && player < actions[id].state.size() && actions[id].state[player].held;
}
bool InputMap::pressed(ActionId id, uint32_t player) const {
    return actionLive(id) && player < actions[id].state.size() && actions[id].state[player].pressed;
}
bool InputMap::released(ActionId id, uint32_t player) const {
    return actionLive(id) && player < actions[id].state.size() && actions[id].state[player].released;
}

bool InputMap::keyDown(Key key) const {
    const int code = static_cast<int>(key);
    return code >= 0 && code < kKeyCodeCount && keys[code];
}

bool InputMap::keyPressed(Key key) const {
    const int code = static_cast<int>(key);
    return code >= 0 && code < kKeyCodeCount && keyPressedFrame[code];
}

void InputMap::beginCapture(ActionId id, bool replace) {
    if (id >= actions.size()) return;
    captureTarget = id;
    captureReplace = replace;
    captureDone = false;
}

void InputMap::cancelCapture() { captureTarget = kInvalidAction; }

void InputMap::setTextModeExempt(ActionId id, bool exempt) {
    if (id < actions.size()) actions[id].textExempt = exempt;
}

void InputMap::setPointerModeExempt(ActionId id, bool exempt) {
    if (id < actions.size()) actions[id].pointerExempt = exempt;
}


namespace {

/// One cursor, so one flag, and it records a *desire*: this translation unit includes no
/// window, so the frame loop is what calls the platform with it.
bool mouseGrabDesired = false;

} // namespace

void mouseGrab() { mouseGrabDesired = true; }
void mouseRelease() { mouseGrabDesired = false; }
bool mouseGrabbed() { return mouseGrabDesired; }
void mouseGrabReset() { mouseGrabDesired = false; }


namespace {

/// What separates one step from the next. Commas so a script needs no shell quoting,
/// whitespace so one pasted out of a log parses as it reads.
constexpr std::string_view kStepSeparators = ", \t\n\r";

} // namespace

bool Script::parse(std::string_view text) {
    // Built locally and swapped in at the end. Parsing into `list` directly leaves a
    // malformed script half-loaded, and a half-applied script reports a result against half
    // a scenario.
    std::vector<Step> parsed;

    for (size_t at = 0; at < text.size();) {
        const size_t begin = text.find_first_not_of(kStepSeparators, at);
        if (begin == std::string_view::npos) break;
        size_t end = text.find_first_of(kStepSeparators, begin);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view token = text.substr(begin, end - begin);
        at = end;

        const auto refuse = [&](const char* why) {
            Logger::error(LogCategory::Input, "Input script: '%.*s' %s; script ignored",
                          static_cast<int>(token.size()), token.data(), why);
        };

        const size_t colon = token.find(':');
        if (colon == std::string_view::npos) {
            refuse("is not <frame>:<action>[+|-]");
            return false;
        }

        const std::string_view digits = token.substr(0, colon);
        uint64_t frame = 0;
        const auto [stopped, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), frame);
        if (ec != std::errc{} || stopped != digits.data() + digits.size()) {
            refuse("does not begin with a frame number");
            return false;
        }

        std::string_view action = token.substr(colon + 1);
        // The pad selector comes off before the edge suffix: `Player.Forward+@1` has to
        // read as player two pushing forward, not as an action nobody declared.
        uint32_t pad = 0;
        if (const size_t at = action.rfind('@'); at != std::string_view::npos) {
            const std::string_view digitsPad = action.substr(at + 1);
            const auto [padStop, padEc] = std::from_chars(digitsPad.data(), digitsPad.data() + digitsPad.size(), pad);
            if (digitsPad.empty() || padEc != std::errc{} || padStop != digitsPad.data() + digitsPad.size() ||
                pad >= kMaxPads) {
                refuse("names no pad after '@'");
                return false;
            }
            action.remove_suffix(action.size() - at);
        }
        // Read before the suffix is stripped: a bare action is a third case, not a
        // defaulted one.
        const char suffix = action.empty() ? '\0' : action.back();
        const bool explicitEdge = suffix == '+' || suffix == '-';
        if (explicitEdge) action.remove_suffix(1);
        if (action.empty()) {
            refuse("names no action");
            return false;
        }

        if (explicitEdge) {
            parsed.push_back({frame, std::string(action), suffix == '+', pad});
        } else {
            // A bare action is a tap: down and up on the same frame, in that order.
            // `beginFrame` reports both edges for exactly this.
            parsed.push_back({frame, std::string(action), true, pad});
            parsed.push_back({frame, std::string(action), false, pad});
        }
    }

    list = std::move(parsed);
    return true;
}

void Script::add(uint64_t frame, std::string action, bool down, uint32_t pad) {
    list.push_back({frame, std::move(action), down, pad});
}

uint64_t Script::lastFrame() const {
    uint64_t last = 0;
    for (const Step& s : list) last = std::max(last, s.frame);
    return last;
}

std::vector<std::string> Script::unknownActions(const InputMap& map) const {
    std::vector<std::string> missing;
    for (const Step& s : list) {
        if (map.find(s.action) != kInvalidAction) continue;
        // Deduplicated: a tap is two steps, so a typo would be reported twice for having
        // been written once.
        if (std::find(missing.begin(), missing.end(), s.action) == missing.end()) missing.push_back(s.action);
    }
    return missing;
}

void Script::apply(InputMap& map, uint64_t frame) const {
    // The pad state is rebuilt by replaying every step up to this frame rather than carried
    // between calls: `setGamepad` takes a whole state, so pressing one button has to
    // restate every other one still down, and the replay is what keeps `apply` a function
    // of the frame index alone. Keys and mouse buttons are level state in the map already.
    //
    // One state per pad the script names, so a script driving only pad 1 leaves pad 0 where
    // the hardware left it.
    std::vector<GamepadState> built(kMaxPads);
    std::vector<bool> touched(kMaxPads, false);

    for (const Step& s : list) {
        if (s.frame > frame) continue;
        const ActionId id = map.find(s.action);
        if (id == kInvalidAction) continue;
        const std::vector<Binding>& bound = map.bindings(id);
        if (bound.empty()) continue;

        const Binding& b = bound.front();
        const bool thisFrame = s.frame == frame;
        switch (b.source) {
        case Binding::Source::Key:
            if (thisFrame) map.onKey(static_cast<Key>(b.code), s.down);
            break;
        case Binding::Source::Mouse:
            if (thisFrame) map.onMouseButton(static_cast<MouseButton>(b.code), s.down);
            break;
        case Binding::Source::PadButton:
            touched[s.pad] = true;
            if (b.code >= 0 && b.code < kPadButtonCount) built[s.pad].buttons[b.code] = s.down;
            break;
        case Binding::Source::PadAxis:
            touched[s.pad] = true;
            // The sign belongs to the binding, not the step: `Camera.Forward` is
            // `Pad.LeftY-`, so pressing it drives that axis to -1 and `resolve` multiplies
            // the scale back out. Full travel rather than just past `kDigitalThreshold`,
            // because an analog action reads the value -- half a stick is half the speed.
            if (b.code >= 0 && b.code < kPadAxisCount) {
                built[s.pad].axes[b.code] = s.down ? std::copysign(1.0f, b.scale) : 0.0f;
            }
            break;
        case Binding::Source::Unbound: break;
        }
    }

    for (uint32_t i = 0; i < kMaxPads; ++i) {
        if (!touched[i]) continue;
        built[i].connected = true;
        map.setGamepad(built[i], i);
    }
}


size_t utf8SequenceLength(const std::string& s, size_t at) {
    if (at >= s.size()) return 0;
    const auto lead = static_cast<unsigned char>(s[at]);
    size_t length = 1;
    if ((lead & 0xE0u) == 0xC0u) length = 2;
    else if ((lead & 0xF0u) == 0xE0u) length = 3;
    else if ((lead & 0xF8u) == 0xF0u) length = 4;
    // A malformed lead byte must advance by one, never zero: a walk that cannot make
    // progress hangs the editor on a byte nobody can see.
    return std::min(length, s.size() - at);
}

void utf8Encode(std::string& out, uint32_t codepoint) {
    if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) return;

    if (codepoint < 0x80u) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800u) {
        out += static_cast<char>(0xC0u | (codepoint >> 6));
        out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint < 0x10000u) {
        out += static_cast<char>(0xE0u | (codepoint >> 12));
        out += static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (codepoint >> 18));
        out += static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
    }
}

namespace {
/// Byte offset of the codepoint before `at`, stepping over continuation bytes.
size_t utf8Previous(const std::string& s, size_t at) {
    if (at == 0) return 0;
    size_t i = at - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}
} // namespace


void TextInput::setActive(bool on) {
    if (activeFlag == on) return;
    activeFlag = on;
    repeatKey = Key::Unknown;
    repeatHeld = 0.0f;
    repeatCount = 0;
    overflowFlag = false;
}

void TextInput::onChar(uint32_t codepoint) {
    if (!activeFlag) return;

    std::string encoded;
    utf8Encode(encoded, codepoint);
    if (encoded.empty()) return;

    if (maxBytes != 0 && buffer.size() + encoded.size() > maxBytes) {
        // Reported rather than silently dropped: a field that stops accepting input with no
        // signal is indistinguishable from a keyboard that stopped working.
        overflowFlag = true;
        return;
    }

    buffer.insert(cursorBytes, encoded);
    cursorBytes += encoded.size();
}

bool TextInput::repeatable(Key key) {
    switch (key) {
    case Key::Backspace:
    case Key::Delete:
    case Key::Left:
    case Key::Right: return true;
    default: return false;
    }
}

void TextInput::apply(Key key) {
    switch (key) {
    case Key::Backspace: {
        if (cursorBytes == 0) return;
        const size_t start = utf8Previous(buffer, cursorBytes);
        buffer.erase(start, cursorBytes - start);
        cursorBytes = start;
        return;
    }
    case Key::Delete: {
        const size_t length = utf8SequenceLength(buffer, cursorBytes);
        if (length == 0) return;
        buffer.erase(cursorBytes, length);
        return;
    }
    case Key::Left: cursorBytes = utf8Previous(buffer, cursorBytes); return;
    case Key::Right: cursorBytes += utf8SequenceLength(buffer, cursorBytes); return;
    case Key::Home: cursorBytes = 0; return;
    case Key::End: cursorBytes = buffer.size(); return;
    case Key::Enter:
    case Key::KpEnter: submitted = true; return;
    case Key::Escape: cancelled = true; return;
    default: return;
    }
}

void TextInput::onKey(Key key, bool down) {
    if (!activeFlag) return;

    if (!down) {
        if (key == repeatKey) repeatKey = Key::Unknown;
        return;
    }

    apply(key);
    if (repeatable(key)) {
        repeatKey = key;
        repeatHeld = 0.0f;
        repeatCount = 0;
    }
}

void TextInput::update(float dt) {
    if (!activeFlag || repeatKey == Key::Unknown || repeatRate <= 0.0f) return;

    repeatHeld += dt;
    while (repeatHeld >= repeatDelay + static_cast<float>(repeatCount) * repeatRate) {
        apply(repeatKey);
        ++repeatCount;
    }
}

void TextInput::setText(std::string value) {
    buffer = std::move(value);
    cursorBytes = buffer.size();
    overflowFlag = false;
}

void TextInput::clear() {
    buffer.clear();
    cursorBytes = 0;
    overflowFlag = false;
}

bool TextInput::takeSubmitted() {
    const bool was = submitted;
    submitted = false;
    return was;
}

bool TextInput::takeCancelled() {
    const bool was = cancelled;
    cancelled = false;
    return was;
}


uint32_t applyBindings(InputMap& map, const std::vector<std::pair<std::string, std::string>>& table) {
    uint32_t applied = 0;
    std::vector<std::pair<std::string, std::string>> park;
    for (const auto& [name, list] : table) {
        // `findDeclared`, so a row the game retired before startup finished still receives
        // the player's edit. Through `find` it reads as unknown, falls back to its default,
        // and the next save drops the edit because the value then *is* the default.
        const ActionId id = map.findDeclared(name);
        if (id == kInvalidAction) {
            // Not a warning: this runs once, and a camera declares its rows when it is
            // installed, so unknown and merely early are indistinguishable here. The caller
            // that can tell reads `parkedBindings()`.
            Logger::debug(LogCategory::Input, "Config binds \"%s\", which nothing has declared yet; held",
                          name.c_str());
            park.emplace_back(name, list);
            continue;
        }
        map.setBindings(id, list);
        ++applied;
    }
    map.setParkedBindings(std::move(park));
    return applied;
}

bool saveBindings(const InputMap& map, const std::string& configPath) {
    rapidjson::Document doc;
    doc.SetObject();

    if (std::ifstream in(configPath); in.is_open()) {
        rapidjson::IStreamWrapper stream(in);
        rapidjson::Document existing;
        existing.ParseStream(stream);
        if (existing.HasParseError()) {
            // Refuse rather than overwrite: a config that fails to parse still holds
            // settings, and replacing it with two keys loses every one of them.
            Logger::error(LogCategory::Input, "Cannot save bindings: %s is not valid JSON (%s at %zu)",
                          configPath.c_str(), rapidjson::GetParseError_En(existing.GetParseError()),
                          existing.GetErrorOffset());
            return false;
        }
        if (existing.IsObject()) doc.Swap(existing);
    }

    auto& alloc = doc.GetAllocator();

    if (!doc.HasMember("input") || !doc["input"].IsObject()) {
        doc.RemoveMember("input");
        doc.AddMember("input", rapidjson::Value(rapidjson::kObjectType), alloc);
    }
    rapidjson::Value& section = doc["input"];

    rapidjson::Value bindings(rapidjson::kObjectType);
    uint32_t written = 0;
    for (ActionId id = 0; id < map.actionCount(); ++id) {
        if (map.isDefault(id)) continue;
        const std::string& name = map.actionName(id);

        rapidjson::Value entries(rapidjson::kArrayType);
        for (const Binding& b : map.bindings(id)) {
            const std::string text = bindingName(b);
            entries.PushBack(rapidjson::Value(text.c_str(), static_cast<rapidjson::SizeType>(text.size()), alloc),
                             alloc);
        }
        bindings.AddMember(rapidjson::Value(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), alloc),
                           entries, alloc);
        ++written;
    }

    section.RemoveMember("bindings");
    section.AddMember("bindings", bindings, alloc);

    rapidjson::StringBuffer text;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(text);
    writer.SetIndent(' ', 2);
    doc.Accept(writer);

    // Written beside the config and renamed over it. The file being replaced is the
    // player's whole settings file, and `trunc` would empty it before there is any way to
    // know the write will succeed.
    if (!writeFileAtomically(configPath, std::string(text.GetString()) + '\n', LogCategory::Input, "bindings")) {
        return false;
    }

    Logger::status(LogCategory::Input, "Saved %u rebound action%s to %s", written, written == 1 ? "" : "s",
                   configPath.c_str());
    return true;
}

} // namespace input

} // namespace core
