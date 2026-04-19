/*
 * File: src/live_image_recorder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async per-lane live image history recorder.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mrd {

class MrdSink;

class LiveImageRecorder {
public:
    explicit LiveImageRecorder(std::filesystem::path lane_dir);
    ~LiveImageRecorder();

    LiveImageRecorder(const LiveImageRecorder&) = delete;
    LiveImageRecorder& operator=(const LiveImageRecorder&) = delete;

    void append_image(std::string filename,
                      std::string xml,
                      std::vector<uint8_t> body);
    // ECG and other scanner waveforms are persisted via the same per-lane
    // sink so live mode captures them alongside images. Body is the wire
    // payload: WaveformHeader (40 B) + uint32 samples * channels.
    void append_waveform(std::string filename,
                         std::string xml,
                         std::vector<uint8_t> body);
    void close_scan();

    // MEDIUM #15: runtime-visible backpressure on unbounded queue.
    bool had_overflow() const noexcept { return drop_logged_.load(); }
    uint64_t dropped_count() const noexcept { return dropped_count_.load(); }

    // Snapshot of the per-message-kind counters from the active sink. Returns
    // zeros when the lane has not yet opened a sink for the current scan.
    struct CounterSnapshot {
        uint32_t acq{0};
        uint32_t img{0};
        uint32_t wf{0};
        bool sink_open{false};
    };
    CounterSnapshot counters() const;

private:
    // MEDIUM #15: bound the queue. Live archive writes cannot be coalesced
    // (each is a distinct append), so on overflow we drop the oldest
    // pending lambda. The close_scan job is NEVER dropped.
    static constexpr size_t kMaxQueuedJobs = 4096;

    struct Job {
        std::function<void()> fn;
        bool droppable{true};
    };

    std::filesystem::path lane_dir_;
    std::thread worker_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stopping_{false};
    std::atomic<bool> drop_logged_{false};
    std::atomic<uint64_t> dropped_count_{0};

    std::string current_filename_;
    std::unique_ptr<MrdSink> sink_;

    void enqueue(std::function<void()> fn, bool droppable = true);
    void worker_loop();
    void close_scan_on_worker();
    void ensure_sink_on_worker(const std::string& filename, const std::string& xml);
};

} // namespace mrd
