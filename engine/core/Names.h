#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/**
 * @file engine/core/Names.h
 * @brief One name list per enum, and the three things anyone ever asks of one.
 *
 * One list per enum, beside the enum it names, with both directions derived from it. A
 * second list -- a separate parser, or a separate spelling function -- is free to disagree
 * with this one, and an enumerator added to only one of the two is reachable from code and
 * from no name at all.
 *
 * The *first* entry naming a value is what `nameOf` returns and what a save writes back;
 * every later spelling of the same value is an input alias only. Reordering a list
 * therefore changes what lands in a config file.
 *
 * `parseName` takes no fallback on purpose: a fallback makes `--tonemap reinhardt` start in
 * ACES and say nothing. What to do about a refusal is the caller's policy, and `legalNames`
 * is what its message prints.
 */
namespace core {

/// One spelling of one value. `name` must have static storage; it is borrowed, not owned.
template <typename T>
struct Named {
    const char* name;
    T value;
};

/// A whole list, borrowed. The list itself has to outlive every `Names` handed out from it,
/// which is why they are `constexpr` arrays in the translation unit owning the enum.
template <typename T>
using Names = std::span<const Named<T>>;

[[nodiscard]] constexpr char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Case-insensitive equality: `DEBUG` and `debug` are the same log level.
[[nodiscard]] constexpr bool namesEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    }
    return true;
}

/// The canonical name of `value` -- the *first* entry naming it -- or `nullptr` when the
/// list does not name it at all. `namesEveryValue` is what keeps that second case from
/// surviving a compile.
template <typename T>
[[nodiscard]] constexpr const char* nameOf(Names<T> table, T value) {
    for (const Named<T>& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return nullptr;
}

/// The value `text` names, or `std::nullopt`. Case-insensitive; aliases count.
template <typename T>
[[nodiscard]] constexpr std::optional<T> parseName(Names<T> table, std::string_view text) {
    for (const Named<T>& entry : table) {
        if (namesEqual(text, entry.name)) return entry.value;
    }
    return std::nullopt;
}

/// Every spelling the list accepts, in its own order. Aliases included: hiding them makes a
/// refusal list fewer legal values than there are.
template <typename T>
[[nodiscard]] inline std::string legalNames(Names<T> table) {
    std::string out;
    for (const Named<T>& entry : table) {
        if (!out.empty()) out += ", ";
        out += entry.name;
    }
    return out;
}

/**
 * @brief Build-time totality: every value below `E::Count` is named by `table`.
 *
 * `static_assert(namesEveryValue(kTonemaps))` beside a list makes an enumerator added
 * without a name a compile error. A list without one lets that reach a build.
 *
 * Needs an ordinal enum with a `Count` sentinel; the masks in `Logger.h` have none and are
 * guarded by the unit suite instead.
 */
template <typename E, size_t N>
[[nodiscard]] constexpr bool namesEveryValue(const Named<E> (&table)[N]) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(E::Count); ++i) {
        bool found = false;
        for (const Named<E>& entry : table) {
            if (static_cast<uint32_t>(entry.value) == i) found = true;
        }
        if (!found) return false;
    }
    return true;
}

} // namespace core
