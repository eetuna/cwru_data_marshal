/*
 * File: include/dump_recorder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async canonical ISMRMRD H5 dump recorder.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
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

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "mrd_sink.hpp"

namespace mrd {

// HIGH #10: enqueue-time backpressure signal. Every public append_* and
// set_* method returns this so the caller sees drops at the moment they
// happen rather than only via post-hoc accessors at CLOSE.
enum class DumpEnqueueResult {
    Accepted,    // job is queued
    Dropped,     // queue overflow: caller may throttle / escalate
    Stopped,     // recorder is shutting down; no further writes accepted
};

class DumpRecorder {
public:
    explicit DumpRecorder(std::filesystem::path dump_dir);
    ~DumpRecorder();

    DumpRecorder(const DumpRecorder&) = delete;
    DumpRecorder& operator=(const DumpRecorder&) = delete;

    DumpEnqueueResult start_scan(std::string filename, std::string xml);
    void close_scan();

    DumpEnqueueResult set_scanner_config_file(std::string config);
    DumpEnqueueResult set_scanner_config_text(std::string config);
    DumpEnqueueResult append_scanner_text(std::string text);
    DumpEnqueueResult append_recon_text(std::string text);

    DumpEnqueueResult append_scanner_acquisition(const ISMRMRD::AcquisitionHeader& hdr,
                                                 std::vector<uint8_t> traj,
                                                 std::vector<uint8_t> samples);
    DumpEnqueueResult append_scanner_image(const ISMRMRD::ImageHeader& hdr,
                                           std::vector<uint8_t> attr,
                                           std::vector<uint8_t> pixels);
    DumpEnqueueResult append_scanner_waveform(const ISMRMRD::WaveformHeader& hdr,
                                              std::vector<uint8_t> data);

    DumpEnqueueResult append_recon_image(std::vector<uint8_t> body);
    DumpEnqueueResult append_recon_waveform(std::vector<uint8_t> body);

    // Post-hoc accessors. The CLOSE-time view sums both lanes.
    bool had_overflow() const noexcept;
    uint64_t dropped_record_count() const noexcept;
    uint64_t dropped_byte_count() const noexcept;

    // Snapshot of per-sink message counters for /debug/sinks.
    struct SinkCounters {
        uint32_t acq{0};
        uint32_t img{0};
        uint32_t wf{0};
        bool open{false};
    };
    struct CounterSnapshot {
        SinkCounters scanner;
        SinkCounters recon;
        uint64_t dropped_records{0};
        uint64_t dropped_bytes{0};
        bool had_overflow{false};
    };
    CounterSnapshot counters() const;

private:
    // P5: per-lane queue + worker. Scanner-side and recon-side traffic
    // ran through one shared worker pre-fix; a slow HDF5 write to one
    // sink blocked all writes to the other. Each lane now owns its
    // queue, mutex, cv, worker, sink, and drop counters.
    struct Lane {
        // Job lifecycle. Barrier jobs (start_scan side-effects, close_scan)
        // are non-droppable; data jobs are droppable so the overflow path
        // can shed oldest-first without ever evicting a barrier.
        struct Job {
            size_t bytes{0};
            std::function<void()> fn;
            bool droppable{true};
        };

        std::filesystem::path lane_dir;          // directory under dump_dir
        std::string component_tag;               // for log messages

        std::thread worker;
        mutable std::mutex mtx;
        std::condition_variable cv;
        std::deque<Job> queue;
        size_t queued_bytes{0};
        bool stopping{false};

        std::atomic<bool> drop_logged{false};
        std::atomic<uint64_t> dropped_records{0};
        std::atomic<uint64_t> dropped_bytes{0};

        std::string current_filename;
        std::string current_xml;
        std::unique_ptr<MrdSink> sink;
        uint32_t text_count{0};

        // Snapshot of the most-recently-closed sink's counts so /debug/sinks
        // can report final retention after close, when sink is null. Updated
        // in close_scan_on_worker() before sink.reset().
        uint32_t last_closed_acq{0};
        uint32_t last_closed_img{0};
        uint32_t last_closed_wf{0};
        bool ever_closed{false};
    };

    std::filesystem::path dump_dir_;
    Lane scanner_;
    Lane recon_;

    static constexpr size_t kMaxQueuedBytes = 256ULL * 1024ULL * 1024ULL;
    static constexpr size_t kMaxQueuedJobs = 4096;

    void start_lane(Lane& lane, std::filesystem::path lane_dir, std::string tag);
    void stop_lane(Lane& lane);
    DumpEnqueueResult enqueue_scanner(size_t bytes, std::function<void()> fn);
    DumpEnqueueResult enqueue_recon(size_t bytes, std::function<void()> fn);
    DumpEnqueueResult enqueue_on(Lane& lane, size_t bytes,
                                  std::function<void()> fn, bool droppable);
    void worker_loop(Lane& lane);
    void close_scan_on_worker(Lane& lane);
    void write_status_on_worker(MrdSink* sink, const Lane& lane);
};

} // namespace mrd
