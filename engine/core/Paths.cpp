#include "core/Paths.h"

#ifdef _WIN32
#include <windows.h>
#include <string>
#endif

namespace core {

namespace {

std::filesystem::path g_argv0;

/// The platform's own answer, empty when it has none.
std::filesystem::path queryExecutablePath() {
    std::error_code ec;

#ifdef _WIN32
    // Wide, never the narrow GetModuleFileNameA: that transcodes through the active code
    // page, so a profile directory not representable in it yields a path that will not
    // open. `fs::path`'s native encoding on Windows is `wchar_t` anyway.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (written == 0) return {};
        if (written < buf.size()) {
            buf.resize(written);
            return std::filesystem::path(buf);
        }
        // Truncated: the API reports a path past MAX_PATH by filling the buffer exactly
        // rather than by failing.
        if (buf.size() > 64 * 1024) return {};
        buf.resize(buf.size() * 2);
    }
#else
    // /proc/self/exe before argv[0]: argv[0] is whatever the caller passed to exec and need
    // not be a path at all, where the kernel's answer survives a rename, a relative launch
    // and a PATH lookup.
    if (std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) return exe;
    return {};
#endif
}

std::filesystem::path resolveExecutableDir() {
    if (const std::filesystem::path exe = queryExecutablePath(); !exe.empty()) return exe.parent_path();

    // /proc is not mounted in every container, and an AppImage runs from a FUSE mount whose
    // layout is its own business.
    std::error_code ec;
    if (!g_argv0.empty()) {
        if (std::filesystem::path exe = std::filesystem::weakly_canonical(g_argv0, ec); !ec && exe.has_parent_path()) {
            return exe.parent_path();
        }
    }

    // Last resort. An empty path here would silently reroot every lookup at the filesystem
    // root.
    if (std::filesystem::path cwd = std::filesystem::current_path(ec); !ec) return cwd;
    return std::filesystem::path(".");
}

}  // namespace

void seedExecutablePath(const char* argv0) {
    if (argv0 != nullptr && *argv0 != '\0') g_argv0 = argv0;
}

const std::filesystem::path& executableDir() {
    // Function-local static, so initialisation is thread-safe by the language rather than
    // by a namespace-scope cache and a flag.
    static const std::filesystem::path dir = resolveExecutableDir();
    return dir;
}

} // namespace core
