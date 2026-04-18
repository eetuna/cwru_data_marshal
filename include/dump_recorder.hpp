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

    // Post-hoc accessors (retained). Callers that want fine-grained runtime
    // signal use the DumpEnqueueResult return values above; these give the
    // cumulative view for CLOSE-time logging.
    bool had_overflow() const noexcept { return drop_logged_.load(); }
    uint64_t dropped_record_count() const noexcept {
        return dropped_records_.load();
    }
    uint64_t dropped_byte_count() const noexcept {
        return dropped_bytes_.load();
    }

private:
    struct Job {
        size_t bytes{0};
        std::function<void()> fn;
    };

    std::filesystem::path dump_dir_;
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    size_t queued_bytes_{0};
    bool stopping_{false};
    std::atomic<bool> drop_logged_{false};
    std::atomic<uint64_t> dropped_records_{0};
    std::atomic<uint64_t> dropped_bytes_{0};

    std::string current_filename_;
    std::string current_xml_;
    std::unique_ptr<MrdSink> scanner_sink_;
    std::unique_ptr<MrdSink> recon_sink_;
    uint32_t scanner_text_count_{0};
    uint32_t recon_text_count_{0};

    static constexpr size_t kMaxQueuedBytes = 256ULL * 1024ULL * 1024ULL;
    static constexpr size_t kMaxQueuedJobs = 4096;

    DumpEnqueueResult enqueue(size_t bytes, std::function<void()> fn);
    void worker_loop();
    void close_scan_on_worker();
    void ensure_recon_sink_on_worker();
    void write_status_on_worker(MrdSink* sink);
};

} // namespace mrd
