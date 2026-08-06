#include "BuildLock.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace tool {
namespace {

namespace fs = std::filesystem;

/// The environment variable carrying the held directory to child processes. `substrate run`
/// holds the lock across the build *and* the check that the binary exists, which is the whole
/// window, and the build underneath it must not queue behind its own caller.
constexpr const char* kEnvVar = "SUBSTRATE_BUILD_LOCK";

constexpr int kGiveUpSeconds = 900;

void exportHeld(const std::string& dir) {
#ifdef _WIN32
    SetEnvironmentVariableA(kEnvVar, dir.c_str());
#else
    setenv(kEnvVar, dir.c_str(), 1);
#endif
}

} // namespace

bool BuildLock::acquire(const fs::path& buildDir) {
    const std::string dir = buildDir.generic_string();

    const char* already = std::getenv(kEnvVar);
    if (already && dir == already) return true;

    std::error_code ec;
    fs::create_directories(buildDir, ec);
    const fs::path lockFile = buildDir / ".build.lock";

    bool waited = false;
    const auto start = std::chrono::steady_clock::now();

#ifdef _WIN32
    for (;;) {
        HANDLE handle = CreateFileA(lockFile.string().c_str(), GENERIC_WRITE,
                                    0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            handle_ = handle;
            break;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION) return true;
        if (!waited) {
            std::fprintf(stderr, "==> waiting for another build in %s/\n", dir.c_str());
            waited = true;
        }
        if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(kGiveUpSeconds)) {
            std::fprintf(stderr, "error: gave up waiting for the build lock on %s/ after 15 "
                                 "minutes.\n", dir.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
#else
    fd_ = ::open(lockFile.c_str(), O_WRONLY | O_CREAT, 0666);
    // A lock we cannot create is not a lock we should block on: a read-only tree still
    // builds, and refusing here would be a new failure rather than a preserved one.
    if (fd_ < 0) return true;

    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "==> waiting for another build in %s/\n", dir.c_str());
        waited = true;
        for (;;) {
            if (flock(fd_, LOCK_EX | LOCK_NB) == 0) break;
            if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(kGiveUpSeconds)) {
                std::fprintf(stderr, "error: gave up waiting for the build lock on %s/ after 15 "
                                     "minutes.\n", dir.c_str());
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
#endif

    (void)waited;
    held_ = dir;
    exportHeld(dir);
    return true;
}

void BuildLock::release() {
    if (held_.empty()) return;
#ifdef _WIN32
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
#else
    if (fd_ >= 0) {
        flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    fd_ = -1;
#endif
    held_.clear();
#ifdef _WIN32
    SetEnvironmentVariableA(kEnvVar, nullptr);
#else
    unsetenv(kEnvVar);
#endif
}

BuildLock::~BuildLock() { release(); }

} // namespace tool
