#include "Trace.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <tuple>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// The frame's three blocks on the GPU. Subtracting them from the frame's wall time is what
/// separates "the CPU was busy" from "the CPU was asleep waiting for the GPU" -- without it
/// a GPU-bound frame reports a CPU cost equal to its GPU cost, because that is precisely
/// what wall time is. The engine's HUD subtracts the same three; keep the lists in step.
const char* const kBlockingPaths[] = {"Frame/Renderer::waitFence", "Frame/Renderer::acquire",
                                      "Frame/Renderer::present"};

struct Span {
    double start = 0.0;
    double end = 0.0;
    std::string name;

    bool operator<(const Span& other) const {
        return std::tie(start, end, name) < std::tie(other.start, other.end, other.name);
    }
};

/// Per frame: `Frame` less the union of the GPU zones inside it, in ms.
///
/// **Computed per frame and by union, because neither shortcut works.** Summing the per-zone
/// medians and subtracting them from the median `Frame` is a different quantity and a badly
/// behaved one: several zones are strongly right-skewed and skewed *together*, so the median
/// of the sum sits well above the sum of the medians -- half a millisecond of "unattributed"
/// frame that no timestamp supports. The union rather than the sum is what makes a nested
/// zone count once instead of twice.
///
/// Zones are clipped to `Frame`'s span, so one recorded outside it neither counts as named
/// time nor drags the residual negative.
std::vector<double> unnamedGpuMs(const std::map<int, std::vector<Span>>& spansByFrame) {
    std::vector<double> out;
    for (const auto& [frame, spans] : spansByFrame) {
        const Span* frameSpan = nullptr;
        for (const Span& span : spans) {
            if (span.name == "Frame") {
                frameSpan = &span;
                break;
            }
        }
        if (!frameSpan) continue;

        const double start = frameSpan->start;
        const double end = frameSpan->end;

        std::vector<Span> inner;
        for (const Span& span : spans) {
            if (span.name != "Frame") inner.push_back(span);
        }
        std::sort(inner.begin(), inner.end());

        double covered = 0.0;
        double cursor = start;
        for (const Span& span : inner) {
            const double lo = std::max(span.start, cursor);
            const double hi = std::min(span.end, end);
            if (hi > lo) {
                covered += hi - lo;
                cursor = hi;
            }
        }
        out.push_back((end - start - covered) / 1000.0);
    }
    return out;
}

bool isBlocking(const std::string& path) {
    for (const char* blocking : kBlockingPaths) {
        if (path == blocking) return true;
    }
    return false;
}

} // namespace

bool readTrace(const fs::path& path, Trace& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.generic_string();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    rapidjson::Document document;
    document.Parse(text.c_str());
    if (document.HasParseError()) {
        error = std::string("malformed trace: ") + rapidjson::GetParseError_En(document.GetParseError());
        return false;
    }

    // The profiler writes a bare array; a trace that has been through another tool arrives
    // wrapped in {"traceEvents": [...]}. Both are read, because rejecting the second is a
    // failure nobody can act on from the message.
    const rapidjson::Value* events = &document;
    if (document.IsObject() && document.HasMember("traceEvents")) {
        events = &document["traceEvents"];
    }
    if (!events->IsArray()) {
        error = "trace holds no event array";
        return false;
    }

    std::map<int, double> wallByFrame;
    std::map<int, double> blockedByFrame;
    std::map<int, std::vector<Span>> gpuSpans;

    for (const rapidjson::Value& event : events->GetArray()) {
        if (!event.IsObject() || !event.HasMember("ph")) continue;
        const std::string phase = event["ph"].GetString();

        // `M` is Chrome's metadata -- the thread_name events that label the tracks -- and
        // carries neither a duration nor a frame. `C` is a counter: a quantity rather than
        // a span, so it goes in its own table and never into `cpu`, where its "duration"
        // would be meaningless.
        if (phase == "C") {
            if (!event.HasMember("args") || !event.HasMember("name")) continue;
            const rapidjson::Value& args = event["args"];
            const int frame = args.HasMember("frame") ? args["frame"].GetInt() : 0;
            if (frame == 0) continue;
            const std::string name = event["name"].GetString();
            if (args.HasMember(name.c_str()) && args[name.c_str()].IsNumber()) {
                out.counters[name].push_back(args[name.c_str()].GetDouble());
            }
            continue;
        }
        if (phase != "X") continue;
        if (!event.HasMember("args") || !event.HasMember("dur") || !event.HasMember("name")) continue;

        const rapidjson::Value& args = event["args"];
        if (!args.HasMember("frame")) continue;

        const int frame = args["frame"].GetInt();
        const double durUs = event["dur"].GetDouble();
        const double ms = durUs / 1000.0;
        const std::string name = event["name"].GetString();
        const std::string category = event.HasMember("cat") ? event["cat"].GetString() : "";
        const std::string pathStr = args.HasMember("path") ? args["path"].GetString() : name;

        // Frame 0 is the startup frame -- window creation, device init and asset load all
        // happen inside it -- and the profiler pins it in the window on purpose, which is
        // exactly why a benchmark has to take it back out.
        if (frame == 0) {
            if (category != "gpu") {
                StartupZone& zone = out.startup[pathStr];
                zone.totalMs += ms;
                zone.count += 1;
            }
            continue;
        }

        if (category == "gpu") {
            out.gpu[name].push_back(ms);
            const double ts = event.HasMember("ts") ? event["ts"].GetDouble() : 0.0;
            gpuSpans[frame].push_back({ts, ts + durUs, name});
            continue;
        }

        if (name == "Frame" && args.HasMember("depth") && args["depth"].GetInt() == 0) {
            // Reported as `wall frame`, so listing it again as a CPU zone would be the same
            // number under two labels.
            wallByFrame[frame] = ms;
            continue;
        }

        static const std::string kFramePrefix = "Frame/";
        const std::string key = pathStr.rfind(kFramePrefix, 0) == 0
                                    ? pathStr.substr(kFramePrefix.size())
                                    : pathStr;
        out.cpu[key].push_back(ms);
        if (isBlocking(pathStr)) blockedByFrame[frame] += ms;
    }

    for (const auto& [frame, ms] : wallByFrame) {
        out.wallMs.push_back(ms);
        const auto blocked = blockedByFrame.find(frame);
        const double block = blocked == blockedByFrame.end() ? 0.0 : blocked->second;
        out.busyMs.push_back(std::max(0.0, ms - block));
    }

    out.unnamedMs = unnamedGpuMs(gpuSpans);
    return true;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<long>(middle), values.end());
    const double upper = values[middle];
    if (values.size() % 2 == 1) return upper;

    const double lower = *std::max_element(values.begin(),
                                           values.begin() + static_cast<long>(middle));
    return (lower + upper) / 2.0;
}

double minimum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
}

double maximum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

} // namespace tool
