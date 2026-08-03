#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/**
 * @file engine/core/Names.h
 * @brief One name list per enum, and the three things anyone ever asks of one (D12).
 *
 * ## Why this exists
 *
 * A value a person spells by name -- a tonemap operator, a debug view, a log level, an
 * `auto|on|off` -- used to have its names written twice: once in the parser that turned
 * text into the value, and once in the function that turned the value back into text.
 * `gfx::tonemapKey` and a `kTonemapAliases` table were two lists of the same operator's
 * spellings, free to disagree, and an operator added to the enum and to one of them was
 * reachable from code and not from a file with nothing to catch it.
 *
 * So there is exactly one list per enum, it lives beside the enum it names, and both
 * directions are derived from it: `nameOf` is the value's canonical spelling and
 * `parseName` is the inverse. That makes the round trip total by construction --
 * `nameOf(parseName(n)) == n` for every canonical `n`, and `parseName(nameOf(v)) == v`
 * for every `v` -- rather than by two authors agreeing.
 *
 * ## The first entry is canonical, and the rest are input conveniences
 *
 * A value may appear more than once: `warning` beside `warn`, `none` beside `clamp`,
 * `true` beside `on`. The **first** entry naming a value is the one `nameOf` returns, so
 * an alias is a spelling the parser accepts and never a spelling anything writes back. A
 * file that says `none` is rewritten as `clamp` the next time it is saved, which is the
 * point: the alias is an input, the file is output, and a value with two names in the
 * file is the drift this list exists to remove.
 *
 * ## There is no fallback parameter, deliberately
 *
 * `parseName` returns `std::nullopt` for a name the list does not hold. The function it
 * replaced took a `fallback` and its whole contract was the failure -- `--tonemap
 * reinhardt` started in ACES and said nothing -- which is
 * [principles.md](../../docs/architecture/principles.md) section 7's *"a key that parses
 * and does nothing"* committed by the parser. What a caller does about a refusal is the
 * caller's policy and is written out where the refusal happens; `legalNames` is what that
 * message prints so the person reading it does not have to go looking.
 */
namespace core {

/// One spelling of one value. `name` is a literal with static storage: every list here is
/// `constexpr` and lives for the program.
template <typename T>
struct Named {
    const char* name;
    T value;
};

/// A whole list, borrowed. The lists are `constexpr` arrays in the translation unit that
/// owns the enum, and this is what an accessor hands out so a caller in another module can
/// hold one without knowing its length.
template <typename T>
using Names = std::span<const Named<T>>;

/// ASCII only, and that is not a shortcut: a setting's name is a key in a JSON file and an
/// argument on a command line, both of which the ASCII guard already holds to ASCII.
[[nodiscard]] constexpr char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Case-insensitive equality, which is what "matched by name" has always meant here:
/// `DEBUG` and `debug` are the same log level, and a config file that shouts is still a
/// config file.
[[nodiscard]] constexpr bool namesEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    }
    return true;
}

/**
 * @brief The canonical name of `value` -- the **first** entry naming it.
 *
 * `nullptr` when the list names it not at all, which is what lets a caller loop to a
 * sentinel rather than carry a separate length, and what the build-time guard below
 * checks so that case cannot survive a compile.
 */
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

/// Every spelling the list accepts, in its own order -- `critical, error, warn, warning,
/// status, debug`. Aliases are included because a refusal that hid them would be a refusal
/// listing fewer legal values than there are.
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
 * `static_assert(namesEveryValue(kTonemaps))` beside the list is what makes an operator
 * added to the enum and not to the list a **compile error** rather than a value reachable
 * from code and from no name at all. That failure has happened once already, which is why
 * the guard is here rather than only in the suite.
 *
 * It wants an ordinal enum with a `Count` sentinel. The two masks in `Logger.h` --
 * `LogCategory` and `LogOutput` -- have no such thing and are guarded by the unit suite
 * instead, over `AllLogCategories` and over `LogOutput::Both`.
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
