#include "gfx/GpuProfiler.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/VulkanContext.h"

#include <cstring>

namespace gfx {

ProfilerStatus GpuProfiler::init(const VulkanContext& ctx, uint32_t inFlight) {
    if (ctx.properties.limits.timestampComputeAndGraphics == VK_FALSE) {
        core::Logger::warn(core::LogCategory::Profile, "GPU timestamps unsupported on this queue; GPU zones disabled");
        return ProfilerStatus::TimestampsUnsupported;
    }

    framesInFlight = inFlight;
    timestampPeriod = ctx.timestampPeriod;
    device = ctx.device;
    frames.assign(inFlight, FrameZones{});

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = inFlight * kMaxZonesPerFrame * 2;

    gfx::vkCheck(vkCreateQueryPool(ctx.device, &info, nullptr, &queryPool), "vkCreateQueryPool");

    // ------------------------------------------------ calibrated timestamps (5.4)
    // Enabling this changes where GPU zones sit in the trace, so it says so once rather
    // than leaving a reader to work out which placement a capture was taken under.
    if (ctx.calibratedTimestampsSupported) {
        hostDomain = ctx.calibratedHostDomain;

        VkCalibratedTimestampInfoEXT infos[2]{
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, hostDomain},
        };
        uint64_t stamps[2]{};
        uint64_t maxDeviation = 0;
        const VkResult r = vkGetCalibratedTimestampsEXT(device, 2, infos, stamps, &maxDeviation);
        if (r != VK_SUCCESS) {
            // Advertised but not working. Fall back rather than emit a trace whose GPU
            // row is placed by an unchecked call's uninitialised output.
            hostDomain = VK_TIME_DOMAIN_DEVICE_EXT;
            core::Logger::warn(core::LogCategory::Profile,
                         "vkGetCalibratedTimestampsEXT failed (%s); GPU zones stay relative to frame start",
                         gfx::vkResultString(r));
        } else {
            core::Logger::status(core::LogCategory::Profile,
                           "GPU zones on the CPU clock (CLOCK_MONOTONIC, +/- %llu ns sampling deviation)",
                           static_cast<unsigned long long>(maxDeviation));
        }
    }

    core::Logger::status(core::LogCategory::Profile, "GPU profiler ready (%u queries, %.1f ns/tick, %s)", info.queryCount,
                   static_cast<double>(timestampPeriod), calibrated() ? "calibrated" : "uncalibrated");
    return ProfilerStatus::Enabled;
}

GpuProfiler::Calibration GpuProfiler::sampleCalibration() const {
    Calibration c;
    if (hostDomain == VK_TIME_DOMAIN_DEVICE_EXT) return c;

    VkCalibratedTimestampInfoEXT infos[2]{
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, hostDomain},
    };
    uint64_t stamps[2]{};
    uint64_t maxDeviation = 0;
    if (vkGetCalibratedTimestampsEXT(device, 2, infos, stamps, &maxDeviation) != VK_SUCCESS) return c;

    c.gpuTick = stamps[0];
    // CLOCK_MONOTONIC is reported in nanoseconds, which is what steady_clock counts on
    // this platform -- that identity is the whole reason this domain is the one asked
    // for in VulkanContext::init, and it is why no scaling appears here.
    c.hostNs = static_cast<int64_t>(stamps[1]);
    c.valid = true;
    return c;
}

void GpuProfiler::shutdown(const VulkanContext& ctx) {
    if (queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(ctx.device, queryPool, nullptr);
        queryPool = VK_NULL_HANDLE;
    }
    frames.clear();
    lastResults.clear();
}

void GpuProfiler::beginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t cpuFrameNumber) {
    if (queryPool == VK_NULL_HANDLE) return;

    // Queries must be reset on the device timeline before they are written again.
    vkCmdResetQueryPool(cmd, queryPool, queryBase(frameSlot), kMaxZonesPerFrame * 2);

    FrameZones& f = frames[frameSlot];
    f.count = 0;
    f.cpuFrameNumber = cpuFrameNumber;
    f.pending = true;
    // Taken here, and kept per slot, because collect() runs on this frame two or three
    // frames from now: a calibration sampled then would be extrapolated backwards
    // across the queue latency this is measuring.
    f.calib = sampleCalibration();
}

