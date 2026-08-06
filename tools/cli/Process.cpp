#include "Process.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace tool {
namespace {

namespace fs = std::filesystem;

/// How long a child gets between "please stop" and "stop". Two seconds is the profiler's
/// trace flush plus the swapchain teardown it happens after; a shorter grace produces a
/// truncated trace, which reads as a fast frame rather than as a killed run.
constexpr int kGraceSeconds = 2;

#ifdef _WIN32

/// Quoting for CreateProcess, whose command line is one string that the CRT re-splits.
/// Backslashes are only special in front of a quote, which is why the run is counted
/// rather than each one escaped.
std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;

    std::string out = "\"";
    size_t slashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++slashes;
            out += c;
            continue;
        }
        if (c == '"') {
            out.append(slashes + 1, '\\');
            out += '"';
        } else {
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes, '\\');
    return out + "\"";
}

struct Pipe {
    HANDLE read = nullptr;
    HANDLE write = nullptr;

    bool open() {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&read, &write, &sa, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        return true;
    }
    void closeWrite() {
        if (write) CloseHandle(write);
        write = nullptr;
    }
    void closeRead() {
        if (read) CloseHandle(read);
        read = nullptr;
    }
};

void drain(HANDLE handle, std::string& into) {
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        into.append(buffer, got);
    }
}

#else

void drain(int fd, std::string& into) {
    char buffer[4096];
    ssize_t got = 0;
    while ((got = ::read(fd, buffer, sizeof(buffer))) > 0) {
        into.append(buffer, static_cast<size_t>(got));
    }
}

#endif

std::vector<std::string> pathExtensions() {
#ifdef _WIN32
    const char* raw = std::getenv("PATHEXT");
    std::vector<std::string> out{""};
    std::string value = raw ? raw : ".COM;.EXE;.BAT;.CMD";
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(';', start);
        std::string ext = value.substr(start, end == std::string::npos ? end : end - start);
        if (!ext.empty()) out.push_back(ext);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
#else
    return {""};
#endif
}

} // namespace

bool RunResult::saidAny(std::string_view needle) const {
    return out.find(needle) != std::string::npos || err.find(needle) != std::string::npos;
}

std::string stripAnsi(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\x1b' || i + 1 >= text.size() || text[i + 1] != '[') {
            out += text[i];
            continue;
        }
        size_t end = i + 2;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
                                     text[end] == ';')) {
            ++end;
        }
        // A sequence with no final byte is not an escape; keep it rather than eat the rest.
        if (end < text.size() && text[end] == 'm') {
            i = end;
        } else {
            out += text[i];
        }
    }
    return out;
}

fs::path which(const std::string& name) {
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return fs::exists(name) ? fs::path(name) : fs::path();
    }

    const char* raw = std::getenv("PATH");
    if (!raw) return {};

