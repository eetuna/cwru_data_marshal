/*
 * File: src/live_image_recorder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async per-lane live image+waveform history recorder.
 *
 * 2026-04-19 (round-7): architecture unified with dump. The worker
 * now writes raw MRD wire-frame records to a flat .spool file and,
 * at close_scan time, runs a convert-to-HDF5 pass to produce the
 * canonical live per-scan HDF5 artifact (scan stem TS). The
 * mid-scan reader interface is latest_image.h5 (written by
 * LatestImageWriter with atomic closed-file snapshots); per-scan
 * live HDF5 is archival and finalized after close. This eliminates
 * the per-record HDF5 append ceiling that previously required an
 * unbounded in-memory queue, and closes the memory-growth failure
 * mode without any drop-oldest path.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spool_writer.hpp"

namespace mrd {

class LiveImageRecorder {
public:
    explicit LiveImageRecorder(std::filesystem::path lane_dir);
    ~LiveImageRecorder();

    LiveImageRecorder(const LiveImageRecorder&) = delete;
    LiveImageRecorder& operator=(const LiveImageRecorder&) = delete;

    // API surface unchanged from callers' perspective. filename and xml
    // are kept as the scan-identity / header used at convert time.
    // body is the raw MRD wire body that the listener already has on
    // hand, stored verbatim in the spool.
    void append_image(std::string filename,
                      std::string xml,
                      std::vector<uint8_t> body);
    void append_waveform(std::string filename,
                         std::string xml,
                         std::vector<uint8_t> body);
    void close_scan();

    // Retained for backwards compat. With the spool design, drops
    // only happen on disk-write failure (not queue pressure).
    bool had_overflow() const noexcept { return drop_logged_.load(); }
    uint64_t dropped_count() const noexcept { return dropped_count_.load(); }

    struct CounterSnapshot {
        // Converted HDF5 counts, populated by close_scan's converter.
        uint32_t acq{0};
        uint32_t img{0};
        uint32_t wf{0};
        // True while a spool is open for the current scan.
        bool sink_open{false};
        // Current queue depth (records waiting for the worker).
        uint64_t queued_jobs{0};
        // Sticky high-watermark signal on the queue.
        bool high_watermark_hit{false};
        // Spool records written so far (or final if closed).
        uint64_t spool_records{0};
        uint64_t spool_bytes{0};
    };
    CounterSnapshot counters() const;

private:
    // The queue only exists to decouple the TCP reader thread from the
    // spool fwrite call. Spool writes run at disk bandwidth, so the
    // queue is never deep in practice; the watermark is purely
    // diagnostic.
    static constexpr size_t kHighWatermarkJobs = 10000;

    struct Record {
        uint16_t tag{0};
        std::vector<uint8_t> body;
        // Non-droppable barrier used by close_scan to synchronously
        // drive flush + convert on the worker thread.
        bool barrier{false};
        std::shared_ptr<std::promise<void>> signal;
    };

    std::filesystem::path lane_dir_;
    std::string component_tag_;

    std::thread worker_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Record> queue_;
    bool stopping_{false};

    std::atomic<bool> drop_logged_{false};
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<bool> high_watermark_hit_{false};

    // Spool for the current scan (worker-owned).
    std::unique_ptr<SpoolWriter> spool_;
    std::string current_filename_;
    std::string current_xml_;
    bool xml_spooled_{false};   // worker-owned

    // Atomics published by the worker for HTTP-thread reads.
    std::atomic<uint32_t> pub_acq_count_{0};
    std::atomic<uint32_t> pub_img_count_{0};
    std::atomic<uint32_t> pub_wf_count_{0};
    std::atomic<bool>     pub_sink_open_{false};
    std::atomic<uint64_t> pub_spool_records_{0};
    std::atomic<uint64_t> pub_spool_bytes_{0};

    void enqueue_record(uint16_t tag, std::string filename, std::string xml,
                        std::vector<uint8_t> body);
    void worker_loop();
    void ensure_spool_on_worker();
    void close_and_convert_on_worker();
    void spool_xml_once_on_worker();
};

} // namespace mrd
