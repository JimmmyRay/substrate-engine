# Cross-compile Windows x86_64 binaries with MinGW-w64, from Linux.
#
#   cmake -B build/win-release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
#
# `./build_release.sh <name> windows --docker` is what actually uses this; the container is
# where the cross-compiler lives. Nothing here is specific to the container, though, so a
# machine with the Debian mingw-w64 packages installed can use it directly.
#
# MSVC was the alternative and was rejected for one concrete reason: it cannot run in a
# Linux container, so choosing it would have meant no reproducible cross-build at all. The
# consolation is large -- MinGW *is* GCC, so the project's entire warning set
# (-Wall -Wextra -Wpedantic, and the -w on miniaudio) is valid verbatim, and there is no
# `if(MSVC)` branch anywhere in CMakeLists.txt. Do not add one.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# The `-posix` suffix is load-bearing and not a preference. Under MinGW-w64's `win32`
# thread model libstdc++ ships no <thread>, <mutex> or <condition_variable> at all -- the
# headers are simply absent -- and this engine uses all three in Logger, Profiler and the
# audio job thread. The failure is a wall of "no member named 'mutex' in namespace 'std'",
# which reads like a missing include and is not one.
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Programs come from the host, libraries and headers from the sysroot. glslangValidator,
# spirv-val and check_ascii.sh all run on the build machine during the build, and SPIR-V is
# target-independent -- a Windows game's shaders are correctly compiled by a Linux glslang,
# because the output is the same bytes either way.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The single most important line in this file.
#
# Without it, GCC's `printf` format archetype defaults to `ms_printf`, under which every
# `%zu` and `%llu` in the engine is an "unknown conversion character" error -- there are
# about twenty of the first and six of the second.
#
# Worse, and silently: `Logger::vformat` sizes its buffer with `vsnprintf(nullptr, 0, ...)`
# and returns an empty string when that reports a negative length. The MS CRT's version
# returns -1 for a null buffer rather than the length C99 requires, so without this define
# *every log line in the engine becomes an empty string* -- and it fails as a well-formed
# empty message, with nothing to grep for. This define routes printf and vsnprintf through
# mingw-w64's C99-conformant implementations and fixes both problems at once.
#
# tests/LoggerTests.cpp asserts a %zu message round-trips, which is what makes this
# checkable under wine rather than merely believed.
add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
