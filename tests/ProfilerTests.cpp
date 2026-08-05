#include "core/Config.h"
#include "core/Profiler.h"

#include "core/Settings.h"

#include <rapidjson/document.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace core;

namespace fs = std::filesystem;

/**
 * @file tests/ProfilerTests.cpp
 * @brief The CPU profiler (5.1).
 *
 * Three concurrency bugs were found in this profiler by hand during the port. Every
 * assertion below is on observable output -- the JSON `toJson()` returns and the Chrome
 * trace `writeToFile()` writes -- rather than on internals, because `Profiler::Impl` is
 * deliberately not in the header and a test that reached into it would be testing a
 * shape rather than a behaviour.
 *
 * The recording path only stores a scope while a frame is open, and a frame is only
 * moved into the pending window by the *next* `beginFrame()`. So the shape of nearly
 * every test here is: open a frame, do the thing, open another frame, then read.
 */

namespace {

/// Parse `toJson()` and hand back the scopes array. Fails the calling test rather than
/// returning a half-valid document, so a parse error reads as a parse error.
rapidjson::Document parseJson(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.c_str());
    EXPECT_FALSE(doc.HasParseError()) << "profiler emitted invalid JSON: " << text;
    return doc;
}

/// One frame's worth of scope names, in the order they were recorded.
std::vector<std::string> scopeNames(const rapidjson::Document& doc) {
    std::vector<std::string> out;
    if (!doc.IsObject() || !doc.HasMember("scopes")) return out;
    for (const auto& s : doc["scopes"].GetArray()) out.emplace_back(s["name"].GetString());
    return out;
}

const rapidjson::Value* findScope(const rapidjson::Document& doc, const char* name) {
    if (!doc.IsObject() || !doc.HasMember("scopes")) return nullptr;
    for (const auto& s : doc["scopes"].GetArray()) {
        if (std::string(s["name"].GetString()) == name) return &s;
    }
    return nullptr;
}

/// Record one frame containing `body`, then open a second frame so the first one lands
/// in the pending window where toJson() can see it.
template <typename Fn> void oneFrame(Fn&& body) {
    {
        auto frame = Profiler::beginFrame();
        body();
    }
    { auto flush = Profiler::beginFrame(); }
}

