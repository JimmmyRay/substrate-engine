#include "core/Recorder.h"

#include "core/AudioTap.h"
#include "core/Logger.h"
#include "core/Profiler.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace core {

namespace {

/// Wrap `text` for /bin/sh. Every string reaching a command line here has to go through
/// this: "a path this program chose" includes a home directory with an apostrophe in it,
/// and the failure mode is a shell running something nobody wrote.
std::string shellQuote(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

std::string shellQuote(const std::filesystem::path& path) { return shellQuote(path.string()); }

/// True when `tool` runs. `command -v` rather than searching PATH by hand, so a shell
/// function or alias resolves the same way the `popen`ed pipes below will resolve it.
bool onPath(const char* tool) {
    const std::string probe = std::string("command -v ") + tool + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

} // namespace

bool recordingToolsAvailable() { return onPath("ffmpeg"); }

Recorder::~Recorder() {
    if (started) stop();
}

bool Recorder::start(Options options, AudioTap* audio) {
    if (running.load(std::memory_order_acquire)) {
        Logger::warn(LogCategory::Render, "Record: already recording");
        return false;
    }
    // A session whose encoder died cleared `running` from the worker thread and left the
    // thread and the pipes behind. Reclaim them first: `popen` over a live `FILE*` leaks
    // it, and assigning to a joinable `std::thread` is a `std::terminate`.
    if (started) stop();

    opt = std::move(options);
    tap = audio;

    if (opt.width == 0 || opt.height == 0) {
        Logger::error(LogCategory::Render, "Record: refusing a %ux%u frame", opt.width, opt.height);
        return false;
    }
    // yuv420p subsamples chroma by two and cannot represent an odd dimension. Without this
    // check libx264 starts, prints nothing useful and produces no file.
    if (opt.width % 2 != 0 || opt.height % 2 != 0) {
        Logger::error(LogCategory::Render,
                      "Record: %ux%u has an odd dimension, which yuv420p cannot encode. Resize the window.",
                      opt.width, opt.height);
        return false;
    }
    if (opt.fps == 0) opt.fps = 30;
    if (!recordingToolsAvailable()) {
        Logger::error(LogCategory::Render, "Record: ffmpeg is not on PATH");
        return false;
    }

    std::error_code ec;
    if (opt.path.has_parent_path()) std::filesystem::create_directories(opt.path.parent_path(), ec);

    // Beside the output rather than in a temporary directory, so a failed mux leaves the
    // two halves where the person looking for them will look.
    videoIntermediate = opt.path;
    videoIntermediate += ".video.mp4";
    audioIntermediate = opt.path;
    audioIntermediate += ".audio.wav";

    // A write to a pipe whose reader has died raises SIGPIPE, which by default kills the
    // process -- an encoder that falls over would take the session with it. Ignored, so it
    // surfaces as the write error the `fwrite` checks already look for.
    std::signal(SIGPIPE, SIG_IGN);

    // `-preset ultrafast` because this runs alongside the game: a slower preset costs the
    // frame rate of the thing being recorded, where a bigger intermediate costs disk.
    // `-g fps` puts a keyframe every second, which is what lets `stop()` trim to the window
    // with a stream copy instead of a re-encode.
    const std::string videoCmd =
        "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format " + shellQuote(opt.pixelFormat) +
        " -video_size " + std::to_string(opt.width) + "x" + std::to_string(opt.height) + " -framerate " +
        std::to_string(opt.fps) + " -i pipe:0 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g " +
        std::to_string(opt.fps) + " " + shellQuote(videoIntermediate);

    videoPipe = popen(videoCmd.c_str(), "w");
    if (videoPipe == nullptr) {
        Logger::error(LogCategory::Render, "Record: could not start the video encoder");
        return false;
    }
    // Set the moment there is something to give back, whatever happens to the session.
    started = true;

    if (tap != nullptr) {
        // Uncompressed: the mux re-encodes it anyway, so compressing here is work spent
        // twice.
        const std::string audioCmd = "ffmpeg -hide_banner -loglevel error -y -f f32le -ar " +
                                     std::to_string(opt.sampleRate) + " -ac " + std::to_string(opt.channels) +
                                     " -i pipe:0 -c:a pcm_s16le " + shellQuote(audioIntermediate);
        audioPipe = popen(audioCmd.c_str(), "w");
        if (audioPipe == nullptr) {
            Logger::warn(LogCategory::Render, "Record: could not start the audio encoder; recording will be silent");
            tap = nullptr;
        }
    }

    queue.clear();
    spare.clear();
    carriedRepeat = 0;
    reserved = 0;
    silenceWritten = 0;
    submitted.store(0, std::memory_order_relaxed);
    dropped.store(0, std::memory_order_relaxed);
    written.store(0, std::memory_order_relaxed);

    running.store(true, std::memory_order_release);
    worker = std::thread(&Recorder::encodeLoop, this);

    Logger::status(LogCategory::Render, "Record: %ux%u at %u fps, keeping the last %.0f s, to %s", opt.width,
                   opt.height, opt.fps, opt.windowSeconds, opt.path.string().c_str());
    return true;
}

uint32_t framesOwedAt(double elapsedSeconds, uint32_t fps, uint64_t delivered) {
    if (elapsedSeconds <= 0.0 || fps == 0) return 0;

    // Where the file *should* be, from the wall clock alone. Accumulating per-frame deltas
    // instead lets rounding errors add up into drift; here the next call corrects them.
    const uint64_t target = static_cast<uint64_t>(elapsedSeconds * static_cast<double>(fps));
    if (target <= delivered) return 0;

    uint64_t owed = target - delivered;
    // A hitch can leave whole seconds owed, and paying that in full writes seconds of one
    // frozen image. The cap drops those frames from the file instead.
    if (owed > kMaxFrameRepeat) owed = kMaxFrameRepeat;
    return static_cast<uint32_t>(owed);
}

uint32_t Recorder::framesOwed(double elapsedSeconds) {
    if (!running.load(std::memory_order_acquire)) return 0;

    const uint32_t owed = framesOwedAt(elapsedSeconds, opt.fps, reserved);
    // Advanced past what the cap discarded, not just past what was handed out: otherwise a
    // hitch leaves a debt the next call pays and the file runs long.
    if (owed > 0) reserved = static_cast<uint64_t>(elapsedSeconds * static_cast<double>(opt.fps));
    return owed;
}

void Recorder::submitFrame(const void* pixels, size_t bytes, uint32_t repeat) {
    if (!running.load(std::memory_order_acquire) || pixels == nullptr || bytes == 0) return;

    const uint32_t total = repeat + carriedRepeat;
    if (total == 0) return;

    std::vector<uint8_t> buffer;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (queue.size() >= queueLimit) {
            // The encoder is behind. Carrying what this frame owed onto the next one that
            // fits is what keeps the file the right length.
            carriedRepeat = total;
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!spare.empty()) {
            buffer = std::move(spare.back());
            spare.pop_back();
        }
    }

    buffer.resize(bytes);
    std::memcpy(buffer.data(), pixels, bytes);

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back(QueuedFrame{std::move(buffer), total});
    }
    carriedRepeat = 0;
    submitted.fetch_add(1, std::memory_order_relaxed);
    queueSignal.notify_one();
}

