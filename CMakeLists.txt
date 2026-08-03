cmake_minimum_required(VERSION 3.20)
project(Substrate VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# --------------------------------------------------------------- section GC
# One function per section, one variable per section, and a linker told to drop the ones
# nothing reaches. Worth 21% of the stripped binary, and most of that is inside the
# dependencies -- a linker pulls a whole archive member to resolve one undefined symbol,
# so every game carries all of Jolt's solvers, all of miniaudio's decoders and all of
# fastgltf's extensions whether or not it reaches them.
#
# Directory scope, and above every add_subdirectory below, because that is what makes the
# dependencies compile with it too. Put on `substrate` alone it would collect nothing that
# matters: the engine's own code is the part a game does reach.
#
# In all four configurations, deliberately:
#
#   Debug     -ffunction-sections changes where a function is emitted, not what debug info
#             describes it -- a backtrace through live code is unchanged, and the sections
#             that carry unwind data (.eh_frame) and static construction (.init_array) are
#             KEEP-ed by the default linker script rather than left to reachability. A
#             configuration built differently from the one the golden set runs is worse
#             than a slightly smaller Debug binary is good.
#   asan/tsan Both sanitizers place their own metadata in sections the linker already knows
#             to keep, and their runtimes are linked in whole. Verified rather than assumed:
#             ./test.sh asan and ./test.sh tsan pass with these on.
#
# Not -flto, and not by oversight. INTERPROCEDURAL_OPTIMIZATION is forced OFF below with
# its own argument; section GC is a link-time deletion of what is already compiled, which
# is a different decision with a different cost, and bundling them would make one
# measurement answer for two.
#
# Must precede the targets and the add_subdirectory calls: add_compile_options and
# add_link_options only affect what is created after them.
add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)

# ----------------------------------------------------------------------- game
# Which game a build directory holds is a property of the build directory:
# `build_game.sh <name>` sets this, `build.sh` clears it, and the engine's object files
# survive the toggle -- so alternating costs a reconfigure rather than a rebuild. It is
# also what keeps every script's signature unchanged: `run.sh` reads the configured name
# instead of taking a --game flag.
#
# Declared up here rather than beside add_subdirectory(game/...) because the shader rules
# below need it: a game brings its own GLSL, and that tree has to be globbed before any
# target exists to depend on it.
set(SUBSTRATE_GAME "" CACHE STRING "Name of a directory under game/ to build, or empty for none")

# ---------------------------------------------------------------- system deps
#
# Headers only when cross-compiling, and this is not a concession -- it is what the project
# already needed. FindVulkan insists on the loader's import library, which no MinGW sysroot
# has, and would fail configure on this line before anything else was attempted. But nothing
# here links the loader: volk resolves every entry point out of vulkan-1.dll at runtime, and
# `Vulkan_INCLUDE_DIRS` is read exactly once below, in a status message.
#
# Keeping the loader unlinked is also what makes a packaged .exe start on a machine with no
# Vulkan at all and print its own error, rather than dying in the Windows loader with a
# missing-DLL dialog before main().
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CROSSCOMPILING)
    find_path(Vulkan_INCLUDE_DIRS vulkan/vulkan.h REQUIRED)
    # volk does its own find_package(Vulkan QUIET) otherwise, and hits the same wall.
    set(VULKAN_HEADERS_INSTALL_DIR "${Vulkan_INCLUDE_DIRS}/.." CACHE PATH "" FORCE)
else()
    find_package(Vulkan REQUIRED)
endif()
find_package(Threads REQUIRED)

# ------------------------------------------------------------ submodule guard
foreach(dep glm volk VulkanMemoryAllocator fastgltf stb glfw rapidjson googletest JoltPhysics miniaudio
        meshoptimizer)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/${dep}/README.md
       AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/${dep}/readme.md)
        message(FATAL_ERROR
            "external/${dep} is empty. Run: git submodule update --init --recursive")
    endif()
endforeach()

# ------------------------------------------------------------------ externals
# GLFW is submoduled rather than taken from the system: jammy ships 3.3.6, which
# predates glfwInitVulkanLoader() and so cannot share volk's loader.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw EXCLUDE_FROM_ALL)

# volk: Vulkan meta-loader. Removes the loader trampoline from every vk* call.
set(VOLK_PULL_IN_VULKAN ON  CACHE BOOL "" FORCE)
set(VOLK_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(external/volk EXCLUDE_FROM_ALL)

set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glm EXCLUDE_FROM_ALL)

set(VMA_BUILD_SAMPLES      OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
add_subdirectory(external/VulkanMemoryAllocator EXCLUDE_FROM_ALL)

# fastgltf downloads the simdjson amalgamation at configure time (cached in build/).
set(FASTGLTF_COMPILE_AS_CPP20 ON  CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_TESTS     OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS      OFF CACHE BOOL "" FORCE)
add_subdirectory(external/fastgltf EXCLUDE_FROM_ALL)

# rapidjson is header-only; its own CMake build is test/example scaffolding we skip.
add_library(rapidjson INTERFACE)
target_include_directories(rapidjson SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/rapidjson/include)

# stb ships no build system.
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/stb)

# googletest, for the unit suite (5.1). A test framework is a solved problem, which
# simplicity rule 6 explicitly permits taking a dependency on -- and it is the one the
# tests this suite is descended from were written against. gmock is not built: nothing
# here mocks anything, because nothing here has an interface to mock.
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(external/googletest EXCLUDE_FROM_ALL)

