# The two programs that must exist before the engine can be built: `substrate-guard`, which
# every engine target depends on, and `substrate`, which is what drives the build at all.
#
# Configurable standalone, and that is the point. Neither links the engine, a submodule or
# Vulkan, so bootstrapping them needs none of the root project's configure -- no simdjson
# download, no Jolt, no GLFW. `scripts/build.sh` and `scripts/build.cmd` point cmake straight
# at this directory, which is why the first command on a fresh clone takes seconds rather
# than a minute.
#
# Added by the root CMakeLists.txt as a subdirectory too, so there is one definition of each
# target rather than a copy here that drifts.
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SubstrateTools VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    find_package(Threads REQUIRED)

    # The root project holds the real list; standalone gets the part that needs no
    # generator expressions to be correct.
    set(SUBSTRATE_WARNINGS -Wall -Wextra -Wpedantic)
    if(WIN32)
        list(APPEND SUBSTRATE_WARNINGS -Wno-cast-function-type)
    endif()
endif()

set(SUBSTRATE_TOOLS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Neither tool is what a sanitizer configuration exists to test, and instrumenting them
# breaks the build rather than checking anything: the asan and tsan configurations put
# -fsanitize into CMAKE_CXX_FLAGS, which reaches every target, and a ThreadSanitizer binary
# aborts with "unexpected memory mapping" unless it is launched through `setarch -R`. The
# layer guard is launched by ninja, which does no such thing -- so `scripts/test.sh tsan`
# failed in the guard before it compiled a line of the engine.
set(SUBSTRATE_TOOLS_NO_SANITIZE -fno-sanitize=all)

# Declared here only when this is the top-level project; the root CMakeLists.txt has already
# made it otherwise, and two INTERFACE libraries of one name is a configure error.
if(NOT TARGET rapidjson)
    add_library(rapidjson INTERFACE)
    target_include_directories(rapidjson SYSTEM INTERFACE
        ${SUBSTRATE_TOOLS_ROOT}/external/rapidjson/include)
endif()

# ------------------------------------------------------------------ the guard
# Linking nothing but the standard library is the property that matters: a guard built out
# of the tree it guards cannot run before that tree builds.
add_executable(substrate-guard guard/main.cpp)
target_compile_options(substrate-guard PRIVATE ${SUBSTRATE_WARNINGS} ${SUBSTRATE_TOOLS_NO_SANITIZE})
target_link_options(substrate-guard PRIVATE ${SUBSTRATE_TOOLS_NO_SANITIZE})

# -------------------------------------------------------------------- the CLI
# Everything the shell scripts used to be.
#
# SUBSTRATE_SOURCE_DIR is a fallback for finding the checkout, used only when the working
# directory is not inside one -- a binary copied elsewhere then still resolves the tree it
# was configured from rather than failing with "no Substrate checkout here".
add_executable(substrate-cli
    cli/main.cpp
    cli/Bench.cpp
    cli/Arena.cpp
    cli/Build.cpp
    cli/Golden.cpp
    cli/BuildLock.cpp
    cli/HarnessLog.cpp
    cli/Locomotion.cpp
    cli/Manifest.cpp
    cli/Perfgate.cpp
    cli/Process.cpp
    cli/Rdoc.cpp
    cli/Readback.cpp
    cli/Repo.cpp
    cli/Run.cpp
    cli/Setup.cpp
    cli/Test.cpp
    cli/Tools.cpp
    cli/Trace.cpp
)
set_target_properties(substrate-cli PROPERTIES OUTPUT_NAME substrate)
target_compile_options(substrate-cli PRIVATE ${SUBSTRATE_WARNINGS} ${SUBSTRATE_TOOLS_NO_SANITIZE})
target_link_options(substrate-cli PRIVATE ${SUBSTRATE_TOOLS_NO_SANITIZE})
target_compile_definitions(substrate-cli PRIVATE
    SUBSTRATE_SOURCE_DIR="${SUBSTRATE_TOOLS_ROOT}")
target_link_libraries(substrate-cli PRIVATE Threads::Threads rapidjson)
