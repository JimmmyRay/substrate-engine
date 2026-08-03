#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

namespace gfx {

struct VulkanContext;

/**
 * @brief Per-pass GPU timing via timestamp queries, emitted into the CPU trace.
 *
 * CPU scopes cannot see GPU cost, and for a deferred renderer the per-pass GPU
 * number is the one that decides everything — including what MSAA actually costs.
 *
 * Timestamps are written into a query pool partitioned per frame-in-flight, and
 * read back only once that frame's fence has signalled, so the CPU never blocks on
 * the GPU. Results are back-dated into the CPU frame they belong to, which is still
 * buffered in the profiler's frame window.
 */
/// Whether GPU timing came up. Not a bool: see GpuProfiler::init.
enum class ProfilerStatus {
    Enabled,
    TimestampsUnsupported,
};

class GpuProfiler {
  public:
    /// Non-copyable; see the note on `Uploader` in Resources.h.
    GpuProfiler() = default;
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    /// Zones recorded past this are dropped, once with a warning. 64 is headroom for
    /// shadows, SSAO and bloom on top of the three passes that exist; the whole pool
    /// is 2 * 64 * 2 = 256 timestamp queries, which costs nothing worth measuring.
    static constexpr uint32_t kMaxZonesPerFrame = 64;

    /**
     * @brief Set up the timestamp query pool.
     *
     * Returns a status rather than a bool because degrading silently here is worse
     * than failing: with timestamps unavailable every GPU zone reports 0.000 ms, and
     * a pass that reads as free is indistinguishable from a pass that is free. The
     * caller is expected to say so rather than print zeros.
     */
    [[nodiscard]] ProfilerStatus init(const gfx::VulkanContext& ctx, uint32_t framesInFlight);

    /// Whether GPU timing is actually being collected.
    [[nodiscard]] bool enabled() const { return queryPool != VK_NULL_HANDLE; }
    void shutdown(const gfx::VulkanContext& ctx);

    [[nodiscard]] bool available() const { return queryPool != VK_NULL_HANDLE; }

    /// Reset this frame's query range. Call once, first thing in the command buffer.
    void beginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t cpuFrameNumber);

    /// @param name Must have static lifetime (a string literal).
    /// @return Zone handle for endZone(), or UINT32_MAX if the frame is full. A full
    ///         frame warns once rather than silently losing the pass.
    uint32_t beginZone(VkCommandBuffer cmd, uint32_t frameSlot, const char* name);
    void endZone(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t zone);

    /// Read back a completed frame's timestamps and push them into the CPU trace.
    /// Safe to call only after that slot's fence has signalled.
    void collect(const gfx::VulkanContext& ctx, uint32_t frameSlot);

    /// Most recent duration recorded for a named zone, in milliseconds (0 if none).
    [[nodiscard]] double lastZoneMs(const char* name) const;

    /// True when GPU zones are placed on the CPU's clock rather than relative to the
    /// frame's first GPU timestamp (5.4). Reported rather than assumed: the two
    /// placements are the difference between a trace you can reason about queue latency
    /// from and one where every pass appears to start when the CPU submitted it.
    [[nodiscard]] bool calibrated() const { return hostDomain != VK_TIME_DOMAIN_DEVICE_EXT; }

  private:
    /**
     * @brief One matched (device tick, host nanosecond) pair (5.4).
     *
     * Sampled per frame rather than once at startup because the two oscillators drift.
     * A single startup calibration is accurate for about a second and then slowly
     * wrong, which is the worst kind of wrong -- it looks right in the first capture
     * anyone takes.
     */
    struct Calibration {
        uint64_t gpuTick = 0;
        int64_t hostNs = 0;
        bool valid = false;
    };

    struct FrameZones {
        const char* names[kMaxZonesPerFrame]{};
        uint32_t count = 0;
        uint64_t cpuFrameNumber = 0;
        bool pending = false;
        Calibration calib;
    };

    /// Sample a (device, host) pair, or leave it invalid where the extension is absent.
    [[nodiscard]] Calibration sampleCalibration() const;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    /// Held rather than passed, because beginFrame() records into a command buffer and
    /// has no VulkanContext to hand -- and the calibration has to be taken *in* the
    /// frame it describes, not two frames later when collect() runs.
    VkDevice device = VK_NULL_HANDLE;
    VkTimeDomainEXT hostDomain = VK_TIME_DOMAIN_DEVICE_EXT;
    uint32_t framesInFlight = 0;
    float timestampPeriod = 1.0f;
    bool overflowWarned = false; ///< Warn once, not once per frame at 600 FPS.
    bool latencyReported = false;
    std::vector<FrameZones> frames;

    struct ZoneResult {
        const char* name;
        double ms;
    };
    std::vector<ZoneResult> lastResults;

    [[nodiscard]] uint32_t queryBase(uint32_t frameSlot) const { return frameSlot * kMaxZonesPerFrame * 2; }
};

/**
 * @brief RAII GPU zone. Mirrors ProfileScope, but for command-buffer time.
 *
 * Also emits a `VK_EXT_debug_utils` label around the same range, which is what names
 * the pass in a RenderDoc capture. Every pass in `Renderer` is already wrapped in one
 * of these, so putting the label here names all of them with no new call sites, and a
 * future nested scope nests correctly because Vulkan labels are a stack.
 *
 * The label is deliberately *not* tied to the timestamp succeeding. `beginZone`
 * returns early when the query pool is absent, and a device without timestamps is
 * exactly the one whose frame you most want to read in a capture.
 */
class GpuScope {
  public:
    GpuScope(GpuProfiler& profiler, VkCommandBuffer cmd, uint32_t frameSlot, const char* literalName)
        : prof(&profiler), cmdBuf(cmd), slot(frameSlot) {
        // Null whenever VK_EXT_debug_utils is not on the instance -- volk leaves the
        // pointer unresolved -- so this is the whole availability check.
        if (vkCmdBeginDebugUtilsLabelEXT != nullptr) {
            VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
            label.pLabelName = literalName;
            vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
            labelled = true;
        }
        zone = profiler.beginZone(cmd, frameSlot, literalName);
    }

    ~GpuScope() {
        if (zone != UINT32_MAX) prof->endZone(cmdBuf, slot, zone);
        if (labelled) vkCmdEndDebugUtilsLabelEXT(cmdBuf);
    }

    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

  private:
    GpuProfiler* prof;
    VkCommandBuffer cmdBuf;
    uint32_t slot;
    uint32_t zone = UINT32_MAX;
    bool labelled = false;
};

} // namespace gfx
