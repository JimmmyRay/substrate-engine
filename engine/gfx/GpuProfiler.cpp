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
            // Advertised but not working: without the fallback the trace's GPU row would
            // be placed from this call's uninitialised output.
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
    // No scaling: CLOCK_MONOTONIC is reported in nanoseconds, which is what steady_clock
    // counts here. Any other host domain would need one, which is why `VulkanContext::init`
    // accepts no other.
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
    // Sampled here and kept per slot: `collect` runs two or three frames later, so a
    // calibration taken there extrapolates backwards across the latency being measured.
    f.calib = sampleCalibration();
}

uint32_t GpuProfiler::beginZone(VkCommandBuffer cmd, uint32_t frameSlot, const char* name) {
    if (queryPool == VK_NULL_HANDLE) return UINT32_MAX;

    FrameZones& f = frames[frameSlot];
    if (f.count >= kMaxZonesPerFrame) {
        // A dropped zone makes a pass look free rather than unmeasured, so it has to say
        // so -- once, not once per frame at 600 FPS.
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

    // BOTTOM_OF_PIPE, not TOP_OF_PIPE: a top-of-pipe stamp is written as the command
    // enters the pipeline, so the zone absorbs the previous pass's drain and the passes
    // steal time from each other.
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

    // No WAIT bit: this slot's fence has already signalled. Adding one turns a missing
    // result into a stall in the frame loop.
    const VkResult r = vkGetQueryPoolResults(ctx.device, queryPool, queryBase(frameSlot), queryCount,
                                             sizeof(uint64_t) * queryCount, stamps, sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) {
        f.pending = false;
        return;
    }

    // Without calibration the two clocks are unrelated, so zones are placed relative to
    // the frame's first timestamp. Durations are exact either way.
    const uint64_t frameStartTick = stamps[0];

    // The tick delta is signed on purpose: `begin` is routinely *earlier* than the
    // calibration point, taken while the CPU recorded a frame the GPU had not started.
    const auto hostNsOf = [&f, this](uint64_t tick) {
        const double deltaNs = (static_cast<double>(tick) - static_cast<double>(f.calib.gpuTick)) * timestampPeriod;
        return f.calib.hostNs + static_cast<int64_t>(deltaNs);
    };

    // How far the GPU trails the CPU that submitted the work -- the number the
    // uncalibrated placement asserts is zero.
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