class ProfilerTest : public ::testing::Test {
  protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "substrate_profiler_tests";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override {
        Profiler::shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

// ================================================================= lifecycle

TEST_F(ProfilerTest, EnabledFollowsConfigAndSetEnabled) {
    Profiler::init();
    EXPECT_TRUE(Profiler::enabled());

    Profiler::setEnabled(false);
    EXPECT_FALSE(Profiler::enabled());
    Profiler::setEnabled(true);
    EXPECT_TRUE(Profiler::enabled());

    Profiler::shutdown();
    // Not merely disabled: with no impl there is nothing to be enabled.
    EXPECT_FALSE(Profiler::enabled());
}

TEST_F(ProfilerTest, DisabledProfilerRecordsNothing) {
    ProfilerConfig cfg;
    cfg.enabled = false;
    Profiler::init(cfg);

    oneFrame([] { auto s = Profiler::scope("NeverRecorded"); });

    EXPECT_EQ(Profiler::toJson(), "{}");
}

TEST_F(ProfilerTest, DisabledProfilerCreatesNoTraceFile) {
    // `--no-profiler --trace <path>` used to leave a zero-byte file and a running writer
    // thread: the truncate and the thread start were gated on the *path* alone. A flag
    // that says "no trace" and produces a file is the same defect as one that says "no
    // GPU queries" and writes them, one layer down.
    const fs::path out = dir / "disabled.json";

    ProfilerConfig cfg;
    cfg.enabled = false;
    cfg.outputFile = out.string();
    cfg.autoFlushFrames = 1;
    Profiler::init(cfg);

    oneFrame([] { auto s = Profiler::scope("NeverWritten"); });
    Profiler::shutdown();

    EXPECT_FALSE(fs::exists(out));
}

TEST_F(ProfilerTest, ToJsonIsEmptyObjectBeforeAnyFrameCloses) {
    Profiler::init();
    EXPECT_EQ(Profiler::toJson(), "{}");

    { auto frame = Profiler::beginFrame(); }
    // The frame is open, not closed: still nothing in the window.
    EXPECT_EQ(Profiler::toJson(), "{}");
}

TEST_F(ProfilerTest, FrameNumberCountsClosedFrames) {
    Profiler::init();
    EXPECT_EQ(Profiler::frameNumber(), 0u);

    for (int i = 0; i < 4; ++i) { auto frame = Profiler::beginFrame(); }
    // Four beginFrame() calls close three frames; the fourth is still open.
    EXPECT_EQ(Profiler::frameNumber(), 3u);
}

// ============================================================ scope recording

TEST_F(ProfilerTest, ScopeCarriesNamePathDepthAndDuration) {
    Profiler::init();

    oneFrame([] {
        auto s = Profiler::scope("Work");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });

    const rapidjson::Document doc = parseJson(Profiler::toJson());
    ASSERT_TRUE(doc.IsObject());
    EXPECT_EQ(doc["frame"].GetUint64(), 0u);

    const rapidjson::Value* work = findScope(doc, "Work");
    ASSERT_NE(work, nullptr);
    EXPECT_EQ(std::string((*work)["path"].GetString()), "Frame/Work");
    EXPECT_EQ((*work)["depth"].GetUint(), 1u);
    // Slept 2 ms, so anything at or below zero means the clock is not being read.
    EXPECT_GT((*work)["cpuMs"].GetDouble(), 0.0);
}

TEST_F(ProfilerTest, NestedScopesGetIncreasingDepthAndAHierarchicalPath) {
    Profiler::init();

    oneFrame([] {
        auto outer = Profiler::scope("Outer");
        auto middle = Profiler::scope("Middle");
        auto inner = Profiler::scope("Inner");
    });

    const rapidjson::Document doc = parseJson(Profiler::toJson());

    const rapidjson::Value* inner = findScope(doc, "Inner");
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ((*inner)["depth"].GetUint(), 3u);
    EXPECT_EQ(std::string((*inner)["path"].GetString()), "Frame/Outer/Middle/Inner");

    const rapidjson::Value* middle = findScope(doc, "Middle");
    ASSERT_NE(middle, nullptr);
    EXPECT_EQ((*middle)["depth"].GetUint(), 2u);
}

TEST_F(ProfilerTest, SameNameUnderDifferentParentsGetsADifferentPath) {
    // The path hash folds the parent in for exactly this case: two passes that both
    // have an "Upload" step are two lines in a trace, not one averaged together.
    Profiler::init();

    oneFrame([] {
        {
            auto a = Profiler::scope("PassA");
            auto u = Profiler::scope("Upload");
        }
        {
            auto b = Profiler::scope("PassB");
            auto u = Profiler::scope("Upload");
        }
    });

    const rapidjson::Document doc = parseJson(Profiler::toJson());
    std::vector<std::string> uploadPaths;
    for (const auto& s : doc["scopes"].GetArray()) {
        if (std::string(s["name"].GetString()) == "Upload") uploadPaths.emplace_back(s["path"].GetString());
    }

    ASSERT_EQ(uploadPaths.size(), 2u);
    EXPECT_NE(uploadPaths[0], uploadPaths[1]);
    EXPECT_EQ(uploadPaths[0], "Frame/PassA/Upload");
    EXPECT_EQ(uploadPaths[1], "Frame/PassB/Upload");
}

TEST_F(ProfilerTest, ScopesCloseInnermostFirst) {
    Profiler::init();

    oneFrame([] {
        auto outer = Profiler::scope("Outer");
        { auto inner = Profiler::scope("Inner"); }
    });

    // Recording happens on destruction, so the inner scope is written first and the
    // enclosing Frame scope last. A trace viewer relies on nothing here, but a stack
    // that unwound in the wrong order would show up as this ordering changing.
    const std::vector<std::string> names = scopeNames(parseJson(Profiler::toJson()));
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "Inner");
    EXPECT_EQ(names[1], "Outer");
    EXPECT_EQ(names[2], "Frame");
}

TEST_F(ProfilerTest, MovedFromScopeRecordsExactlyOnce) {
    Profiler::init();

    oneFrame([] {
        auto s = Profiler::scope("Moved");
        auto other = std::move(s);
    });

    const std::vector<std::string> names = scopeNames(parseJson(Profiler::toJson()));
    EXPECT_EQ(std::count(names.begin(), names.end(), "Moved"), 1);
}

// Move *assignment* is deleted, so `a = std::move(b)` no longer compiles. It cannot be
// written correctly -- closing `a` means popping an entry that `b` is sitting on top of --
// and left defaulted it leaked one stack entry per use, giving every later scope on that
// thread the wrong depth. The compile-time proof is below; move construction, which is
// what `Profiler::scope()` returning by value needs, is tested above.
static_assert(!std::is_move_assignable_v<ProfileScope>,
              "ProfileScope must not be move-assignable: closing a scope pops a LIFO "
              "stack, and assigning over a live one has no legal pop order");
static_assert(std::is_move_constructible_v<ProfileScope>,
              "Profiler::scope() returns by value and needs move construction");

// =================================================================== scopef

TEST_F(ProfilerTest, ScopefFormatsAndThenCollapsesOnceThePoolIsFull) {
    // Both halves live in one test on purpose. Interning is permanent and process-wide,
    // so filling the pool in a separate test would silently change what every later
    // scopef() call in the binary returns -- an ordering dependency that would look
    // like a flake.
    Profiler::init();

    oneFrame([] { auto s = Profiler::scopef("Shadow cascade %d", 3); });
    EXPECT_NE(findScope(parseJson(Profiler::toJson()), "Shadow cascade 3"), nullptr);

    // Fill the pool with no frame open, so this costs interning and not 5000 recorded
    // scopes. The cap is 4096 names; overshooting it is what makes the test independent
    // of how many names earlier assertions happened to intern.
    for (int i = 0; i < 5000; ++i) {
        auto s = Profiler::scopef("filler %d", i);
    }

    oneFrame([] { auto s = Profiler::scopef("a name the pool has no room for %d", 12345); });

    const rapidjson::Document doc = parseJson(Profiler::toJson());
    EXPECT_NE(findScope(doc, "<scopef pool full>"), nullptr)
        << "a name past the cap must collapse to one bucket rather than growing the pool forever";
}

// ================================================================== GPU zones

TEST_F(ProfilerTest, GpuZoneLandsInTheFrameItNames) {
    Profiler::init();

    oneFrame([] { auto s = Profiler::scope("CpuWork"); });

    Profiler::recordGpuZone(0, "GBuffer", 250.0, 1.25);

    const rapidjson::Document doc = parseJson(Profiler::toJson());
    EXPECT_EQ(doc["frame"].GetUint64(), 0u);

    const rapidjson::Value* zone = findScope(doc, "GBuffer");
    ASSERT_NE(zone, nullptr);
    EXPECT_DOUBLE_EQ((*zone)["cpuMs"].GetDouble(), 1.25);
    EXPECT_EQ((*zone)["depth"].GetUint(), 0u);
    // GPU zones go on their own Chrome Tracing row rather than a worker thread's.
    EXPECT_EQ((*zone)["threadId"].GetUint(), 1000u);
}

TEST_F(ProfilerTest, GpuZoneForAnEvictedFrameIsDroppedRatherThanMisattributed) {
    Profiler::init();

    oneFrame([] { auto s = Profiler::scope("CpuWork"); });
    const size_t before = scopeNames(parseJson(Profiler::toJson())).size();

    // GPU results arrive several frames late. One for a frame that has already aged out
    // must not be pinned onto whatever frame happens to be at hand.
    Profiler::recordGpuZone(9999, "Ghost", 0.0, 1.0);

    const rapidjson::Document doc = parseJson(Profiler::toJson());
    EXPECT_EQ(scopeNames(doc).size(), before);
    EXPECT_EQ(findScope(doc, "Ghost"), nullptr);
}

// ============================================================== JSON escaping

TEST_F(ProfilerTest, SpecialCharactersInANameStayValidJson) {
    Profiler::init();

    oneFrame([] {
        auto quoted = Profiler::scope("say \"hi\"");
        auto slashed = Profiler::scope("back\\slash");
        auto tabbed = Profiler::scope("with\ttab");
    });

    // parseJson asserts the document parses at all, which is the real check: an
    // unescaped quote makes the whole trace unreadable rather than one name wrong.
    const rapidjson::Document doc = parseJson(Profiler::toJson());
    EXPECT_NE(findScope(doc, "say \"hi\""), nullptr);
    EXPECT_NE(findScope(doc, "back\\slash"), nullptr);
    EXPECT_NE(findScope(doc, "with\ttab"), nullptr);
}

// =============================================================== file output

namespace {

/// Frame indices present in a Chrome trace file, in order of first appearance.
std::vector<uint64_t> tracedFrames(const fs::path& path) {
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    rapidjson::Document doc;
    doc.Parse(text.c_str());
    EXPECT_FALSE(doc.HasParseError()) << "trace file is not valid JSON: " << text;

    std::vector<uint64_t> frames;
    if (!doc.IsArray()) return frames;
    for (const auto& event : doc.GetArray()) {
        // `M` is the metadata phase -- `thread_name` -- and carries no frame. Only a
        // timed `X` event belongs to one.
        if (std::string(event["ph"].GetString()) != "X") continue;
        const uint64_t f = event["args"]["frame"].GetUint64();
        if (frames.empty() || frames.back() != f) frames.push_back(f);
    }
    return frames;
}

} // namespace

TEST_F(ProfilerTest, WriteToFileEmitsAValidChromeTrace) {
    Profiler::init();

    oneFrame([] { auto s = Profiler::scope("Traced"); });

    const fs::path out = dir / "trace.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));
    ASSERT_TRUE(fs::exists(out));

    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    rapidjson::Document doc;
    doc.Parse(text.c_str());
    ASSERT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc.IsArray());
    ASSERT_GT(doc.Size(), 0u);