# Jolt, for S4. A rigid-body solver is a solved problem, which simplicity rule 6
# explicitly permits taking a dependency on -- and it is the one Tethered used.
#
# Only its library builds: Jolt adds its samples, viewer, unit tests and performance test
# solely when it is the top-level project, so there is nothing to switch off for those.
# What does need switching off is below, and the first one is not optional.
#
# OVERRIDE_CXX_FLAGS defaults to ON and rewrites CMAKE_CXX_FLAGS_DEBUG and
# CMAKE_CXX_FLAGS_RELEASE for the *whole* project rather than for Jolt. Left alone it
# silently changes how Substrate itself is compiled, and it would fight build.sh's
# sanitizer configurations, which pass their flags the same way.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
# Jolt's own build treats warnings as errors, which makes a compiler upgrade break the
# dependency rather than break our code.
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
# LTO across a dependency this size costs link time on every build and interacts badly
# with the sanitizer builds.
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
# Jolt otherwise unmasks floating-point exceptions around its own update. This process
# contains a graphics driver, and a trap raised inside one is neither our bug nor one we
# can do anything about.
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
# Serialisation and an install target for a physics world nothing serialises or installs.
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL       OFF CACHE BOOL "" FORCE)
# Jolt defaults to -fno-rtti and Substrate does not, which is a disagreement with exactly
# one symptom and it is a link error rather than a compile one: the engine's single
# derived type (S4.5's debug renderer) emits a typeinfo that references its base's, and
# the base's was never emitted. Making the two agree is the fix; compiling one
# translation unit with -fno-rtti instead would work here and break the next time
# something in this file derives from something in that one.
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
# Deliberately off. CROSS_PLATFORM_DETERMINISTIC buys the same results across platforms
# and compilers at a cost Jolt's own documentation states. What 5.3 needs is that the
# same binary produces the same frame on the same machine, which Jolt gives
# unconditionally for a fixed step, a fixed thread count and a fixed call order.
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
# The debug renderer is what S4.5 draws through, and it is wanted in Release too: a
# collision shape you can only see in a Debug build is one you cannot check against a
# frame time.
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ON CACHE BOOL "" FORCE)
# The option next to it, and the opposite answer -- stated because the pair defaults ON
# and inheriting one of them is not a decision. Jolt's profiler is 193 scoped measurements
# through the solver and its jobs, and nothing in this tree ever calls JPH_PROFILE_START,
# so `ProfileThread::sGetInstance()` is null at every one of them: each site costs a
# thread-local load and a branch around a 32-byte stack sample that is never written, and
# there is no way to read what it would collect. The engine has its own profiler and its
# own trace, and `simulate` is the zone physics is read from. Both switches are named
# rather than the one that was on, because the second would silently put it back.
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DISTRIBUTION      OFF CACHE BOOL "" FORCE)
# Jolt 5.6 ships GPU backends for its hair solver, and their defaults are all ON: with
# them left alone the build compiles a dozen HLSL shaders through a `glslc` this project
# does not use and does not require, and fails before a single source file is touched.
# Turning them off is not only a build fix. JPH_USE_VK would have Jolt open its own
# Vulkan device beside `VulkanContext`, and one process holding two is a memory budget
# nobody is accounting for.
set(JPH_USE_DX12        OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK          OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL         OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
add_subdirectory(external/JoltPhysics/Build EXCLUDE_FROM_ALL)

# miniaudio, for S5. A mixer is a solved problem, which simplicity rule 6 explicitly
# permits taking a dependency on -- and it is the one Tethered used.
#
# Its own CMakeLists is not used. Upstream builds a shared library plus its examples and
# tests, and what this engine wants is the header compiled into one translation unit of
# its own (engine/scene/miniaudio_impl.cpp), which is how the same file already handles stb
# and VMA. So this is an INTERFACE target carrying an include directory and nothing else.
#
# No dev packages are needed for ALSA, PulseAudio or JACK: miniaudio dlopen()s whichever
# of them is present at runtime, which is why the only link-time additions are libm and
# libdl. A machine with none of them opens no device, and AudioEngine::init falls back to
# mixing without one rather than failing.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio)

# meshoptimizer, for C17's LOD chains. A mesh simplifier is a solved problem, which the
# conventions explicitly permit taking a dependency on -- and quadric edge collapse written
# here would be a few hundred lines of numerics with no golden set of its own, competing
# with the reference implementation of the technique.
#
# Only the library builds. The demo and gltfpack are its own tooling, and its install rules
# would put a static library into a package that has no use for one.
set(MESHOPT_BUILD_DEMO      OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK  OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL         OFF CACHE BOOL "" FORCE)
add_subdirectory(external/meshoptimizer EXCLUDE_FROM_ALL)

# RenderDoc's in-application API. A single MIT header, vendored rather than submoduled
# because that is how upstream ships it -- it is meant to be copied into a project, and
# a submodule of the whole RenderDoc tree to get one header would be absurd. Vendored
# rather than included from the local install so the build does not depend on RenderDoc
# being present: the header only declares function pointer types, and
# engine/gfx/RenderDoc.cpp resolves them at runtime or does nothing.
add_library(renderdoc_app INTERFACE)
target_include_directories(renderdoc_app SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/renderdoc)

# Treat every dependency's headers as system headers, so their warnings (fastgltf's
# MSVC #pragma region, VMA's unused params) never appear in our build output.
foreach(dep glfw volk volk_headers glm VulkanMemoryAllocator fastgltf gtest Jolt meshoptimizer)
    if(TARGET ${dep})
        get_target_property(_dep_includes ${dep} INTERFACE_INCLUDE_DIRECTORIES)
        if(_dep_includes)
            set_target_properties(${dep} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dep_includes}")
        endif()
    endif()
endforeach()

# -------------------------------------------------------------------- shaders
# Two source trees, compiled by one rule and kept apart on the way out.
#
#   engine/shaders/            -> build/<cfg>/shaders/
#   game/<name>/shaders/       -> build/<cfg>/shaders/game/
#
# Separate output directories rather than one, because a compiled name is unqualified
# (`gbuffer.frag.spv`): a game shader sharing an engine shader's name would otherwise
# overwrite it, and overriding one is exactly what the game tree is for. readShaderBinary
# searches the game's directory first and falls through, so the override is a lookup order
# rather than a clobber.
#
# The game's output directory is `shaders/game` and not `shaders/<name>` on purpose. One
# build directory holds one game, so a fixed name keeps SUBSTRATE_GAME_SHADER_DIR the same
# string for every game -- and a define that never varies is a define whose engine object
# files survive `build_game.sh demo` following `build_game.sh other`.
find_program(GLSLANG_VALIDATOR NAMES glslangValidator REQUIRED)
find_program(SPIRV_VAL         NAMES spirv-val)

set(SHADER_OUT_DIR      ${CMAKE_BINARY_DIR}/shaders)
set(GAME_SHADER_OUT_DIR ${SHADER_OUT_DIR}/game)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

# Emptied on every configure, and only this one. A compiled shader outlives its source --
# nothing removes an output whose add_custom_command stopped being generated -- and in the
# game's tree the mere presence of a file decides which of two shaders runs. So deleting
# game/<name>/shaders/tonemap.frag would otherwise leave tonemap.frag.spv overriding the
# engine's forever, with no source anywhere in the tree to explain the picture. The glob
# below is CONFIGURE_DEPENDS, so adding or removing a shader forces the reconfigure that
# makes this fire.
#
# The engine's output directory is left alone: deleting an engine shader also deletes the
# readShaderBinary call that named it, so a stale .spv there is unreferenced rather than
# silently preferred. Cost of the asymmetry is recompiling a game's shaders on reconfigure.
file(REMOVE_RECURSE ${GAME_SHADER_OUT_DIR})
file(MAKE_DIRECTORY ${GAME_SHADER_OUT_DIR})

set(ENGINE_SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders)
set(GAME_SHADER_SRC_DIR "")
if(SUBSTRATE_GAME AND IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/game/${SUBSTRATE_GAME}/shaders)
    set(GAME_SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/game/${SUBSTRATE_GAME}/shaders)
endif()

# Shared .glsl fragments are #included, not compiled on their own; every shader
# must rebuild when one of them changes.
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${ENGINE_SHADER_SRC_DIR}/*.glsl)
set(GAME_SHADER_INCLUDES "")
if(GAME_SHADER_SRC_DIR)
    file(GLOB GAME_SHADER_INCLUDES CONFIGURE_DEPENDS ${GAME_SHADER_SRC_DIR}/*.glsl)
endif()

# One loop over both trees rather than two loops or a function to jump to. What differs
# per tree is three lines at the top of the body: where it compiles to, and what it may
# include. A game shader can include out of the engine's tree; an engine shader cannot
# include out of a game's, which is the direction the static library already enforces in
# C++ and holds here for the same reason.
set(SHADER_BINARIES "")
set(GAME_SHADER_BINARIES "")

set(SHADER_TREES ${ENGINE_SHADER_SRC_DIR})
if(GAME_SHADER_SRC_DIR)
    list(APPEND SHADER_TREES ${GAME_SHADER_SRC_DIR})
endif()

foreach(tree ${SHADER_TREES})
    if("${tree}" STREQUAL "${GAME_SHADER_SRC_DIR}")
        set(out_dir       ${GAME_SHADER_OUT_DIR})
        set(include_flags -I${GAME_SHADER_SRC_DIR} -I${ENGINE_SHADER_SRC_DIR})
        set(include_deps  ${GAME_SHADER_INCLUDES} ${SHADER_INCLUDES})
    else()
        set(out_dir       ${SHADER_OUT_DIR})
        set(include_flags -I${ENGINE_SHADER_SRC_DIR})
        set(include_deps  ${SHADER_INCLUDES})
    endif()

    file(GLOB sources CONFIGURE_DEPENDS ${tree}/*.vert ${tree}/*.frag ${tree}/*.comp)

    set(binaries "")
    foreach(shader ${sources})
        get_filename_component(name ${shader} NAME)
        set(out ${out_dir}/${name}.spv)

        set(validate_cmd COMMAND ${CMAKE_COMMAND} -E true)
        if(SPIRV_VAL AND CMAKE_BUILD_TYPE STREQUAL "Debug")
            # --scalar-block-layout because VulkanContext enables scalarBlockLayout
            # unconditionally, so scalar is what the device actually validates against.
            # Without it the tracing shaders' vertex struct -- three floats then three
            # more, matching the C++ Vertex byte for byte -- is rejected for straddling a
            # 16-byte boundary under std430 rules the device is not using.
            set(validate_cmd COMMAND ${SPIRV_VAL} --target-env vulkan1.3 --scalar-block-layout ${out})
        endif()

        add_custom_command(
            OUTPUT ${out}
            COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3
                    ${include_flags} -o ${out} ${shader}
            ${validate_cmd}
            DEPENDS ${shader} ${include_deps}
            COMMENT "GLSL ${name}"
            VERBATIM)
        list(APPEND binaries ${out})
    endforeach()

    if("${tree}" STREQUAL "${GAME_SHADER_SRC_DIR}")
        set(GAME_SHADER_BINARIES ${binaries})
    else()
        set(SHADER_BINARIES ${binaries})
    endif()
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_BINARIES})
# Declared whether or not the game has any, so substrate_game() can depend on it
# unconditionally and a game that grows a shaders/ directory needs no CMake edit.
add_custom_target(game_shaders DEPENDS ${GAME_SHADER_BINARIES})

# ---------------------------------------------------------------- ASCII guard
# Runs on every build, not just a clean one: the point is to catch a stray
# non-English word in a comment on the build that introduces it, not weeks later.
# It is a whole-tree grep over ~6k lines and costs single-digit milliseconds.
add_custom_target(ascii_guard ALL
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_ascii.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "ASCII guard (engine/ game/ tests/ tools/)"
    VERBATIM)

# --------------------------------------------------------------------- engine
# Translation units that pull in neither Vulkan nor a window, listed separately
# because that is exactly the set a unit test can link against (5.1). Keeping it a
# list rather than promoting it to a library is deliberate: two targets is not three,
# and an OBJECT library here would buy nothing but a name.
set(SUBSTRATE_HOSTED_SOURCES
    engine/ai/Planner.cpp
    engine/core/AudioTap.cpp
    engine/core/Config.cpp
    engine/core/FileWrite.cpp
    engine/core/Input.cpp
    engine/core/Logger.cpp
    engine/core/Paths.cpp
    engine/core/Profiler.cpp
    engine/core/RangeAllocator.cpp
    engine/core/Recorder.cpp
    engine/core/Resources.cpp
    engine/core/SaveFile.cpp
    engine/core/Settings.cpp
    engine/gfx/DebugView.cpp
    engine/gfx/Decal.cpp
    engine/gfx/ImageTable.cpp
    engine/gfx/Light.cpp
    engine/gfx/Presentation.cpp
    engine/gfx/ViewTable.cpp
    engine/scene/Animation.cpp
    engine/scene/Audio.cpp
    engine/scene/AudioSource.cpp
    engine/scene/Camera.cpp
    engine/scene/CameraControllers.cpp
    engine/scene/Cloth.cpp
    engine/scene/Collider.cpp
    engine/scene/InstanceTable.cpp
    engine/scene/LitSprite.cpp
    engine/scene/Locomotion.cpp
    engine/scene/MeshLod.cpp
    engine/scene/miniaudio_impl.cpp
    engine/scene/NavMesh.cpp
    engine/scene/ParticleSystem.cpp
    engine/scene/Physics.cpp
    engine/scene/Scene.cpp
    engine/scene/SceneData.cpp
    engine/scene/SceneLoader.cpp
    engine/scene/Simulation.cpp
    engine/scene/SpatialIndex.cpp
    engine/scene/SpriteTable.cpp
    engine/scene/WorldSave.cpp
    engine/ui/BindingMenu.cpp
    engine/ui/Inspector.cpp
    engine/ui/SettingsUi.cpp
    engine/ui/Ui.cpp
)

# Device-free too, and deliberately not in the list above (D9). Both are hosted in the
# sense that matters -- neither names a Vulkan type, so `substrate-bake` links them with
# no driver present -- but neither belongs to every binary that links the engine:
#
#   SceneParse.cpp     the CPU half of a glTF load. Not in the unit suite, which has no
#                      document to parse and would otherwise link fastgltf and stb for
#                      nothing.
#   SceneCacheWrite.cpp  the sidecar *writer*. Not in `substrate`, and that is the whole
#                      mechanism behind D9's invariant: a static library links by object
#                      file, so while this shared a translation unit with the reader,
#                      every game that could read a `.scene` also shipped the code to
#                      write one. The unit suite links it because SceneDataTests.cpp and
#                      MeshLodTests.cpp write a cache and read it back.
set(SUBSTRATE_SCENE_PARSE_SOURCES engine/scene/SceneParse.cpp)
set(SUBSTRATE_SCENE_WRITE_SOURCES engine/scene/SceneCacheWrite.cpp)

# miniaudio's implementation is third-party code the project's warning set was never
# written for, and it is one file. Silenced as a whole here rather than by chasing the
# pragmas in miniaudio_impl.cpp across compiler versions: the point of that TU is to
# contain the library, and this is the rest of containing it.
set_source_files_properties(engine/scene/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# A library, not an executable, and that is the point of G1: `./build.sh` produces no
# runnable binary at all. The engine has to build, test and sanitize with nothing under
# game/ in the tree, which is the strongest available check that the boundary is real --
# a dependency leaking from a game into the engine becomes a link error rather than a
# code review.
add_library(substrate STATIC
    ${SUBSTRATE_HOSTED_SOURCES}
    engine/gfx/GpuProfiler.cpp
    engine/core/InputGlfw.cpp
    engine/gfx/vma_impl.cpp
    engine/gfx/VulkanContext.cpp
    engine/gfx/Swapchain.cpp
    engine/gfx/Renderer.cpp
    engine/gfx/Resources.cpp
    engine/gfx/SettingsBind.cpp
    engine/gfx/Pipeline.cpp
    engine/gfx/SpirvReflect.cpp
    engine/gfx/FrameCapture.cpp
    engine/gfx/RenderDoc.cpp
    engine/gfx/AccelStruct.cpp
    engine/gfx/Ktx2.cpp
    engine/scene/stb_impl.cpp
    ${SUBSTRATE_SCENE_PARSE_SOURCES}
    engine/scene/GltfScene.cpp
    engine/ui/Font.cpp
    engine/ui/FontData.cpp
    engine/Engine.cpp
)

# main() itself, and it is optional. ON by default: the engine compiles the entry point
# and `SUBSTRATE_GAME(T)` is the only thing a game writes to reach it. OFF hands the
# whole loop to a game that wants to drive `Engine` by hand.
#
# It lives in the archive rather than in each game because crt1.o's undefined `main` is
# what pulls it out -- so a game contributes one object file and gets a program.
option(SUBSTRATE_ENTRY_POINT "Compile main() into the engine library" ON)
if(SUBSTRATE_ENTRY_POINT)
    target_sources(substrate PRIVATE engine/Entry.cpp)
endif()

# Shader hot reload needs the shader *sources* and a glslang to run, and both are
# properties of the machine that did the build. ON is right for every development build
# and wrong for exactly one case: a package, where baking them in would ship a binary
# carrying the build machine's home directory and shelling out to a glslang at a path that
# does not exist on the player's disk. `build_release.sh` forces this OFF.
#
# The definitions it gates are the ones recompileShaders, pollShaderReload and the
# kShaderTrees table read, and all three are behind the matching `#if defined`.
option(SUBSTRATE_SHADER_HOT_RELOAD "Bake shader source paths for runtime recompilation" ON)

# Where a built binary looks for the things that ship beside it. OFF is a development
# build: absolute build-tree paths, so the binary works from any working directory and the
# tree it was built in is the tree it reads. ON is a package: relative names, which
# executableDir() anchors to the executable, so the whole directory can be moved, zipped,
# installed or mounted read-only somewhere no part of this machine's layout survives.
#
# One flag rather than four paths because getting a subset right is worse than getting none
# right -- a package with relative shaders and an absolute asset root looks like it works
# right up until it runs somewhere else.
#
# Note the game asset root in particular: its relative fallback in Resources.cpp is the
# empty string, which means "no game is configured" and not "look beside the binary". So a
# package needs these *defined to relative values*, not left undefined to fall back.
option(SUBSTRATE_PORTABLE "Resolve shaders and assets beside the executable, for packaging" OFF)

if(SUBSTRATE_PORTABLE AND SUBSTRATE_SHADER_HOT_RELOAD)
    # Not an error, because ON is the default and nobody asked for it. Hot reload's paths
    # are the build machine's shader sources and its glslang binary, and a package that
    # baked those in would carry a stranger's home directory and shell out to a compiler
    # that is not on the player's disk.
    message(STATUS "Portable build: disabling shader hot reload, which would bake this machine's paths in")
    set(SUBSTRATE_SHADER_HOT_RELOAD OFF)
endif()

# The game's asset tree, derived from the game's name without checking that the directory
# exists. Asset trees are generated (scripts/fetch_assets.sh) and gitignored, so a fresh
# clone configures before they are there; a root guarded on existence would silently stay
# empty until someone reconfigured. A root pointing at nothing simply misses and falls
# through to the engine's.
set(GAME_ASSET_DIR "")
if(SUBSTRATE_GAME)
    set(GAME_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game/${SUBSTRATE_GAME}/assets")
endif()

# The four runtime roots, resolved together, because SUBSTRATE_PORTABLE has to move all of
# them or none.
#
# The portable asset roots repeat the source tree's own shape -- `engine/assets` and
# `game/<name>/assets` -- rather than something tidier like `assets/engine`. That is not
# cosmetic, and it was a bug before it was a comment. A glTF names its buffers, images and
# audio relative to *itself*, and the composite scenes reach across into the other tree with
# `../../../engine/assets/...`; those walks only keep working if the two trees stay the same
# distance apart in the package as they are here. Flattening them put showcase.gltf two
# levels deep instead of three, and every external reference in it then resolved to
# somewhere outside the package -- which the loader reported as "an external buffer was not
# found", naming the buffer and not the reason.
#
# A portable build with no game configured still leaves the game root empty: that is the
# state ./build.sh alone produces, and it is not a package of anything.
if(SUBSTRATE_PORTABLE)
    set(RUNTIME_SHADER_DIR       "shaders")
    set(RUNTIME_GAME_SHADER_DIR  "shaders/game")
    set(RUNTIME_ENGINE_ASSET_DIR "engine/assets")
    set(RUNTIME_GAME_ASSET_DIR   "")
    if(SUBSTRATE_GAME)
        set(RUNTIME_GAME_ASSET_DIR "game/${SUBSTRATE_GAME}/assets")
    endif()
else()
    set(RUNTIME_SHADER_DIR       "${SHADER_OUT_DIR}")
    set(RUNTIME_GAME_SHADER_DIR  "${GAME_SHADER_OUT_DIR}")
    set(RUNTIME_ENGINE_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/assets")
    set(RUNTIME_GAME_ASSET_DIR   "${GAME_ASSET_DIR}")
endif()

# Link options for every executable this project produces on Windows, set at directory
# scope rather than per target on purpose. Put on `substrate_game()` alone -- which is
# where they started -- games got them and `substrate_tests.exe` did not, so the test
# binary needed libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll beside it and
# would not start on a machine without the MinGW runtime. The import-table check in
# build_release.sh only inspects the game, so nothing caught it; a rule that applies to
# "every exe" belongs where every exe can see it.
#
# Must precede the targets: add_link_options only affects ones created after it.
if(WIN32)
    add_link_options(
        # A .exe needing the MinGW runtime DLLs beside it is a .exe that does not start on
        # a machine without them, and the dialog names the DLL rather than the reason.
        -static -static-libgcc -static-libstdc++
        # MinGW defaults the main thread to 2 MB against Linux's 8. Jolt's temp allocator
        # and Renderer.cpp's larger stack-resident structs were written against the latter.
        -Wl,--stack,8388608
    )
endif()

add_dependencies(substrate shaders ascii_guard)

# PUBLIC: a game includes Engine.h, which reaches every subsystem header.
target_include_directories(substrate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)

target_compile_definitions(substrate
    PRIVATE
        # Where readShaderBinary looks, game tree first. Both are the same string for
        # every game -- `shaders/game` is a fixed name, not the game's -- which is what
        # keeps these two out of the reasons a game toggle recompiles the engine. A build
        # with no game configured misses on a directory that does not exist and falls
        # through to the engine's, so neither needs guarding at the call site.
        SUBSTRATE_GAME_SHADER_DIR="${RUNTIME_GAME_SHADER_DIR}"
        SUBSTRATE_SHADER_DIR="${RUNTIME_SHADER_DIR}"
        # Shader hot reload recompiles from source at runtime, so it needs the same
        # paths the build rules above use. All are absolute, so a binary launched from
        # anywhere finds them; a build tree moved after the fact loses hot reload and
        # nothing else. The game's source path is set below, on Renderer.cpp alone.
        #
        # Absent entirely when SUBSTRATE_SHADER_HOT_RELOAD is OFF, which is what a package
        # wants: not an empty string, absent, because the code tests `#if defined`.
        $<$<BOOL:${SUBSTRATE_SHADER_HOT_RELOAD}>:SUBSTRATE_SHADER_SRC_DIR="${ENGINE_SHADER_SRC_DIR}">
        $<$<BOOL:${SUBSTRATE_SHADER_HOT_RELOAD}>:SUBSTRATE_GLSLANG="${GLSLANG_VALIDATOR}">
        # project(Substrate VERSION ...) at the top of this file is the only place the
        # version is written. VulkanContext reports it to the driver, and the release
        # artifact's filename comes from the same variable, so the two cannot disagree.
        SUBSTRATE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        SUBSTRATE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        SUBSTRATE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        VMA_STATIC_VULKAN_FUNCTIONS=0    # volk loads them
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    PUBLIC
        GLM_FORCE_DEPTH_ZERO_TO_ONE      # Vulkan clip space is [0,1], not [-1,1]
        GLM_FORCE_RADIANS
        GLM_ENABLE_EXPERIMENTAL
        GLFW_INCLUDE_NONE                # volk.h owns the Vulkan header
        $<$<CONFIG:Debug>:SUBSTRATE_DEBUG>
)

# The one shader path that genuinely varies with the configured game, scoped to the one
# translation unit that reads it. Put on the target instead, a game toggle would recompile
# all eighteen; here it recompiles Renderer.cpp, which is what the comment beside
# SUBSTRATE_GAME at the top of this file promises. Same mechanism as the miniaudio
# warning suppression above.
#
# Empty when no game is configured or the game brings no shaders, which recompileShaders
# and pollShaderReload read as "one tree".
#
# Gated with the other two: with hot reload off, kShaderTrees is compiled out and this is
# the last of the three build-machine paths that would otherwise reach a shipped binary.
if(SUBSTRATE_SHADER_HOT_RELOAD)
    set_source_files_properties(engine/gfx/Renderer.cpp PROPERTIES
        COMPILE_DEFINITIONS "SUBSTRATE_GAME_SHADER_SRC_DIR=\"${GAME_SHADER_SRC_DIR}\"")
endif()

# The two asset trees a `res:/` name is looked up in, scoped to Resources.cpp for the same
# reason as the line above: the game's varies, and a define on the target would recompile
# the library on a toggle. Both defines go on the file rather than one on each, so that
# `substrate` and `substrate_tests` -- which both compile this hosted source, and only one
# of which gets the target's definitions -- cannot disagree about where assets are.
#
# Absolute in a development build and relative in a package; see SUBSTRATE_PORTABLE above,
# which is where both roots and the two shader ones are chosen together. Either way the
# reason `res:/` exists is unchanged: a name says *what* an asset is, and a root resolved
# against the working directory would put *where* straight back, as "the directory you
# were standing in".
set_source_files_properties(engine/core/Resources.cpp PROPERTIES
    COMPILE_DEFINITIONS "SUBSTRATE_GAME_ASSET_DIR=\"${RUNTIME_GAME_ASSET_DIR}\";SUBSTRATE_ENGINE_ASSET_DIR=\"${RUNTIME_ENGINE_ASSET_DIR}\"")

# The warning set every target in this project is held to, applied to games as well by
# substrate_game() below. One list, because a game compiled to a laxer standard than the
# engine is a game that finds the engine's bugs later than the engine does.
set(SUBSTRATE_WARNINGS
    -Wall -Wextra -Wpedantic
    # `VkFooCreateInfo info{VK_STRUCTURE_TYPE_FOO}` is the idiomatic Vulkan pattern:
    # set sType, zero the rest, assign what matters. This warning fires on every one.
    -Wno-missing-field-initializers
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O3>
)

# Every flag above is valid verbatim under MinGW, because MinGW *is* GCC -- that is the
# single largest payoff of choosing it over MSVC, and the reason there is no `if(MSVC)`
# branch in this file. These two are additions, not replacements.
if(WIN32)
    list(APPEND SUBSTRATE_WARNINGS
        # GetProcAddress returns FARPROC and every use casts it to something else. The
        # cast goes through void* at the one call site; this covers the rest of the idiom.
        -Wno-cast-function-type
        # Renderer.cpp is ~5700 lines of heavy glm and Jolt template instantiation, and at
        # -g3 its debug info exceeds COFF's 32768-section limit. The error is "too many
        # sections", names no cause, and is reached only in a Debug Windows build.
        $<$<CONFIG:Debug>:-Wa,-mbig-obj>
    )
endif()

target_compile_options(substrate PRIVATE ${SUBSTRATE_WARNINGS})

# PUBLIC throughout: these are usage requirements of the headers a game includes, not
# implementation details of the archive.
target_link_libraries(substrate PUBLIC
    volk::volk
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    fastgltf::fastgltf
    meshoptimizer
    stb
    rapidjson
    renderdoc_app
    glfw
    Jolt
    miniaudio
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

# ${CMAKE_DL_LIBS} is empty on Windows and `m` resolves to mingw-w64's stub libm, so both
# stay above rather than being conditioned out -- they cost nothing and removing them would
# make the Linux list read as though it were the special case.
#
# gdi32 is a genuine addition and a genuine trap. GLFW appends it to `glfw_PKG_LIBS`, which
# is the *pkg-config* string, and never to the static target -- so linking libglfw3.a
# directly leaves ChoosePixelFormat and SetPixelFormat undefined, out of wgl_context.c,
# with nothing in GLFW's own CMake to suggest why.
if(WIN32)
    target_link_libraries(substrate PUBLIC gdi32)
endif()

# ----------------------------------------------------------------------- games
# SUBSTRATE_GAME itself is declared at the top of this file, because the shader rules
# need it before any target exists.

# Everything a game's CMakeLists.txt needs, so that the file is one line. A game that
# outgrows it can still call add_executable itself -- nothing here is required.
function(substrate_game name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE substrate)
    target_compile_options(${name} PRIVATE ${SUBSTRATE_WARNINGS})
    # game_shaders is empty unless game/<name>/shaders/ exists, so this costs nothing
    # for a game that brings none.
    add_dependencies(${name} shaders game_shaders)
endfunction()

if(SUBSTRATE_GAME)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/game/${SUBSTRATE_GAME}/CMakeLists.txt)
        message(FATAL_ERROR
            "game/${SUBSTRATE_GAME}/CMakeLists.txt does not exist. "
            "Games live in game/<name>/; see game/demo/ for the shape of one.")
    endif()
    add_subdirectory(game/${SUBSTRATE_GAME})
endif()

# ----------------------------------------------------------------------- tools
# The offline scene baker (D9). A second hosted binary, and it exists for the same
# reason the unit suite does: the property that lets the suite run under ThreadSanitizer
# -- no device, no window, no shaders -- is the property that lets a baker run on a build
# machine or in a container with no GPU at all.
#
# It is the only writer of a `.scene` in the tree. `libsubstrate.a` does not link
# SUBSTRATE_SCENE_WRITE_SOURCES, so a shipped game holds no path that can produce one,
# and that is a link-time guarantee rather than a `#if` someone can flip.
add_executable(substrate-bake
    ${SUBSTRATE_HOSTED_SOURCES}
    ${SUBSTRATE_SCENE_PARSE_SOURCES}
    ${SUBSTRATE_SCENE_WRITE_SOURCES}
    engine/scene/stb_impl.cpp
    tools/bake.cpp
)

add_dependencies(substrate-bake ascii_guard)

target_include_directories(substrate-bake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)

target_compile_definitions(substrate-bake PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    $<$<CONFIG:Debug>:SUBSTRATE_DEBUG>
)

target_compile_options(substrate-bake PRIVATE
    ${SUBSTRATE_WARNINGS}
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O3>
)

# No volk, no glfw. That absence is the check: the day something device-side is pulled
# into a hosted translation unit, this target stops linking.
target_link_libraries(substrate-bake PRIVATE
    glm::glm
    fastgltf::fastgltf
    meshoptimizer
    stb
    rapidjson
    Jolt
    miniaudio
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

# The headless simulation loop (C27). A third hosted binary, for the same reason as the
# second: the property that lets the unit suite run under ThreadSanitizer is the property
# that lets a dedicated server, a CI regression run or a batch tuning job step a scene on a
# machine with no GPU in it.
#
# `--headless` is not this and never was -- it unmaps the window and still creates a real
# surface against a real driver. What makes this more than a second `main()` is that it calls
# the same `scene::Simulation::step` the drawn engine calls; a headless loop with its own copy
# of the call order is a loop that disagrees with the rendered one.
add_executable(substrate-sim
    ${SUBSTRATE_HOSTED_SOURCES}
    ${SUBSTRATE_SCENE_PARSE_SOURCES}
    engine/scene/stb_impl.cpp
    tools/sim.cpp
)

add_dependencies(substrate-sim ascii_guard)

target_include_directories(substrate-sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)

target_compile_definitions(substrate-sim PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    $<$<CONFIG:Debug>:SUBSTRATE_DEBUG>
)

target_compile_options(substrate-sim PRIVATE
    ${SUBSTRATE_WARNINGS}
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O3>
)

# No volk, no glfw, and the same sentence applies: that absence is the check. This target is
# the one that fails to link the day a device dependency reaches the simulation.
target_link_libraries(substrate-sim PRIVATE
    glm::glm
    fastgltf::fastgltf
    meshoptimizer
    stb
    rapidjson
    Jolt
    miniaudio
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

# ---------------------------------------------------------------------- tests
# Built by default, and that is the point of 5.1: a suite nobody builds is a suite
# nobody runs. `./test.sh` builds and runs it; `./build.sh <cfg> --target substrate`
# is the escape hatch for a build that wants the renderer alone.
#
# The binary links `SUBSTRATE_HOSTED_SOURCES` and nothing else, so it needs no device,
# no window and no shaders -- which is what lets it run under TSan, where the renderer
# cannot go at all.
add_executable(substrate_tests
    ${SUBSTRATE_HOSTED_SOURCES}
    ${SUBSTRATE_SCENE_WRITE_SOURCES}
    tests/main.cpp
    tests/AveragingBufferTests.cpp
    tests/PathsTests.cpp
    tests/ProfilerTests.cpp
    tests/ResourcesTests.cpp
    tests/NavMeshTests.cpp
    tests/RangeAllocatorTests.cpp
    tests/SceneLoaderTests.cpp
    tests/SaveFileTests.cpp
    tests/SceneDataTests.cpp
    tests/MeshLodTests.cpp
    tests/SceneTests.cpp
    tests/SimulationTests.cpp
    tests/SpatialIndexTests.cpp
    tests/WorldSaveTests.cpp
    tests/LoggerTests.cpp
    tests/ConfigTests.cpp
    tests/SettingsTests.cpp
    tests/DebugViewTests.cpp
    tests/ImageTableTests.cpp
    tests/ViewTableTests.cpp
    tests/PresentationTests.cpp
    tests/SpriteTableTests.cpp
    tests/LitSpriteTests.cpp
    tests/TilemapTests.cpp
    tests/BrushGeometryTests.cpp
    tests/LightTests.cpp
    tests/AnimationTests.cpp
    tests/InstanceTableTests.cpp
    tests/ParticleSystemTests.cpp
    tests/InputTests.cpp
    tests/CameraTests.cpp
    tests/BindingMenuTests.cpp
    tests/ClothTests.cpp
    tests/ColliderTests.cpp
    tests/PhysicsTests.cpp
    tests/LocomotionTests.cpp
    tests/PlannerTests.cpp
    tests/RigMergeTests.cpp
    tests/AudioSourceTests.cpp
    tests/AudioTests.cpp
    tests/AudioTapTests.cpp
    tests/RecorderTests.cpp
    tests/InspectorTests.cpp
    tests/UiTests.cpp
    tests/SettingsUiTests.cpp
)

add_dependencies(substrate_tests ascii_guard)

target_include_directories(substrate_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)

target_compile_definitions(substrate_tests PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    $<$<CONFIG:Debug>:SUBSTRATE_DEBUG>
)

# ${SUBSTRATE_WARNINGS} rather than a retyped subset of it. The comment on that list
# argues the case for the engine and for games -- "a game compiled to a laxer standard
# than the engine is a game that finds the engine's bugs later than the engine does" --
# and the test suite is the target that argument did not name and needed most. The four
# flags spelled here missed the two the list appends under if(WIN32), in the one target
# that is built on every Windows build.
target_compile_options(substrate_tests PRIVATE
    ${SUBSTRATE_WARNINGS}
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O3>
)

target_link_libraries(substrate_tests PRIVATE
    glm::glm
    meshoptimizer
    rapidjson
    Jolt
    miniaudio
    GTest::gtest
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

enable_testing()
add_test(NAME substrate_tests COMMAND substrate_tests)

message(STATUS "Substrate ${PROJECT_VERSION} | ${CMAKE_BUILD_TYPE} | C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Vulkan headers : ${Vulkan_INCLUDE_DIRS}")
message(STATUS "  glslang        : ${GLSLANG_VALIDATOR}")
message(STATUS "  shader output  : ${SHADER_OUT_DIR}")
if(SUBSTRATE_GAME)
    message(STATUS "  game           : ${SUBSTRATE_GAME} -> ${CMAKE_BINARY_DIR}/${SUBSTRATE_GAME}")
else()
    message(STATUS "  game           : none (engine and tests only; ./build_game.sh <name>)")
endif()
