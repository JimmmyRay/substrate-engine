# Cross-compile a Windows game from Linux, reproducibly.
#
#   docker build -f docker/windows.Dockerfile -t substrate-windows .
#
# Driven by `./build_release.sh <name> windows --docker`, which is the supported entry
# point. Building this by hand is only for debugging the image itself.
#
# 24.04, and deliberately newer than the Linux image, which pins 22.04 so the AppImage does
# not demand a glibc newer than the machine running it. That reasoning does not apply here:
# nothing produced by this image links the container's glibc. The output is a Windows .exe
# built against the mingw-w64 runtime, and the only host tool whose output ships is
# glslangValidator, which emits target-independent SPIR-V.
#
# The version is forced from the other side instead. 22.04's mingw-w64 is GCC 10, which has
# no `__builtin_bit_cast` -- Jolt uses it in Math.h and DVec3.h, and the build dies with
# "'__builtin_bit_cast' was not declared in this scope" partway through the physics library.
# 24.04 ships GCC 13. Anything from GCC 11 up would do.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Two groups, and the split is worth reading:
#   1. the host toolchain, which builds nothing that ships but runs during the build
#   2. the cross toolchain, `-posix` variants only (see cmake/toolchains/ for why)
#
# Nothing here runs the output. The image cross-compiles and packages; what it produces is
# checked statically, by the import table, and not by execution. See
# docs/architecture/limitations.md for what that leaves unverified.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 ca-certificates curl \
        glslang-tools spirv-tools \
        g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix \
        binutils-mingw-w64-x86-64 mingw-w64-tools \
        nsis \
    && rm -rf /var/lib/apt/lists/*

# Debian ships both thread models and leaves the alternative pointing at win32, under which
# libstdc++ has no <thread>, <mutex> or <condition_variable> at all. Selecting posix here
# means the toolchain file's `-posix` suffix and the bare driver name agree, so a build that
# somehow bypasses the toolchain file still gets a working standard library instead of a
# wall of errors that read like a missing include.
RUN update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix && \
    update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

# Vulkan headers into the cross sysroot. Headers are all that is needed and all that is
# wanted: volk loads vulkan-1.dll at runtime and nothing links the loader, so there is no
# import library to install and no version of one to get wrong.
ARG VULKAN_HEADERS_TAG=v1.3.280
RUN git clone --depth 1 --branch ${VULKAN_HEADERS_TAG} \
        https://github.com/KhronosGroup/Vulkan-Headers.git /tmp/vulkan-headers && \
    cp -r /tmp/vulkan-headers/include/vulkan /usr/x86_64-w64-mingw32/include/ && \
    cp -r /tmp/vulkan-headers/include/vk_video /usr/x86_64-w64-mingw32/include/ && \
    rm -rf /tmp/vulkan-headers

WORKDIR /src