    // The first event is a `thread_name` -- `Profiler::init` names its own thread -- so
    // this looks for the first *timed* one rather than assuming index 0.
    const rapidjson::Value* event = nullptr;
    for (const auto& e : doc.GetArray()) {
        if (std::string(e["ph"].GetString()) == "X") {
            event = &e;
            break;
        }
    }
    ASSERT_NE(event, nullptr);
    EXPECT_STREQ((*event)["cat"].GetString(), "cpu");
    EXPECT_TRUE(event->HasMember("ts"));
    EXPECT_TRUE(event->HasMember("dur"));
    EXPECT_TRUE((*event)["args"].HasMember("path"));
}

// ==================================================================== counters

namespace {

/// Every `ph:"C"` event in a trace file, as (name, value) in emitted order.
std::vector<std::pair<std::string, double>> tracedCounters(const fs::path& path) {
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    rapidjson::Document doc;
    doc.Parse(text.c_str());
    EXPECT_FALSE(doc.HasParseError()) << "counters broke the trace: " << text;

    std::vector<std::pair<std::string, double>> out;
    if (!doc.IsArray()) return out;
    for (const auto& e : doc.GetArray()) {
        if (std::string(e["ph"].GetString()) != "C") continue;
        const std::string name = e["name"].GetString();
        // The series key inside `args` is the counter's own name -- that is the shape
        // Perfetto renders as a track graph, and a mismatch between the two would draw an
        // empty track rather than fail to load.
        EXPECT_TRUE(e["args"].HasMember(name.c_str())) << "counter " << name << " has no series named for it";
        out.emplace_back(name, e["args"][name.c_str()].GetDouble());
    }
    return out;
}

} // namespace

