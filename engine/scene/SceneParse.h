#pragma once

#include "scene/SceneData.h"

#include <filesystem>

/**
 * @file engine/scene/SceneParse.h
 * @brief Filling a `SceneData` from a glTF document, or from the sidecar beside it (D9).
 *
 * The two entry points to the CPU half of a scene load, in a header that reaches no Vulkan
 * type. `GltfScene.h` cannot declare these: it owns `VkBuffer`s, so anything that included
 * it to reach the parse would drag the device sources in behind it -- which is exactly what
 * kept the baker inside the renderer until D9.
 *
 * `GltfScene::load` is now "call `loadSceneCpu`, then decode, upload and describe", and the
 * half below the call cannot tell which way the struct was filled. That is C15's property
 * and it is what makes the golden suite a proof rather than a smoke test.
 */
namespace scene {

/**
 * @brief Parse the document at `path`, ignoring any sidecar beside it.
 *
 * The work C15's cache exists to skip, and what `substrate-bake` calls to produce one.
 * `embedded` comes back sized like `data.images`, holding the encoded bytes of any image
 * the document embedded -- deliberately not part of `SceneData`, because the sidecar holds
 * an image *list* and never pixels.
 */
[[nodiscard]] bool parseSceneData(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded);

/**
 * @brief Fill `data` from the sidecar if one applies, and from the document otherwise.
 *
 * What a running game calls, and the only one of the two it links. It never writes: a
 * cache that does not apply is not an error, is not logged, and is not rebuilt -- baking
 * is `substrate-bake`'s job, run once at package time. This is the half C10 put on a
 * worker thread, so it touches no device and no window.
 */
[[nodiscard]] bool loadSceneCpu(const std::filesystem::path& path, SceneData& data, EmbeddedImages& embedded);

} // namespace scene
