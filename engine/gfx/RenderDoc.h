#pragma once

#include <cstdint>
#include <filesystem>

namespace gfx {

/**
 * @file engine/gfx/RenderDoc.h
 * @brief Trigger a RenderDoc frame capture from inside the process.
 *
 * Nothing here loads the layer. RenderDoc is a user implicit Vulkan layer gated on
 * `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`, which `scripts/rdoc.sh` sets, so the loader pulls
 * `librenderdoc.so` in before `main()`. Everything below therefore *detects* rather than
 * loads, and is a logged no-op when the layer is absent.
 */

/**
 * @brief Find the RenderDoc API if the capture layer is in this process.
 *
 * `pathTemplate` is a path prefix, not a filename: RenderDoc appends `_frameNNNN.rdc`,
 * so "debug_frames/rdoc/frame" produces "debug_frames/rdoc/frame_frame0060.rdc".
 * Parent directories are created.
 *
 * Safe to call when RenderDoc is absent, which is the normal case; returns false and
 * says so once. Calling twice is harmless.
 */
bool renderDocAttach(const std::filesystem::path& pathTemplate);

/// Whether renderDocAttach() found the API. False makes every call below a no-op.
bool renderDocAttached();

/**
 * @brief Capture the next `frames` whole frames.
 *
 * Whole frames delimited by `vkQueuePresentKHR`, so this must be called *before* the frame
 * you want; there is no way to bracket a partial one. Warns rather than failing when
 * RenderDoc is not attached.
 */
void renderDocTrigger(uint32_t frames = 1);

/// How many captures this process has written, so a script can assert a file appeared
/// rather than trusting exit code 0.
uint32_t renderDocCaptureCount();

} // namespace gfx