TEST_F(ProfilerTest, ACounterWithNoFrameOpenIsDropped) {
    // A counter's whole meaning is "this was true during frame N". Buffering one recorded
    // outside a frame would attach it to whichever frame opened next, which is a number
    // reported against a frame it was not measured in.
    Profiler::init();
    Profiler::counter("Orphan", 7.0);

    oneFrame([] { auto s = Profiler::scope("Traced"); });

    const fs::path out = dir / "orphan.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));
    for (const auto& [name, value] : tracedCounters(out)) EXPECT_NE(name, "Orphan");
}

TEST_F(ProfilerTest, TwoWritesOfOneCounterInAFrameKeepTheLast) {
    // Last value per frame, not a stack: two writes are a caller correcting itself. That
    // is also what bounds the per-frame storage, which is what keeps recording
    // allocation-free.
    Profiler::init();

    oneFrame([] {
        Profiler::counter("Instances", 1.0);
        Profiler::counter("Instances", 2.0);
        Profiler::counter("Instances", 3.0);
    });

    const fs::path out = dir / "last.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));

    std::vector<double> values;
    for (const auto& [name, value] : tracedCounters(out)) {
        if (name == "Instances") values.push_back(value);
    }
    ASSERT_EQ(values.size(), 1u) << "three writes in one frame emitted " << values.size() << " events";
    EXPECT_DOUBLE_EQ(values[0], 3.0);
}

