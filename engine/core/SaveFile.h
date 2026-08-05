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
 * @brief A versioned byte stream, in sections.
 *
 * A file is a table of contents followed by named sections, each with its own version and
 * byte length, because the engine and the game version independently. A version this build
 * does not know is refused before a byte of that section is consumed; a *section* it does
 * not know is skipped rather than fatal, which is what lets a save written by a later build
 * still load its engine half into an earlier one.
 *
 * The reader hands out data and cannot un-apply what a caller did with it, so atomicity is
 * the caller's: read everything, check it against the world, and only then write anything.
 * `section()` returning false before consuming any data is the tool for that.
 */
namespace core {

/// Bumped when the *framing* changes -- the header, the table of contents, the primitive
/// encodings. Not for a section's contents; that is what a section version is. A framing
/// change without a bump here makes an old save decode as garbage rather than be refused.
inline constexpr uint32_t kSaveFileVersion = 1;

/// @brief Append-only. Builds the whole file in memory and writes it once, so a file either
/// exists complete or was never renamed into place.
class SaveWriter {
  public:
    /**
     * @brief Open a section. Closes the previous one.
     *
     * @param name    Up to 15 bytes -- the table of contents is fixed-width so a reader can
     *                walk it without allocating. Longer is truncated, with a warning.
     * @param version The caller's own, and what a future reader compares against.
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

    /// Write to `path`, through a temporary and a rename. Writing in place instead leaves a
    /// truncated save where the previous one was when it is interrupted.
    [[nodiscard]] bool write(const std::filesystem::path& path);

    /// The bytes as they stand, for a caller putting the file somewhere this class does not
    /// know how to reach.
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

/// @brief Bounds-checked, forward-only within a section.
///
/// A failed read sets `ok()` false permanently. Clearing it would let a corrupt save become
/// a world that is subtly wrong instead of one that refused to load.
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
    /// The same, over bytes already in hand.
    [[nodiscard]] bool openBytes(std::vector<uint8_t> data);

    /**
     * @brief Seek to a section, if this build can read it.
     *
     * @param knownVersion the highest version of this section the caller understands.
     * @return false when the section is absent, or its version is higher than
     *         `knownVersion`. Nothing is consumed either way, which is what lets a caller
     *         decide whether a section it cannot read is fatal.
     */
    [[nodiscard]] bool section(std::string_view name, uint32_t knownVersion);

    /// The version of the section `section()` last opened -- at most what was asked for, so
    /// a caller seeing an old number must read the old shape.
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
