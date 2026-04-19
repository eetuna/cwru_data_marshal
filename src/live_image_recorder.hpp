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

    // Pre-fix (MEDIUM #15): bounded queue, dropped oldest droppable
    // pending lambda on overflow. That violated the lossless-retention
    // contract. Post-fix (2026-04-19): queue is unbounded, live
    // history never drops. dropped_count() and had_overflow() are
    // retained for backwards compat; they stay zero under normal ops.
    bool had_overflow() const noexcept { return drop_logged_.load(); }
    uint64_t dropped_count() const noexcept { return dropped_count_.load(); }

    // Snapshot of the per-message-kind counters from the active sink. Returns
    // zeros when the lane has not yet opened a sink for the current scan.
    struct CounterSnapshot {
        uint32_t acq{0};
        uint32_t img{0};
        uint32_t wf{0};
        bool sink_open{false};
        // Runtime queue depth. Useful on /debug/sinks to see if the
        // live path is falling behind. Expected to stay near 0 under
        // the observed ~65 records/s live rate vs the ~1000/s HDF5
        // append ceiling.
        uint64_t queued_jobs{0};
        // Sticky high-watermark signal. Set (once) when queued_jobs
        // crosses kHighWatermarkJobs; does NOT gate any drop.
        bool high_watermark_hit{false};
    };
    CounterSnapshot counters() const;

private:
    // Non-blocking watermark threshold. If the queue grows past this
    // without being drained, log a single warning and surface it via
    // /debug/sinks. No drop. If the live path is healthy at ~65
    // records/s this watermark is orders of magnitude above normal.
    static constexpr size_t kHighWatermarkJobs = 10000;

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
    // High-watermark surface (see kHighWatermarkJobs). Sticky: set
    // once per scan, reset at close_scan_on_worker.
    std::atomic<bool> high_watermark_hit_{false};

    std::string current_filename_;
    std::unique_ptr<MrdSink> sink_;

    void enqueue(std::function<void()> fn, bool droppable = true);
    void worker_loop();
    void close_scan_on_worker();
    void ensure_sink_on_worker(const std::string& filename, const std::string& xml);
};

} // namespace mrd