TEST_F(ProfilerTest, ADisabledProfilerRecordsNoCounters) {
    ProfilerConfig cfg;
    cfg.enabled = false;
    Profiler::init(cfg);

    oneFrame([] { Profiler::counter("NeverRecorded", 1.0); });

    // Read as text rather than parsed: a disabled profiler buffers no frames, so what
    // `writeToFile` leaves behind is empty rather than an empty JSON array, and asserting
    // on the parse would be asserting on that instead of on the counter.
    const fs::path out = dir / "disabled_counters.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("NeverRecorded"), std::string::npos);
    EXPECT_EQ(text.find("\"ph\":\"C\""), std::string::npos);
}

TEST_F(ProfilerTest, CountersSitOnTheSameTimelineAsTheScopesTheyExplain) {
    // The trap the card named: `writeTrace` emits no wall-clock time, it concatenates
    // frames by cumulative duration. A counter stamped from `steady_clock` at the call
    // site would draw a graph that does not sit above the zones it explains. Each frame's
    // counter must carry that frame's own base timestamp, so the values must be
    // non-decreasing across frames and must match the frame bases the scopes use.
    Profiler::init();
    for (int i = 0; i < 4; ++i) {
        auto frame = Profiler::beginFrame();
        Profiler::counter("Tick", static_cast<double>(i));
        auto s = Profiler::scope("Work");
    }
    { auto flush = Profiler::beginFrame(); }

    const fs::path out = dir / "timeline.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));

    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    rapidjson::Document doc;
    doc.Parse(text.c_str());
    ASSERT_FALSE(doc.HasParseError());

    // The counter for a frame and the earliest scope in it share a base, so a counter's
    // ts is never after the first zone of its own frame.
    std::map<uint64_t, double> counterTs, firstScopeTs;
    for (const auto& e : doc.GetArray()) {
        const std::string ph = e["ph"].GetString();
        if (ph == "C" && std::string(e["name"].GetString()) == "Tick") {
            counterTs[e["args"]["frame"].GetUint64()] = e["ts"].GetDouble();
        } else if (ph == "X") {
            const uint64_t f = e["args"]["frame"].GetUint64();
            const double ts = e["ts"].GetDouble();
            if (!firstScopeTs.count(f) || ts < firstScopeTs[f]) firstScopeTs[f] = ts;
        }
    }
    ASSERT_GE(counterTs.size(), 3u);

    double previous = -1.0;
    for (const auto& [frame, ts] : counterTs) {
        EXPECT_GT(ts, previous) << "frame " << frame << "'s counter is not after the previous frame's";
        previous = ts;
        if (firstScopeTs.count(frame)) {
            EXPECT_LE(ts, firstScopeTs[frame] + 1e-9)
                << "frame " << frame << "'s counter is stamped after the zones it explains";
        }
    }
}