bool Recorder::pumpVideo() {
    QueuedFrame frame;
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        if (queue.empty()) return false;
        frame = std::move(queue.front());
        queue.pop_front();
    }

    if (videoPipe != nullptr) {
        for (uint32_t i = 0; i < frame.repeat; ++i) {
            if (std::fwrite(frame.pixels.data(), 1, frame.pixels.size(), videoPipe) != frame.pixels.size()) {
                Logger::error(LogCategory::Render, "Record: the video encoder stopped reading");
                running.store(false, std::memory_order_release);
                break;
            }
            written.fetch_add(1, std::memory_order_relaxed);
        }
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // Back to the pool rather than freed: at 1600x900 this is 5.8 MB, and allocating
        // and releasing that thirty times a second fragments the heap.
        frame.pixels.clear();
        spare.push_back(std::move(frame.pixels));
    }
    return true;
}

void Recorder::pumpAudio() {
    if (tap == nullptr || audioPipe == nullptr || !tap->active()) return;

    const uint32_t channels = tap->channels();
    if (channels == 0) return;

    // Whatever the ring could not fit, replaced by exactly that much silence, and written
    // *before* the waiting samples: the drop happened while the reader was away, so the gap
    // belongs at the older end of what is being read now.
    const uint64_t lost = tap->dropped();
    if (lost > silenceWritten) {
        const uint64_t gap = lost - silenceWritten;
        static const std::vector<float> silence(1024, 0.0f);
        uint64_t remaining = gap * channels;
        while (remaining > 0) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, silence.size()));
            if (std::fwrite(silence.data(), sizeof(float), chunk, audioPipe) != chunk) break;
            remaining -= chunk;
        }
        silenceWritten = lost;
    }

    constexpr uint64_t kDrainFrames = 4096;
    if (audioScratch.size() < kDrainFrames * channels) audioScratch.resize(kDrainFrames * channels);

    while (true) {
        const uint64_t got = tap->read(audioScratch.data(), kDrainFrames);
        if (got == 0) break;
        const size_t samples = static_cast<size_t>(got) * channels;
        if (std::fwrite(audioScratch.data(), sizeof(float), samples, audioPipe) != samples) {
            Logger::error(LogCategory::Render, "Record: the audio encoder stopped reading");
            break;
        }
    }
}

