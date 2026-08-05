#include "core/SaveFile.h"

#include "core/FileWrite.h"
#include "core/Logger.h"

#include <cstring>
#include <fstream>

namespace core {

namespace {

/// Eight bytes, so a file that is not one of these fails on its first word. It must not
/// move with the format -- a magic that did would turn "a save this build cannot read" into
/// "not a save", and those want different messages.
constexpr char kMagic[8] = {'S', 'B', 'S', 'A', 'V', 'E', '\0', '\0'};

constexpr size_t kNameBytes = 16;

/// magic + framing version + section count.
constexpr size_t kHeaderBytes = sizeof(kMagic) + sizeof(uint32_t) * 2;
/// name + version + offset + length. Fixed width, so the table can be walked without
/// allocating and without trusting a length inside it.
constexpr size_t kEntryBytes = kNameBytes + sizeof(uint32_t) + sizeof(uint64_t) * 2;

template <typename T> void append(std::vector<uint8_t>& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

} // namespace


void SaveWriter::closeSection() {
    if (!open) return;
    toc.back().length = payload.size() - toc.back().offset;
    open = false;
}

void SaveWriter::beginSection(std::string_view name, uint32_t version) {
    closeSection();

    Entry e;
    // One byte reserved for the terminator, so a reader may treat the field as a C string.
    // Filling all 16 makes a 15-byte name run into the next field.
    if (name.size() >= kNameBytes) {
        Logger::warn(LogCategory::Core, "save section '%.*s' is longer than %zu bytes and was truncated",
                     static_cast<int>(name.size()), name.data(), kNameBytes - 1);
        name = name.substr(0, kNameBytes - 1);
    }
    std::memcpy(e.name, name.data(), name.size());
    e.version = version;
    e.offset = payload.size();
    toc.push_back(e);
    open = true;
}

void SaveWriter::u32(uint32_t value) { append(payload, value); }
void SaveWriter::u64(uint64_t value) { append(payload, value); }
void SaveWriter::i32(int32_t value) { append(payload, value); }
void SaveWriter::f32(float value) { append(payload, value); }
/// One byte, never `sizeof(bool)`: that is implementation-defined, and a save file written
/// by one compiler has to be readable by another.
void SaveWriter::boolean(bool value) { append(payload, static_cast<uint8_t>(value ? 1 : 0)); }
void SaveWriter::vec3(const glm::vec3& value) { append(payload, value); }
void SaveWriter::vec4(const glm::vec4& value) { append(payload, value); }
void SaveWriter::quat(const glm::quat& value) { append(payload, value); }
void SaveWriter::mat4(const glm::mat4& value) { append(payload, value); }

void SaveWriter::text(std::string_view value) {
    u64(value.size());
    blob(value.data(), value.size());
}

void SaveWriter::blob(const void* data, size_t bytes) {
    if (bytes == 0) return;
    const auto* p = static_cast<const uint8_t*>(data);
    payload.insert(payload.end(), p, p + bytes);
}

bool SaveWriter::write(const std::filesystem::path& path) {
    closeSection();

    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + toc.size() * kEntryBytes + payload.size());
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    append(out, kSaveFileVersion);
    append(out, static_cast<uint32_t>(toc.size()));

    // Offsets become absolute file positions only here: the table's own size depends on how
    // many sections there turned out to be, which is not known before this point.
    const uint64_t base = kHeaderBytes + toc.size() * kEntryBytes;
    for (const Entry& e : toc) {
        out.insert(out.end(), std::begin(e.name), std::end(e.name));
        append(out, e.version);
        append(out, base + e.offset);
        append(out, e.length);
    }
    out.insert(out.end(), payload.begin(), payload.end());

    // Written beside the save and renamed over it. Writing in place leaves a save that
    // failed half way through, and the player cannot get the old one back.
    return writeFileAtomically(path, out.data(), out.size(), LogCategory::Core, "save");
}


void SaveReader::fail(std::string message) {
    good = false;
    if (failure.empty()) failure = std::move(message);
    cursor = end;
}

bool SaveReader::open(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        failure = path.string() + ": no such save";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        failure = path.string() + ": could not be opened";
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(file.gcount()) != bytes.size()) {
        failure = path.string() + ": short read";
        return false;
    }
    return openBytes(std::move(bytes));
}

