#pragma once

#include "core/Settings.h"
#include "ui/Ui.h"

#include <string_view>

/**
 * @file SettingsUi.h
 * @brief One widget per settings row, generated from the table (G2).
 *
 * ## Why this is not the property registry `Inspector.h` refused
 *
 * `Inspector.h` refused a schema, a type-erased setter and a name-to-offset map, and that
 * refusal stands. It is about *inspecting objects*: nothing names an instance's fields, so
 * a registry would have to be invented, and inventing three abstractions to save writing
 * `ui.slider("X", p.x, ...)` is a bad trade.
 *
 * A setting is the opposite case. The schema already exists -- `SUBSTRATE_SETTINGS` is a
 * name, a type, a range and a label per row, written for the JSON parser and the dump
 * before any panel wanted it -- so this function invents nothing. It walks a table that is
 * already there and is already the authority on what a setting is called and what values
 * it may take. Writing the panel by hand instead is what would add a copy: a fifth
 * spelling of every key, in a fourth place, free to disagree with the other three.
 *
 * ## What it refuses to draw, and why that is the point
 *
 * A generated control over a value nothing applies is worse than no control: it moves,
 * reports success, and does nothing -- the exact failure the settings table was built to
 * remove. Two kinds of row are therefore drawn as **readouts** rather than widgets:
 *
 * - `engine.` rows, which `Settings::set` refuses by design; they report state.
 * - `initOnly` rows once `freezeInitOnly()` has run, because what they sized has been
 *   sized. The table already refuses these *with a reason*; a slider over one would log
 *   that reason on every frame of a drag.
 *
 * Which rows those are is the table's declaration, not this file's opinion. A row that
 * takes effect only at startup and does not say so is a defect in the table, and the fix
 * belongs there -- where the console, the config loader and the dump all see it too.
 */
namespace ui {

/**
 * @brief Draw every row of one module, between a `beginPanel` and its `endPanel`.
 *
 * @param module the JSON section -- `"render"`, `"audio"`, `"physics"`. Not a filter over
 *        labels: it is the first half of the key, so what a caller asks for is exactly
 *        what the config file calls it.
 * @return true on any frame a row was written, for a caller that has something to do
 *         about it. Nothing has to: a bound row *is* the field the renderer reads.
 *
 * Every write goes through `Settings::set`, so a value that arrives here is clamped by the
 * row's own bounds, recorded with its provenance, and pushed into the live field -- the
 * same path the config file and the command line take.
 */
bool drawSettings(Context& ui, core::settings::Settings& settings, std::string_view module);

} // namespace ui
