#pragma once

#include "scene/SceneData.h"

#include <filesystem>

/**
 * @file engine/scene/SceneParse.h
 * @brief Filling a `SceneData` from a glTF document, or from the sidecar beside it.
 *
 * The two entry points to the CPU half of a scene load, in a header that reaches no Vulkan
 * type. Declaring them in `GltfScene.h` would make anything that wanted the parse -- the
 * baker included -- drag the device sources in behind it.
 *
 * Nothing below `loadSceneCpu` can tell which way the struct was filled, and that is the
 * property the golden suite tests.
 */
namespace scene {

/**
 * @brief Parse the document at `path`, ignoring any sidecar beside it.
 *
 * What `substrate-bake` calls to produce a sidecar. `embedded` comes back sized like
 * `data.images`, holding the encoded bytes of any image the document embedded; it is not
 * part of `SceneData` because the sidecar holds an image *list* and never pixels.
 */
[[nodiscard]] bool parseSceneData(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded);

/**
 * @brief Fill `data` from the sidecar if one applies, and from the document otherwise.
 *
 * What a running game calls, and the only one of the two it links. It never writes a
 * sidecar: a cache that does not apply is not an error and is not rebuilt, because baking
 * is `substrate-bake`'s job. Safe on a worker thread -- it touches no device and no window.
 */
[[nodiscard]] bool loadSceneCpu(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded);

} // namespace scene
