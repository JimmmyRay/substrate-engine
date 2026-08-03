#include "core/Paths.h"

#ifdef _WIN32
#include <windows.h>
#include <string>
#endif

namespace core {

namespace {

std::filesystem::path g_argv0;

/// The platform's own answer, empty when it has none. Separate from the caching in
/// executableDir() so the fallback chain below reads as the list of attempts it is.
std::filesystem::path queryExecutablePath() {
    std::error_code ec;

#ifdef _WIN32
    // Wide, and deliberately: the narrow GetModuleFileNameA transcodes through the active
    // code page, so a user whose profile directory is not representable in it gets a path
    // that does not open. fs::path's native encoding on Windows is wchar_t anyway, so the
    // wide answer needs no conversion at all.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (written == 0) return {};
        if (written < buf.size()) {
            buf.resize(written);
            return std::filesystem::path(buf);
        }
        // Truncated. Long paths exceed MAX_PATH, and the API reports that by filling the
        // buffer exactly rather than by failing.
        if (buf.size() > 64 * 1024) return {};
        buf.resize(buf.size() * 2);
    }
#else
    // Not argv[0]: that is whatever the caller passed to exec, which need not be a path at
    // all. /proc/self/exe is the kernel's own answer and survives a rename, a relative
    // launch and a PATH lookup.
    if (std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) return exe;
    return {};
#endif
}

std::filesystem::path resolveExecutableDir() {
    if (const std::filesystem::path exe = queryExecutablePath(); !exe.empty()) return exe.parent_path();

    // /proc is not mounted in every container, and an AppImage runs from a FUSE mount
    // whose layout is its own business. argv[0] is the next best evidence.
    std::error_code ec;
    if (!g_argv0.empty()) {
        if (std::filesystem::path exe = std::filesystem::weakly_canonical(g_argv0, ec); !ec && exe.has_parent_path()) {
            return exe.parent_path();
        }
    }

    // Neither worked. The working directory is wrong often enough to be worth a thought
    // and right often enough to beat an empty path, which would silently reroot every
    // lookup at the filesystem root.
    if (std::filesystem::path cwd = std::filesystem::current_path(ec); !ec) return cwd;
    return std::filesystem::path(".");
}

}  // namespace

void seedExecutablePath(const char* argv0) {
    if (argv0 != nullptr && *argv0 != '\0') g_argv0 = argv0;
}

const std::filesystem::path& executableDir() {
    // Function-local static: resolved on the first call, and the answer cannot change
    // while the process runs. Thread-safe initialisation is the language's problem here,
    // which is the reason to prefer it to a namespace-scope cache and a flag.
    static const std::filesystem::path dir = resolveExecutableDir();
    return dir;
}

} // namespace core
