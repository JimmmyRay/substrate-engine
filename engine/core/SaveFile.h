#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file engine/core/SaveFile.h
 * @brief A versioned byte stream, in sections (C6).
 *
 * ## What this is not
 *
 * Not a `SaveGame` hierarchy, not an `ISerializable`, not a reflection system. A save
 * system is the classic place for all three to appear and all three are refused in
 * writing: **C6 is two virtuals and a byte stream.** `Game::save` and `Game::load` get a
 * writer and a reader; what a game puts in them is a game's business, and the engine has
 * no opinion beyond the framing.
 *
 * ## Sections, and why the file has a table of contents
 *
 * A save has at least two authors -- the engine and the game -- and they version
 * independently. A file is therefore a list of named sections, each with its own version
 * and byte length, and the list is at the front. Three things follow, and each is a
 * property the disposition table promised:
 *
 * - **A version this build does not know is refused with a reason**, before a byte of that
 *   section is consumed. `section()` returns false and `reason()` says why.
 * - **A section this build does not know is skipped**, not fatal. A save written by a
 *   later build still loads its engine half into an earlier one, which is what makes a
 *   version number a compatibility statement rather than a wall.
 * - **No fixed record count.** A section is a length, so what is inside it is whatever the
 *   author streamed.
 *
 * ## Refusal, not partial application
 *
 * The reader hands out data; it cannot un-apply what a caller already did with it. So the
 * rule is the caller's to keep, and the engine keeps it for its own section: read
 * everything, check it against the world it is being loaded into, and only then write
 * anything. `Engine::loadGame` does exactly that and refuses a save from a different scene
 * rather than scattering one scene's transforms over another's.
 *
 * A game's section is the game's to make atomic, and `section()` returning false *before*
 * any of its data is consumed is the tool for it.
 */
namespace core {

/// Bumped when the framing changes -- the header, the table of contents, the primitive
/// encodings. Not when a *section's* contents change; that is what a section version is.
inline constexpr uint32_t kSaveFileVersion = 1;

/**
 * @brief Append-only. Build the whole file in memory, then write it once.
 *
 * A save is a few hundred kilobytes and written when a person asks, so the simple shape
 * is the right one: no seeking, no partial flush, and a file that either exists complete
 * or was never renamed into place.
 */
class SaveWriter {
  public:
    /**
     * @brief Open a section. Closes the previous one.
     *
     * @param name    Up to 15 bytes, so the table of contents is fixed-width and a reader
     *                can walk it without allocating. Longer is truncated and said so.
     * @param version The caller's own, and the number a future reader compares against
     *                what it understands.
     */
    void beginSection(std::string_view name, uint32_t version);

    void u32(uint32_t value);
    void u64(uint64_t value);
    void i32(int32_t value);
    void f32(float value);
    void boolean(bool value);
    void vec3(const glm::vec3& value);
    void vec4(const glm::vec4& value);
    void quat(const glm::quat& value);
    void mat4(const glm::mat4& value);
    /// Length-prefixed. No terminator, so an embedded zero survives.
    void text(std::string_view value);
    void blob(const void* data, size_t bytes);

    /// Write to `path`, through a temporary and a rename. A save interrupted mid-write
    /// leaves the previous save rather than a truncated one, which for the one file a
    /// player would be upset to lose is worth the rename.
    [[nodiscard]] bool write(const std::filesystem::path& path);

    /// The bytes as they stand. For a test, and for a caller that wants the file
    /// somewhere this class does not know how to put it.
    [[nodiscard]] const std::vector<uint8_t>& bytes() const { return payload; }
    [[nodiscard]] size_t sectionCount() const { return toc.size(); }

  private:
    struct Entry {
        char name[16]{};
        uint32_t version = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    void closeSection();

    std::vector<Entry> toc;
    std::vector<uint8_t> payload;
    bool open = false;
};

/**
 * @brief Bounds-checked, forward-only within a section.
 *
 * Every read is checked and a failed read sets `ok()` false for good: a reader that went
 * off the end once cannot be trusted for anything after, and returning zeros while
 * pretending otherwise is how a corrupt save turns into a world that is subtly wrong
 * instead of one that refused to load.
 */
class SaveReader {
  public:
    /**
     * @brief Read `path` and parse its table of contents.
     *
     * @return false for a missing file, one that is not a save, one whose *framing*
     *         version this build does not know, or one whose table of contents does not
     *         agree with its own length. `reason()` is set in every case.
     */
    [[nodiscard]] bool open(const std::filesystem::path& path);
    /// The same, over bytes already in hand. What the tests use, and what a caller with a
    /// save from somewhere other than a file would.
    [[nodiscard]] bool openBytes(std::vector<uint8_t> data);

    /**
     * @brief Seek to a section, if this build can read it.
     *
     * @param knownVersion the highest version of this section the caller understands.
     * @return false when the section is absent, or when its version is higher than
     *         `knownVersion` -- and in the second case `reason()` says so by name and
     *         number. **Nothing has been consumed either way**, which is what lets a
     *         caller decide whether a section it cannot read is fatal.
     */
    [[nodiscard]] bool section(std::string_view name, uint32_t knownVersion);

    /// The version of the section `section()` last opened. Lower than or equal to what was
    /// asked for, so a caller reads the old shape when it sees the old number.
    [[nodiscard]] uint32_t sectionVersion() const { return currentVersion; }

    [[nodiscard]] uint32_t u32();
    [[nodiscard]] uint64_t u64();
    [[nodiscard]] int32_t i32();
    [[nodiscard]] float f32();
    [[nodiscard]] bool boolean();
    [[nodiscard]] glm::vec3 vec3();
    [[nodiscard]] glm::vec4 vec4();
    [[nodiscard]] glm::quat quat();
    [[nodiscard]] glm::mat4 mat4();
    [[nodiscard]] std::string text();
    /// Copies `bytes` out. Fails, like every other read, rather than truncating.
    void blob(void* out, size_t bytes);

    /// False once any read has run off the end of its section. Sticky.
    [[nodiscard]] bool ok() const { return good; }
    /// Bytes left in the current section. Zero outside one.
    [[nodiscard]] size_t remaining() const { return good ? static_cast<size_t>(end - cursor) : 0; }
    /// Why the last failure happened, in a sentence fit for a log line. Empty when nothing
    /// has failed.
    [[nodiscard]] const std::string& reason() const { return failure; }

    /// Every section in the file, in write order, whether or not this build knows them.
    /// For a diagnostic that says what a save contains rather than what it could apply.
    struct Section {
        std::string name;
        uint32_t version = 0;
        uint64_t length = 0;
    };
    [[nodiscard]] const std::vector<Section>& sections() const { return toc; }

  private:
    void fail(std::string message);
    bool take(void* out, size_t bytes);

    std::vector<uint8_t> data;
    std::vector<Section> toc;
    std::vector<uint64_t> offsets;
    const uint8_t* cursor = nullptr;
    const uint8_t* end = nullptr;
    uint32_t currentVersion = 0;
    bool good = true;
    std::string failure;
};

} // namespace core
