#pragma once

#include "core/Settings.h"
#include "ui/Ui.h"

#include <string_view>

/**
 * @file SettingsUi.h
 * @brief One widget per settings row, generated from the table.
 *
 * Generated from `SUBSTRATE_SETTINGS` rather than written out by hand, so a key is spelled once.
 *
 * Two kinds of row must stay readouts: `engine.` rows, which `Settings::set` refuses, and
 * `initOnly` rows once `freezeInitOnly()` has run. Give either a widget and it moves, reports
 * success, changes nothing, and logs the table's refusal on every frame of a drag. Which rows
 * those are is the table's declaration -- a row that takes effect only at startup and does not
 * say so is a defect to fix there, where the console and the config loader see it too.
 */
namespace ui {

/**
 * @brief Draw every row of one module, between a `beginPanel` and its `endPanel`.
 *
 * @param module the JSON section -- `"render"`, `"audio"`, `"physics"`. Not a filter over
 *        labels: it is the first half of the key, so what a caller asks for is exactly
 *        what the config file calls it.
 * @return true on any frame a row was written. Nothing has to act on it -- a bound row *is*
 *         the field the renderer reads.
 *
 * Every write goes through `Settings::set`, so a value that arrives here is clamped by the
 * row's own bounds, recorded with its provenance, and pushed into the live field -- the
 * same path the config file and the command line take.
 */
bool drawSettings(Context& ui, core::settings::Settings& settings, std::string_view module);

} // namespace ui