#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif

    const std::vector<std::string> extensions = pathExtensions();
    std::string path = raw;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(kSeparator, start);
        const std::string dir = path.substr(start, end == std::string::npos ? end : end - start);
        if (!dir.empty()) {
            for (const std::string& ext : extensions) {
                std::error_code ec;
                const fs::path candidate = fs::path(dir) / (name + ext);
                if (fs::is_regular_file(candidate, ec)) return candidate;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

RunResult run(const std::vector<std::string>& argv, const RunOptions& options) {
    RunResult result;
    if (argv.empty()) {
        result.exitCode = 2;
        return result;
    }

#ifdef _WIN32
    std::string commandLine;
    for (const std::string& arg : argv) {
        if (!commandLine.empty()) commandLine += ' ';
        commandLine += quoteArg(arg);
    }

    Pipe outPipe, errPipe;
    if (!options.inherit && (!outPipe.open() || !errPipe.open())) {
        result.exitCode = 2;
        return result;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (!options.inherit) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = outPipe.write;
        si.hStdError = errPipe.write;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    // A new process group is what makes GenerateConsoleCtrlEvent able to name this child
    // and nothing else. Without it the event reaches every process sharing the console,
    // including the one sending it.
    std::string block;
    for (const auto& [key, value] : options.env) block += key + "=" + value + '\0';
    if (!block.empty()) {
        LPCH existing = GetEnvironmentStringsA();
        for (LPCH cursor = existing; cursor && *cursor;) {
            const size_t len = std::strlen(cursor);
            block.append(cursor, len);
            block += '\0';
            cursor += len + 1;
        }
        if (existing) FreeEnvironmentStringsA(existing);
        block += '\0';
    }

    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessA(
        nullptr, commandLine.data(), nullptr, nullptr, options.inherit ? FALSE : TRUE,
        CREATE_NEW_PROCESS_GROUP, block.empty() ? nullptr : block.data(),
        options.cwd.empty() ? nullptr : options.cwd.string().c_str(), &si, &pi);

    outPipe.closeWrite();
    errPipe.closeWrite();

    if (!started) {
        outPipe.closeRead();
        errPipe.closeRead();
        result.exitCode = 127;
        result.err = "error: cannot run " + argv[0] + "\n";
        return result;
    }

    std::thread outReader, errReader;
    if (!options.inherit) {
        outReader = std::thread([&] { drain(outPipe.read, result.out); });
        errReader = std::thread([&] { drain(errPipe.read, result.err); });
    }

    const DWORD limit = options.timeoutSeconds > 0
                            ? static_cast<DWORD>(options.timeoutSeconds) * 1000
                            : INFINITE;
    if (WaitForSingleObject(pi.hProcess, limit) == WAIT_TIMEOUT) {
        result.timedOut = true;
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
        if (WaitForSingleObject(pi.hProcess, kGraceSeconds * 1000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
        }
    }

    if (outReader.joinable()) outReader.join();
    if (errReader.joinable()) errReader.join();
    outPipe.closeRead();
    errPipe.closeRead();

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exitCode = result.timedOut ? 124 : static_cast<int>(code);
    return result;

#else
    int outFds[2] = {-1, -1};
    int errFds[2] = {-1, -1};
    if (!options.inherit && (pipe(outFds) != 0 || pipe(errFds) != 0)) {
        result.exitCode = 2;
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!options.inherit) {
        posix_spawn_file_actions_addclose(&actions, outFds[0]);
        posix_spawn_file_actions_addclose(&actions, errFds[0]);
        posix_spawn_file_actions_adddup2(&actions, outFds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, errFds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, outFds[1]);
        posix_spawn_file_actions_addclose(&actions, errFds[1]);
    }
    if (!options.cwd.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
    }

    std::vector<std::string> owned;
    owned.reserve(options.env.size());
    std::vector<char*> envp;
    for (char** e = environ; *e; ++e) envp.push_back(*e);
    for (const auto& [key, value] : options.env) {
        owned.push_back(key + "=" + value);
        envp.push_back(owned.back().data());
    }
    envp.push_back(nullptr);

    std::vector<char*> args;
    std::vector<std::string> argsOwned = argv;
    for (std::string& arg : argsOwned) args.push_back(arg.data());
    args.push_back(nullptr);

    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, args.data(),
                                     envp.data());
    posix_spawn_file_actions_destroy(&actions);

    if (!options.inherit) {
        close(outFds[1]);
        close(errFds[1]);
    }

    if (spawned != 0) {
        if (!options.inherit) {
            close(outFds[0]);
            close(errFds[0]);
        }
        result.exitCode = 127;
        result.err = "error: cannot run " + argv[0] + ": " + std::strerror(spawned) + "\n";
        return result;
    }

    std::thread outReader, errReader;
    if (!options.inherit) {
        outReader = std::thread([&] { drain(outFds[0], result.out); });
        errReader = std::thread([&] { drain(errFds[0], result.err); });
    }

    // waitpid has no timed form, so the deadline is a poll. Ten milliseconds is below
    // anything a human notices and costs nothing against runs measured in seconds.
    int status = 0;
    auto reap = [&](int deadlineSeconds) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const pid_t done = waitpid(pid, &status, WNOHANG);
            if (done == pid) return true;
            if (done < 0) return true;
            if (deadlineSeconds > 0) {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= std::chrono::seconds(deadlineSeconds)) return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    if (!reap(options.timeoutSeconds)) {
        result.timedOut = true;
        kill(pid, SIGTERM);
        if (!reap(kGraceSeconds)) {
            kill(pid, SIGKILL);
            reap(0);
        }
    }

    if (outReader.joinable()) outReader.join();
    if (errReader.joinable()) errReader.join();
    if (!options.inherit) {
        close(outFds[0]);
        close(errFds[0]);
    }

    if (result.timedOut) {
        // 124 is what coreutils `timeout` returns, and harnesses ported from the shell
        // still test for it by value.
        result.exitCode = 124;
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }
    return result;
#endif
}

void execOrExit(const std::vector<std::string>& argv, const RunOptions& options) {
    if (argv.empty()) std::exit(2);

#ifndef _WIN32
    if (!options.cwd.empty()) {
        if (chdir(options.cwd.c_str()) != 0) std::exit(1);
    }
    for (const auto& [key, value] : options.env) setenv(key.c_str(), value.c_str(), 1);

    std::vector<std::string> owned = argv;
    std::vector<char*> args;
    for (std::string& arg : owned) args.push_back(arg.data());
    args.push_back(nullptr);
    execvp(argv[0].c_str(), args.data());
    // Only reached when the exec failed.
    std::fprintf(stderr, "error: cannot run %s: %s\n", argv[0].c_str(), std::strerror(errno));
    std::exit(127);
#else
    RunOptions passthrough = options;
    passthrough.inherit = true;
    std::exit(run(argv, passthrough).exitCode);
#endif
}

} // namespace tool
