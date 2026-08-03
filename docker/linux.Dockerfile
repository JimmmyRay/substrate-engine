# Build a Linux AppImage reproducibly.
#
#   docker build -f docker/linux.Dockerfile -t substrate-linux .
#
# Driven by `./build_release.sh <name> linux --docker`. Building natively works too and is
# faster to iterate on; this exists so the artifact does not inherit whatever glibc the
# developer happens to have.
#
# 22.04 is the point of the image. An AppImage bundles what it can but still links the host
# glibc, and glibc's symbol versioning is forward-compatible only -- a binary built against
# 2.39 will not start on a machine with 2.35, with an error naming GLIBC_2.39 and not the
# cause. Building against the older one is what makes the artifact run on both.
#
# Contrast docker/windows.Dockerfile, which is deliberately 24.04: nothing it produces links
# the container's glibc, so the constraint there runs the other way -- it needs a *newer*
# compiler, because 22.04's MinGW is GCC 10 and Jolt needs __builtin_bit_cast.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 ca-certificates curl file \
        libvulkan-dev libglfw3-dev glslang-tools spirv-tools \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libwayland-dev libxkbcommon-dev pkg-config \
        desktop-file-utils zsync \
    && rm -rf /var/lib/apt/lists/*

# libglfw3-dev is installed for its dependencies rather than for itself: GLFW is built from
# the submodule (jammy's 3.3.6 predates glfwInitVulkanLoader), but the apt package is what
# pulls in the X11 and Wayland development headers GLFW's own CMake looks for.

ARG APPIMAGETOOL_URL=https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
RUN curl -fsSL "${APPIMAGETOOL_URL}" -o /usr/local/bin/appimagetool && \
    chmod +x /usr/local/bin/appimagetool

# appimagetool is itself an AppImage and mounts itself with FUSE, which a container does not
# have unless it is run privileged. This tells every AppImage to unpack to a temp directory
# and run from there instead, which needs no kernel support and is why this image does not
# require --privileged or --device /dev/fuse.
ENV APPIMAGE_EXTRACT_AND_RUN=1

WORKDIR /src