TEST_F(ProfilerTest, ARecycledTrackIsRenamedRatherThanInheriting) {
    // The failure this exists to catch: slots are recycled, so one `tid` is the scene-load
    // worker early in a run and the recorder later. A name attached to the *slot* would
    // label the recorder's work with the loader's name, which is worse than no label --
    // an unlabelled track is unhelpful, a track labelled from a stale owner is wrong.
    Profiler::init();
    { auto frame = Profiler::beginFrame(); }

    std::thread first([] { Profiler::nameThread("first"); });
    first.join();
    // The slot is back in the pool here, so the next thread takes it.
    std::thread second([] { Profiler::nameThread("second"); });
    second.join();
    // And a thread that names itself nothing at all, which is the sharp case: it must not
    // come out labelled "second".
    std::thread anonymous([] { auto s = Profiler::scope("Anonymous"); });
    anonymous.join();

    { auto flush = Profiler::beginFrame(); }

    const fs::path out = dir / "threads.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    rapidjson::Document doc;
    doc.Parse(text.c_str());
    ASSERT_FALSE(doc.HasParseError()) << "metadata events broke the trace: " << text;
    ASSERT_TRUE(doc.IsArray());

    // Every name each track was given, in the order it was given, and which track the
    // anonymous thread's scope landed on. Keyed by tid rather than assumed, because which
    // slot the pool hands out depends on what every earlier test in this binary left in
    // it -- the registry outlives `shutdown()` on purpose.
    std::map<int, std::vector<std::string>> named;
    int anonymousTid = -1;
    for (const auto& e : doc.GetArray()) {
        const std::string ph = e["ph"].GetString();
        if (ph == "M") {
            EXPECT_STREQ(e["name"].GetString(), "thread_name");
            named[e["tid"].GetInt()].emplace_back(e["args"]["name"].GetString());
        } else if (ph == "X" && std::string(e["name"].GetString()) == "Anonymous") {
            anonymousTid = e["tid"].GetInt();
        }
    }

    // `Profiler::init` named its own track.
    bool sawMain = false;
    for (const auto& [tid, list] : named) sawMain = sawMain || list.back() == "main";
    EXPECT_TRUE(sawMain);

    const auto has = [](const std::vector<std::string>& v, const char* s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    bool sawFirst = false, sawSecond = false;
    for (const auto& [tid, list] : named) {
        sawFirst = sawFirst || has(list, "first");
        sawSecond = sawSecond || has(list, "second");
        // Where a slot carried both -- which is what happens wherever thread_local
        // destructors run, so everywhere but MinGW -- the order is the order the threads
        // ran in, and there are four events rather than one: two acquisitions and two
        // names.
        if (has(list, "first") && has(list, "second")) {
            EXPECT_LT(std::find(list.begin(), list.end(), "first"),
                      std::find(list.begin(), list.end(), "second"));
            EXPECT_GE(list.size(), 4u);
        }
    }
    EXPECT_TRUE(sawFirst);
    EXPECT_TRUE(sawSecond);

    // **The assertion the card exists for.** Whatever track the anonymous thread ended up
    // on, its last label is the default one -- it did not inherit whatever the previous
    // owner of that slot called itself.
    ASSERT_NE(anonymousTid, -1);
    ASSERT_FALSE(named[anonymousTid].empty());
    EXPECT_EQ(named[anonymousTid].back(), "thread " + std::to_string(anonymousTid))
        << "an unnamed thread inherited the previous owner's label";
}

TEST_F(ProfilerTest, InitialiseCreatesTheTraceDirectory) {
    // The default trace goes to debug_frames/, which existed only because run.sh made it.
    // A packaged build has no run.sh, and an ofstream onto a missing directory fails
    // without saying so -- so the trace would simply never appear.
    const fs::path nested = dir / "made" / "by" / "initialize";
    const fs::path out = nested / "profile.json";
    ASSERT_FALSE(fs::exists(nested));

    Profiler::init({.outputFile = out.string()});
    EXPECT_TRUE(fs::is_directory(nested));

    oneFrame([] { auto s = Profiler::scope("Traced"); });
    EXPECT_TRUE(Profiler::writeToFile(out.string()));
    EXPECT_TRUE(fs::exists(out));
}

TEST_F(ProfilerTest, WriteToFileReportsFailureOnAnUnopenablePath) {
    Profiler::init();
    oneFrame([] { auto s = Profiler::scope("Traced"); });

    EXPECT_FALSE(Profiler::writeToFile("/nonexistent-directory/trace.json"));
}

TEST_F(ProfilerTest, WriteToFileSucceedsTriviallyWithNothingBuffered) {
    Profiler::init();
    // Nothing to write is not a failure, and the caller has no way to tell the two
    // apart other than by this contract.
    EXPECT_TRUE(Profiler::writeToFile("/nonexistent-directory/trace.json"));
}

TEST_F(ProfilerTest, FrameZeroSurvivesTheRollingWindow) {
    // 0.5: startup, device init and asset load all happen inside frame 0 and never
    // happen again, so a plain FIFO window loses the most expensive work the process
    // ever does. Frame 1 is evicted instead.
    ProfilerConfig cfg;
    cfg.maxFrames = 3;
    Profiler::init(cfg);

    for (int i = 0; i < 10; ++i) {
        auto frame = Profiler::beginFrame();
        auto s = Profiler::scope("Work");
    }

    const fs::path out = dir / "window.json";
    ASSERT_TRUE(Profiler::writeToFile(out.string()));

    const std::vector<uint64_t> frames = tracedFrames(out);
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0], 0u) << "frame 0 must be pinned, not aged out";
    EXPECT_EQ(frames[1], 7u);
    EXPECT_EQ(frames[2], 8u);
}

