#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

/**
 * @file tools/cli/Trace.h
 * @brief The Chrome trace the engine writes, as numbers a table can be built from.
 *
 * Read the trace, never the `GPU @` log line: that line is `GpuProfiler::lastZoneMs`, one
 * frame, whichever the last collect happened to land on -- so a median over runs of it is a
 * median over arbitrary frames.
 */
namespace tool {

/// Frame 0's CPU zones, summed and counted rather than kept as one sample. "There is one
/// startup" does not mean "there is one of each zone in it": the demo builds the
/// acceleration structure five times inside `Game::init`, and last-value-wins reported that
/// as the cost of the last one.
struct StartupZone {
    double totalMs = 0.0;
    int count = 0;
};

/// GPU zones are keyed by name, CPU zones by path, and that asymmetry is deliberate: a CPU
/// scope mirrors the GPU zone of the pass it records, so `GBuffer` names one of each and a
/// single table keyed by name would silently pool the two.
struct Trace {
    std::map<std::string, std::vector<double>> gpu;
    std::map<std::string, std::vector<double>> cpu;
    std::map<std::string, std::vector<double>> counters;

    /// Frame-to-frame time. This is what FPS means, and it contains every block on the GPU.
    std::vector<double> wallMs;

    /// Wall less the frame's three blocks on the GPU, and the only one of the two that
    /// answers "would a faster CPU help".
    std::vector<double> busyMs;

    std::map<std::string, StartupZone> startup;

    /// Per frame, `Frame` less the union of the GPU zones inside it.
    std::vector<double> unnamedMs;

    bool empty() const { return gpu.empty() || wallMs.empty(); }
};

/// Returns false having said why on stderr.
bool readTrace(const std::filesystem::path& path, Trace& out, std::string& error);

/// Python's `statistics.median`: the mean of the two middle values on an even count, not
/// the lower of them. Every published figure in the architecture docs was produced that
/// way, so an off-by-half-a-sample here silently restates the baseline.
double median(std::vector<double> values);

double minimum(const std::vector<double>& values);
double maximum(const std::vector<double>& values);

} // namespace tool
