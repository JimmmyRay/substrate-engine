#include "ui/SettingsUi.h"

#include <string>

namespace ui {

/// Abbreviated rather than pulled in with `using`: `ui::Id` is a widget's identity within
/// a frame and `settings::Id` is a row, and a file that had both unqualified would be one
/// name meaning two things three lines apart.
namespace cs = core::settings;

using cs::Flags;
using cs::Row;
using cs::Source;
using cs::Type;

namespace {

/// The row's own key, minus the module and the dot. `key` is `render.ssrIntensity`, so a
/// caller asking for `"render"` gets it and a caller asking for `"render.ssr"` does not --
/// the separator is part of the match, or `render.ssr` would also claim `render.ssrIntensity`.
bool inModule(const Row& r, std::string_view module) {
    const std::string_view key = r.key;
    return key.size() > module.size() && key.compare(0, module.size(), module) == 0 && key[module.size()] == '.';
}

/// Equal bounds mean unbounded, which is the table's own convention: a byte budget has no
/// ceiling to invent, and a slider with no ends is not a slider.
bool bounded(const Row& r) {
    return r.minimum != r.maximum;
}

} // namespace

bool drawSettings(Context& ui, cs::Settings& settings, std::string_view module) {
    bool changed = false;

    // `rowCount()` rather than `count()`: the second is the engine's own half of the table,
    // and a panel that walked it would draw every row a game declared for no module at all.
    for (uint16_t i = 0; i < settings.rowCount(); ++i) {
        const auto id = static_cast<cs::Id>(i);
        const Row& r = settings.row(id);
        if (!inModule(r, module)) continue;

        // A row nothing would apply is a readout. See the header: a control that moves and
        // does nothing is worse than no control, and which rows those are is the table's
        // declaration rather than this file's guess.
        const bool frozen = (r.flags & Flags::kInitOnly) != 0 && settings.initOnlyFrozen();
        if ((r.flags & Flags::kEngine) != 0 || frozen) {
            ui.beginRow(2);
            ui.labelDim(r.label);
            ui.labelRight(settings.valueString(id));
            ui.endRow();
            continue;
        }

        // Everything the sliders decline -- a string, a 64-bit count, an unbounded number
        // -- is the string door's case, and it is already written: `setFromString` parses
        // exactly what a config file or a console would put here, and refuses the same
        // things with the same message.
        const auto textRow = [&] {
            std::string text = settings.valueString(id);
            if (ui.textField(r.label, text)) {
                changed |= settings.setFromString(r.key, text, Source::Game, "panel");
            }
        };

        switch (r.type) {
            case Type::Bool: {
                bool value = settings.getBool(id);
                if (ui.checkbox(r.label, value)) {
                    changed |= settings.setValue(id, value, Source::Game, "panel");
                }
                break;
            }
            case Type::Float: {
                if (!bounded(r)) { textRow(); break; }
                float value = settings.getFloat(id);
                if (ui.slider(r.label, value, static_cast<float>(r.minimum), static_cast<float>(r.maximum))) {
                    changed |= settings.setValue(id, value, Source::Game, "panel");
                }
                break;
            }
            case Type::Int: {
                if (!bounded(r)) { textRow(); break; }
                int value = settings.getInt(id);
                if (ui.sliderInt(r.label, value, static_cast<int>(r.minimum), static_cast<int>(r.maximum))) {
                    changed |= settings.setValue(id, value, Source::Game, "panel");
                }
                break;
            }
            case Type::Uint: {
                if (!bounded(r)) { textRow(); break; }
                // Through an `int` because that is the widget there is, and every bounded
                // unsigned row's range fits one. The `set` is still the `uint32_t`
                // overload, so the row's own clamp decides the value rather than this cast.
                int value = static_cast<int>(settings.getUint(id));
                if (ui.sliderInt(r.label, value, static_cast<int>(r.minimum), static_cast<int>(r.maximum))) {
                    changed |= settings.setValue(id, static_cast<uint32_t>(value < 0 ? 0 : value), Source::Game,
                                                 "panel");
                }
                break;
            }
            case Type::Uint64:
            case Type::String: textRow(); break;
        }
    }

    return changed;
}

} // namespace ui
