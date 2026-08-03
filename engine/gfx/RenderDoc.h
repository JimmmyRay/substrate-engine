#pragma once

#include <cstdint>
#include <filesystem>

namespace gfx {

/**
 * @brief Trigger a RenderDoc frame capture from inside the process.
 *
 * Three free functions and a file-static API pointer, in the same shape as
 * FrameCapture: there is no state worth a class, and the one thing that *is* state --
 * the API table RenderDoc hands back -- is a process-wide singleton that a class
 * instance could only pretend to own.
 *
 * ## Why in-application rather than the F12 hotkey
 *
 * RenderDoc's default capture trigger is a keypress, which means a shell script has to
 * go through `xdotool` against a Vulkan swapchain to take one. Tier 2 did exactly that
 * for its visual checks and it repeatedly cost more time than the change being
 * verified -- it grabs whichever window is topmost, and it cannot state *which* frame
 * it caught. The in-application API removes both problems: `--rdoc-capture-frame 60`
 * captures frame 60, headlessly, every time.
 *
 * ## How the layer gets into the process
 *
 * It is not loaded from here. RenderDoc is registered as a user implicit Vulkan layer
 * gated on `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`, so the loader pulls
 * `librenderdoc.so` in before `main()` when that variable is set and does not
 * otherwise. `scripts/rdoc.sh` sets it. Everything here therefore *detects* rather
 * than loads, and is a logged no-op when the layer is absent -- the same treatment
 * `ProfilerStatus::TimestampsUnsupported` gets, and for the same reason: a capture
 * that silently did not happen is worse than one that says why.
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
 * Whole frames delimited by `vkQueuePresentKHR`, so this must be called *before* the
 * frame you want -- there is nothing to bracket around `drawFrame` and no chance to
 * catch a partial one. Warns rather than failing when RenderDoc is not attached: a run
 * that was asked for a capture and produced none should say why.
 */
void renderDocTrigger(uint32_t frames = 1);

/// How many captures this process has written. The point of returning it is that a
/// script can assert a file appeared rather than trusting exit code 0.
uint32_t renderDocCaptureCount();

} // namespace gfx
