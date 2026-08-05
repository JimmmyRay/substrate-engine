#include "core/FileWrite.h"

#include <fstream>

namespace core {

bool writeFileAtomically(const std::filesystem::path& path, const void* bytes, size_t count, LogCategory category,
                         const char* what) {
    const std::filesystem::path temp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            Logger::warn(category, "%s: %s could not be opened for writing", what, temp.string().c_str());
            return false;
        }
        out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(count));
        if (!out) {
            Logger::warn(category, "%s: writing %s failed", what, temp.string().c_str());
            // Removed, never left: a later run has no way to tell a half-written temp from
            // a good one.
            out.close();
            std::error_code rm;
            std::filesystem::remove(temp, rm);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        Logger::warn(category, "%s: %s could not be renamed into place", what, path.string().c_str());
        return false;
    }
    return true;
}

} // namespace core
