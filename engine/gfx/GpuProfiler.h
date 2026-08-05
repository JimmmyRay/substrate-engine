#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

namespace gfx {

struct VulkanContext;

/**
 * @file engine/gfx/GpuProfiler.h
 * @brief Per-pass GPU timing via timestamp queries, emitted into the CPU trace.
 *
 * Timestamps go into a query pool partitioned per frame-in-flight and are read back only
 * once that frame's fence has signalled, so the CPU never blocks. Results are back-dated
 * into the CPU frame they belong to, which must still be inside the profiler's frame
 * window.
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

    /// Zones recorded past this are dropped, once with a warning. The pool is
    /// `framesInFlight * this * 2` timestamp queries.
    static constexpr uint32_t kMaxZonesPerFrame = 64;

    /**
     * @brief Set up the timestamp query pool.
     *
     * Returns a status rather than a bool: with timestamps unavailable every zone reports
     * 0.000 ms, and a caller that treats that as success prints a pass that reads as free.
     */
    [[nodiscard]] ProfilerStatus init(const gfx::VulkanContext& ctx, uint32_t framesInFlight);

    /// Whether GPU timing is actually being collected.
    [[nodiscard]] bool enabled() const { return queryPool != VK_NULL_HANDLE; }
    void shutdown(const gfx::VulkanContext& ctx);

    [[nodiscard]] bool available() const { return queryPool != VK_NULL_HANDLE; }

    /// Reset this frame's query range. Call once, first thing in the command buffer.
    void beginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t cpuFrameNumber);

    /// @param name Must have static lifetime (a string literal) -- the pointer is kept
    ///        until `collect` runs, several frames later.
    /// @return Zone handle for endZone(), or UINT32_MAX if the frame is full.
    uint32_t beginZone(VkCommandBuffer cmd, uint32_t frameSlot, const char* name);
    void endZone(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t zone);

    /// Read back a completed frame's timestamps and push them into the CPU trace.
    /// Safe to call only after that slot's fence has signalled.
    void collect(const gfx::VulkanContext& ctx, uint32_t frameSlot);

    /// Most recent duration recorded for a named zone, in milliseconds (0 if none).
    [[nodiscard]] double lastZoneMs(const char* name) const;

    /// True when GPU zones are placed on the CPU's clock rather than relative to the
    /// frame's first GPU timestamp. A trace read as though this were true when it is not
    /// asserts the GPU starts each frame the moment the CPU submitted it.
    [[nodiscard]] bool calibrated() const { return hostDomain != VK_TIME_DOMAIN_DEVICE_EXT; }

  private:
    /// @brief One matched (device tick, host nanosecond) pair. Sampled per frame, not once
    ///        at startup: the two oscillators drift, so a startup calibration is accurate
    ///        for about a second and slowly wrong after that.
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
    /// Held rather than passed: `beginFrame` has no `VulkanContext` to hand, and the
    /// calibration must be sampled in the frame it describes rather than in `collect`.
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
 * Also emits a `VK_EXT_debug_utils` label over the same range, which is what names the
 * pass in a RenderDoc capture. The label is deliberately *not* conditional on the
 * timestamp succeeding: a device without timestamps is the one whose frame is most worth
 * reading in a capture.
 */
class GpuScope {
  public:
    GpuScope(GpuProfiler& profiler, VkCommandBuffer cmd, uint32_t frameSlot, const char* literalName)
        : prof(&profiler), cmdBuf(cmd), slot(frameSlot) {
        // volk leaves the pointer unresolved when VK_EXT_debug_utils is not on the
        // instance, so this null check is the whole availability test.
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