TEST_F(ProfilerTest, FrameZeroSurvivesTheWriterQueueToo) {
    // The same pin, one layer down. pendingFrames is drained wholesale into writeQueue
    // on the first auto-flush, so without the second pin the startup frame survives its
    // own window only to be trimmed out of the one that reaches disk.
    const fs::path out = dir / "autoflush.json";

    ProfilerConfig cfg;
    cfg.maxFrames = 3;
    cfg.autoFlushFrames = 2;
    cfg.outputFile = out.string();
    Profiler::init(cfg);

    for (int i = 0; i < 10; ++i) {
        auto frame = Profiler::beginFrame();
        auto s = Profiler::scope("Work");
    }

    Profiler::shutdown(); // flushes and joins the writer thread
    ASSERT_TRUE(fs::exists(out));

    const std::vector<uint64_t> frames = tracedFrames(out);
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames[0], 0u) << "frame 0 must survive the writer queue's trim as well";
}

// ============================================================== thread safety

TEST_F(ProfilerTest, ScopesFromSeveralThreadsAllArriveOnTheirOwnTracks) {
    Profiler::init();

    {
        auto frame = Profiler::beginFrame();

        // Both scopes have to be *open at the same time* for the two threads to hold
        // distinct slots -- slots are recycled at thread exit, so a worker that finishes
        // before the next one starts legitimately gets the same id back. Sleeping is not
        // a synchronisation primitive; this handshake is.
        std::atomic<int> arrived{0};
        const auto worker = [&arrived](const char* name) {
            auto s = Profiler::scope(name);
            arrived.fetch_add(1, std::memory_order_release);
            while (arrived.load(std::memory_order_acquire) < 2) std::this_thread::yield();
        };

        std::thread a(worker, "WorkerA");
        std::thread b(worker, "WorkerB");
        a.join();
        b.join();
    }
    { auto flush = Profiler::beginFrame(); }

    const rapidjson::Document doc = parseJson(Profiler::toJson());
    const rapidjson::Value* wa = findScope(doc, "WorkerA");
    const rapidjson::Value* wb = findScope(doc, "WorkerB");
    ASSERT_NE(wa, nullptr);
    ASSERT_NE(wb, nullptr);
    EXPECT_NE((*wa)["threadId"].GetUint(), (*wb)["threadId"].GetUint());
}