uint32_t GpuProfiler::beginZone(VkCommandBuffer cmd, uint32_t frameSlot, const char* name) {
    if (queryPool == VK_NULL_HANDLE) return UINT32_MAX;

    FrameZones& f = frames[frameSlot];
    if (f.count >= kMaxZonesPerFrame) {
        // Dropping a zone silently makes a pass look free rather than unmeasured,
        // which is the worst possible failure for a profiler. Say so, once.
        if (!overflowWarned) {
            overflowWarned = true;
            core::Logger::warn(core::LogCategory::Profile,
                         "GPU profiler: more than %u zones in one frame; '%s' and later zones are not timed. "
                         "Raise GpuProfiler::kMaxZonesPerFrame.",
                         kMaxZonesPerFrame, name);
        }
        return UINT32_MAX;
    }

    const uint32_t zone = f.count++;
    f.names[zone] = name;

    // BOTTOM_OF_PIPE, not TOP_OF_PIPE. A top-of-pipe timestamp is written as soon as
    // the command *enters* the pipeline, so the zone starts ticking while previous
    // work is still draining and absorbs that wait — passes then steal time from each
    // other and the per-pass split becomes meaningless. Bottom-of-pipe stamps once all
    // prior commands have completed, which is the actual pass boundary.
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queryPool, queryBase(frameSlot) + zone * 2);
    return zone;
}

void GpuProfiler::endZone(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t zone) {
    if (queryPool == VK_NULL_HANDLE || zone == UINT32_MAX) return;

    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queryPool,
                         queryBase(frameSlot) + zone * 2 + 1);
}

void GpuProfiler::collect(const VulkanContext& ctx, uint32_t frameSlot) {
    if (queryPool == VK_NULL_HANDLE) return;

    FrameZones& f = frames[frameSlot];
    if (!f.pending || f.count == 0) return;

    const uint32_t queryCount = f.count * 2;
    uint64_t stamps[kMaxZonesPerFrame * 2];

    // No WAIT bit: this slot's fence has already signalled, so the results are there.
    // If they somehow are not, skip rather than stall the frame.
    const VkResult r = vkGetQueryPoolResults(ctx.device, queryPool, queryBase(frameSlot), queryCount,
                                             sizeof(uint64_t) * queryCount, stamps, sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) {
        f.pending = false;
        return;
    }

    // Without calibration the CPU and GPU clocks are unrelated, so zones are placed
    // relative to the first timestamp of the frame. Durations are exact either way; it
    // is only the *position* that 5.4 fixes.
    const uint64_t frameStartTick = stamps[0];

    // A device tick is signed here on purpose: `begin` is routinely *earlier* than the
    // calibration point, because the calibration is taken while the CPU records a frame
    // the GPU has not started yet.
    const auto hostNsOf = [&f, this](uint64_t tick) {
        const double deltaNs = (static_cast<double>(tick) - static_cast<double>(f.calib.gpuTick)) * timestampPeriod;
        return f.calib.hostNs + static_cast<int64_t>(deltaNs);
    };

    // 5.4's one measurable claim, logged once: how far the GPU actually trails the CPU
    // that submitted the work. The uncalibrated placement asserts this number is zero.
    if (f.calib.valid && !latencyReported) {
        latencyReported = true;
        core::Logger::status(core::LogCategory::Profile, "GPU frame %llu starts %.3f ms after its calibration point",
                       static_cast<unsigned long long>(f.cpuFrameNumber),
                       static_cast<double>(hostNsOf(frameStartTick) - f.calib.hostNs) / 1e6);
    }

    for (uint32_t z = 0; z < f.count; ++z) {
        const uint64_t begin = stamps[z * 2];
        const uint64_t end = stamps[z * 2 + 1];
        if (end < begin) continue; // timestamp wrapped or was never written

        const double startUs = static_cast<double>(begin - frameStartTick) * timestampPeriod / 1000.0;
        const double durMs = static_cast<double>(end - begin) * timestampPeriod / 1000000.0;

        if (f.calib.valid) {
            core::Profiler::recordCalibratedGpuZone(f.cpuFrameNumber, f.names[z], hostNsOf(begin), durMs);
        } else {
            core::Profiler::recordGpuZone(f.cpuFrameNumber, f.names[z], startUs, durMs);
        }

        bool found = false;
        for (auto& lr : lastResults) {
            if (lr.name == f.names[z]) {
                lr.ms = durMs;
                found = true;
                break;
            }
        }
        if (!found) lastResults.push_back({f.names[z], durMs});
    }

    f.pending = false;
}

double GpuProfiler::lastZoneMs(const char* name) const {
    for (const auto& lr : lastResults) {
        if (lr.name == name || std::strcmp(lr.name, name) == 0) return lr.ms;
    }
    return 0.0;
}

} // namespace gfx