void Recorder::encodeLoop() {
    core::Profiler::nameThread("recorder encode");
    while (running.load(std::memory_order_acquire)) {
        const bool didWork = pumpVideo();
        pumpAudio();
        if (didWork) continue;

        // Waiting on the queue rather than sleeping keeps a submitted frame from sitting
        // there; the timeout is what keeps audio flowing when the video has nothing.
        std::unique_lock<std::mutex> lock(queueMutex);
        queueSignal.wait_for(lock, std::chrono::milliseconds(5), [this] { return !queue.empty(); });
    }

    // The tail: `running` goes false with frames still queued, and dropping them cuts the
    // last fraction of a second off the recording.
    while (pumpVideo()) {
    }
    pumpAudio();
}

void Recorder::closePipes() {
    if (videoPipe != nullptr) {
        pclose(videoPipe);
        videoPipe = nullptr;
    }
    if (audioPipe != nullptr) {
        pclose(audioPipe);
        audioPipe = nullptr;
    }
}

bool Recorder::muxOutput() {
    std::error_code ec;
    if (!std::filesystem::exists(videoIntermediate, ec)) {
        Logger::error(LogCategory::Render, "Record: the encoder wrote no video");
        return false;
    }

    // `-sseof` seeks backwards from the end of each input, which is what makes the window
    // the *last* N seconds. It must stay before `-i`, per input: after it, it is a filter
    // rather than a seek and the trim costs a re-encode. A window longer than the recording
    // clamps to the start.
    std::string trim;
    if (opt.windowSeconds > 0.0) trim = "-sseof -" + std::to_string(opt.windowSeconds) + " ";

    std::string cmd = "ffmpeg -hide_banner -loglevel error -y " + trim + "-i " + shellQuote(videoIntermediate);
    const bool haveAudio = std::filesystem::exists(audioIntermediate, ec) &&
                           std::filesystem::file_size(audioIntermediate, ec) > 1024;
    if (haveAudio) cmd += " " + trim + "-i " + shellQuote(audioIntermediate);

    // `-c:v copy`: the video is already h264, so re-encoding doubles the cost for no change
    // to the picture. The audio is not already aac and is encoded once, here.
    cmd += " -c:v copy";
    if (haveAudio) cmd += " -c:a aac -b:a 192k -shortest";
    cmd += " " + shellQuote(opt.path);

    if (std::system(cmd.c_str()) != 0) {
        Logger::error(LogCategory::Render, "Record: muxing failed. The halves are at %s and %s",
                      videoIntermediate.string().c_str(), audioIntermediate.string().c_str());
        return false;
    }
    return true;
}

std::filesystem::path Recorder::stop() {
    // Keyed on `started`, never on `running`: the worker clears `running` by itself when
    // the encoder stops reading, so gating teardown on it skips the join and the `pclose`
    // and leaves a joinable thread, two leaked pipes and a zombie ffmpeg behind.
    if (!started) return {};
    started = false;

    running.store(false, std::memory_order_release);
    queueSignal.notify_all();
    if (worker.joinable()) worker.join();

    // After the join, never before: closing a pipe the worker is mid-`fwrite` on is a write
    // to a closed stream, and `pclose` would wait on an encoder still being fed.
    closePipes();

    const uint64_t drops = framesDropped();
    if (drops > 0) {
        Logger::warn(LogCategory::Render, "Record: the encoder could not keep up with %llu frames",
                     static_cast<unsigned long long>(drops));
    }

    std::filesystem::path result;
    if (muxOutput()) {
        result = opt.path;
        std::error_code ec;
        std::filesystem::remove(videoIntermediate, ec);
        std::filesystem::remove(audioIntermediate, ec);
        Logger::status(LogCategory::Render, "Record: wrote %s (%llu frames)", result.string().c_str(),
                       static_cast<unsigned long long>(framesWritten()));
    }

    tap = nullptr;
    return result;
}

} // namespace core
