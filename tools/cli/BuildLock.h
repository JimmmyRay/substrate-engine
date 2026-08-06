#pragma once

#include <filesystem>
#include <string>

/**
 * @file tools/cli/BuildLock.h
 * @brief One build at a time per build directory.
 *
 * Ninja takes no lock of its own and two sessions in one checkout is the normal state of
 * this project, so two builds of the same directory write the same object files and link the
 * same executable at once. The half that is not obvious is what it does to a *finished*
 * build: the linker unlinks its output before writing it, so while one session links `demo`,
 * the file does not exist for anybody -- which is how a golden case failed with "still does
 * not exist after building" one statement after its own build returned zero.
 */
namespace tool {

/// Held for the life of the object. Re-entrant through the environment, so a command that
/// takes the lock and then runs another that would take the same one does not deadlock.
class BuildLock {
public:
    BuildLock() = default;
    ~BuildLock();

    BuildLock(const BuildLock&) = delete;
    BuildLock& operator=(const BuildLock&) = delete;

    /// Blocks until the lock is free, printing once if that wait is not instant. Returns
    /// false only after giving up, which is 15 minutes.
    bool acquire(const std::filesystem::path& buildDir);

    /// Drop it early. A holder that goes on to run a 120-second capture must, or every
    /// other build in the tree queues behind that capture.
    void release();

private:
    std::string held_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace tool