bool SaveReader::openBytes(std::vector<uint8_t> bytes) {
    data = std::move(bytes);
    toc.clear();
    offsets.clear();
    cursor = end = nullptr;
    currentVersion = 0;
    good = true;
    failure.clear();

    if (data.size() < kHeaderBytes || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        failure = "not a save file";
        return false;
    }

    uint32_t framing = 0;
    uint32_t count = 0;
    std::memcpy(&framing, data.data() + sizeof(kMagic), sizeof(framing));
    std::memcpy(&count, data.data() + sizeof(kMagic) + sizeof(framing), sizeof(count));

    if (framing != kSaveFileVersion) {
        failure = "save file format version " + std::to_string(framing) + ", but this build reads version " +
                  std::to_string(kSaveFileVersion);
        return false;
    }

    if (data.size() < kHeaderBytes + static_cast<size_t>(count) * kEntryBytes) {
        failure = "truncated: the table of contents does not fit";
        return false;
    }

    const uint8_t* p = data.data() + kHeaderBytes;
    for (uint32_t i = 0; i < count; ++i) {
        char name[kNameBytes]{};
        uint32_t version = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        std::memcpy(name, p, kNameBytes);
        std::memcpy(&version, p + kNameBytes, sizeof(version));
        std::memcpy(&offset, p + kNameBytes + sizeof(version), sizeof(offset));
        std::memcpy(&length, p + kNameBytes + sizeof(version) + sizeof(offset), sizeof(length));
        p += kEntryBytes;

        // Checked against the file's own length before anything trusts it: a table claiming
        // a section runs past the end is what a truncated or hand-edited save looks like.
        if (offset > data.size() || length > data.size() - offset) {
            failure = "truncated: section '" + std::string(name) + "' runs past the end of the file";
            return false;
        }
        // The writer reserves a byte for the terminator, but a file on disk is not bound by
        // that promise.
        name[kNameBytes - 1] = '\0';
        toc.push_back({std::string(name), version, length});
        offsets.push_back(offset);
    }
    return true;
}

bool SaveReader::section(std::string_view name, uint32_t knownVersion) {
    for (size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].name != name) continue;
        if (toc[i].version > knownVersion) {
            // Not `fail()`: nothing has been consumed, so the reader stays usable for other
            // sections and the caller decides whether this one is fatal.
            failure = "section '" + toc[i].name + "' is version " + std::to_string(toc[i].version) +
                      ", but this build reads up to version " + std::to_string(knownVersion);
            return false;
        }
        cursor = data.data() + offsets[i];
        end = cursor + toc[i].length;
        currentVersion = toc[i].version;
        good = true;
        return true;
    }
    failure = "no section '" + std::string(name) + "' in this save";
    return false;
}

bool SaveReader::take(void* out, size_t bytes) {
    if (!good) return false;
    if (cursor == nullptr || static_cast<size_t>(end - cursor) < bytes) {
        fail("read past the end of the section");
        return false;
    }
    std::memcpy(out, cursor, bytes);
    cursor += bytes;
    return true;
}

uint32_t SaveReader::u32() {
    uint32_t v = 0;
    (void)take(&v, sizeof(v));
    return v;
}
uint64_t SaveReader::u64() {
    uint64_t v = 0;
    (void)take(&v, sizeof(v));
    return v;
}
int32_t SaveReader::i32() {
    int32_t v = 0;
    (void)take(&v, sizeof(v));
    return v;
}
float SaveReader::f32() {
    float v = 0.0f;
    (void)take(&v, sizeof(v));
    return v;
}
bool SaveReader::boolean() {
    uint8_t v = 0;
    (void)take(&v, sizeof(v));
    return v != 0;
}
glm::vec3 SaveReader::vec3() {
    glm::vec3 v(0.0f);
    (void)take(&v, sizeof(v));
    return v;
}
glm::vec4 SaveReader::vec4() {
    glm::vec4 v(0.0f);
    (void)take(&v, sizeof(v));
    return v;
}
glm::quat SaveReader::quat() {
    glm::quat v(1.0f, 0.0f, 0.0f, 0.0f);
    (void)take(&v, sizeof(v));
    return v;
}
glm::mat4 SaveReader::mat4() {
    glm::mat4 v(1.0f);
    (void)take(&v, sizeof(v));
    return v;
}

std::string SaveReader::text() {
    const auto length = static_cast<size_t>(u64());
    if (!good) return {};
    // Checked against what is left *before* it is reserved: a corrupt length is how a
    // truncated file turns into a multi-gigabyte allocation.
    if (length > static_cast<size_t>(end - cursor)) {
        fail("a string in this save claims to be longer than the section holding it");
        return {};
    }
    std::string out(length, '\0');
    (void)take(out.data(), length);
    return out;
}

void SaveReader::blob(void* out, size_t bytes) { (void)take(out, bytes); }

} // namespace core