TEST_F(ProfilerTest, ThreadSlotsAreRecycledRatherThanAccumulated) {
    // An engine that spawns short-lived jobs would otherwise grow the registry without
    // bound and render one near-empty row per thread ever created. Sequential threads
    // must therefore reuse the same slot id.
#ifdef _WIN32
    // Recycling is driven by `ThreadSlotGuard`, a thread_local whose destructor releases
    // the slot at thread exit. MinGW does not run thread_local destructors reliably for
    // threads that end normally -- statically linked winpthreads does not register them
    // through __cxa_thread_atexit -- so the slot is never marked free and the next thread
    // takes a new one. Observed as first=2, second=3 rather than both the same.
    //
    // Skipped rather than deleted, and recorded in docs/architecture/limitations.md: the
    // property is still required on Linux, where it is checked, and the Windows cost is a
    // registry that grows with each thread rather than anything incorrect. A real fix is
    // FlsAlloc with a destructor callback, which does run, and is not worth opening the
    // profiler for until a Windows build actually spawns job threads.
    GTEST_SKIP() << "MinGW does not run thread_local destructors at thread exit";
#endif
    Profiler::init();

    const auto threadIdFor = [](const char* name) -> uint32_t {
        {
            auto frame = Profiler::beginFrame();
            std::thread t([name] { auto s = Profiler::scope(name); });
            t.join();
        }
        { auto flush = Profiler::beginFrame(); }
        const rapidjson::Document doc = parseJson(Profiler::toJson());
        const rapidjson::Value* s = findScope(doc, name);
        return s ? (*s)["threadId"].GetUint() : 0u;
    };

    const uint32_t first = threadIdFor("First");
    const uint32_t second = threadIdFor("Second");

    EXPECT_NE(first, 0u);
    EXPECT_EQ(first, second);
}

TEST_F(ProfilerTest, ConcurrentRecordingWhileFramesFlipDoesNotLoseOrCorruptScopes) {
    // The shape the three hand-found concurrency bugs had: workers recording while the
    // main thread flips the frame underneath them. Assertion is deliberately weak --
    // scopes straddling a boundary land in either frame -- because the value here is
    // running it under ThreadSanitizer, which the renderer cannot do at all.
    Profiler::init();

    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto outer = Profiler::scope("Worker");
                auto inner = Profiler::scope("WorkerInner");
            }
        });
    }

    for (int i = 0; i < 200; ++i) {
        auto frame = Profiler::beginFrame();
        auto s = Profiler::scope("Main");
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : workers) t.join();

    { auto flush = Profiler::beginFrame(); }
    EXPECT_NE(Profiler::toJson(), "{}");
}

// ======================================================================= dump

TEST_F(ProfilerTest, DumpIsSafeWithAndWithoutData) {
    Profiler::init();
    Profiler::dump(); // empty window

    oneFrame([] { auto s = Profiler::scope("Dumped"); });

    testing::internal::CaptureStdout();
    Profiler::dump();
    const std::string out = testing::internal::GetCapturedStdout();

    EXPECT_NE(out.find("Dumped"), std::string::npos);
}

// `ProfilerConfig` is the only spelling of the profiler's configuration now: the six
// `profiler.*` rows are gone, and `Config::profiler` *is* one of these, so there are no
// longer two tables to agree. What replaced D6's agreement test is the assertion that the
// divergence D6 documented has an owner -- the struct's own `outputFile` is empty, because
// *"write nothing to disk"* is the right answer for a caller that did not ask for a trace,
// and the engine states its own path in `Config.h` where `--trace` can override it.
TEST(ProfilerConfigDefaults, WriteNothingUnlessAskedTo) {
    const ProfilerConfig cfg;

    EXPECT_TRUE(cfg.outputFile.empty()) << "constructing a ProfilerConfig must not name a path that gets written to";
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.averagingWindow, 60u);
    EXPECT_EQ(cfg.maxFrames, 240u);
    EXPECT_EQ(cfg.autoFlushFrames, 120u);
    EXPECT_FALSE(cfg.clearAfterFlush);

    // The engine's own choice, and the point is that it is *one* place rather than a
    // settings default a directly-constructed config disagreed with.
    const Config engine;
    EXPECT_EQ(engine.profiler.outputFile, "debug_frames/profile.json");
    EXPECT_EQ(engine.profiler.enabled, cfg.enabled);
    EXPECT_EQ(engine.profiler.maxFrames, cfg.maxFrames);
}
