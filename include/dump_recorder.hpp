/*
 * File: include/dump_recorder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async raw-MRD spool recorder for dump mode.
 *
 * Hot path: reader thread enqueues a lightweight job; per-lane worker
 * thread pops the job and writes the wire bytes to a flat append-only
 * spool file (see SpoolWriter). On close_scan the spool is converted
 * to the canonical ISMRMRD HDF5 file (see SpoolConverter).
 *
 * The live HDF5 append path that used to run here per-record was a
 * ~1000/s ceiling; scanner at 50 Hz x 5 slices x 128 lines pushes
 * ~32,000/s. Spool write is ~disk-bandwidth bound, so the worker
 * keeps up with the scanner and the dropped-records path does not
 * trigger under normal operation. The contract requires no drops.
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

#include "spool_writer.hpp"

namespace mrd {

enum class DumpEnqueueResult {
    Accepted,    // job is queued
    Dropped,     // spool write failed / disk error (hard error signal)
    Stopped,     // recorder is shutting down; no further writes accepted
};

class DumpRecorder {
public:
    // delete_spool_after_convert controls cleanup policy:
    //   false (default) -- keep the .spool file next to the .h5 file
    //     after successful conversion for forensic recovery.
    //   true -- delete the .spool after the converter returns ok().
    explicit DumpRecorder(std::filesystem::path dump_dir,
                          bool delete_spool_after_convert = false);
    ~DumpRecorder();

    DumpRecorder(const DumpRecorder&) = delete;
    DumpRecorder& operator=(const DumpRecorder&) = delete;

    // start_scan sets the filename and XML header that later records
    // will reference. If spool records already arrived before this
    // call (CONFIG_FILE, CONFIG_TEXT, TEXT) they are already captured
    // in the spool; METADATA_XML is just another record and the
    // converter handles ordering.
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

    // Post-hoc accessors. With spool, dropped_records is only nonzero
    // on disk-write failure (enq_fail / spool_error). Under healthy
    // operation both are zero.
    bool had_overflow() const noexcept;
    uint64_t dropped_record_count() const noexcept;
    uint64_t dropped_byte_count() const noexcept;

    enum class ConversionStatus {
        Idle,        // no scan seen
        Spooling,    // scan in progress
        Converting,  // close_scan underway
        Complete,    // last scan converted successfully
        Failed,      // last scan conversion failed
    };

    struct LaneSnapshot {
        uint64_t spool_records{0};
        uint64_t spool_bytes{0};
        bool spool_open{false};
        std::string write_error;
        // Post-conversion counters (0 until ConversionStatus::Complete).
        uint32_t converted_acq{0};
        uint32_t converted_img{0};
        uint32_t converted_wf{0};
        bool conversion_ok{false};
    };
    struct CounterSnapshot {
        LaneSnapshot scanner;
        LaneSnapshot recon;
        uint64_t dropped_records{0};
        uint64_t dropped_bytes{0};
        bool had_overflow{false};
        ConversionStatus status{ConversionStatus::Idle};
    };
    CounterSnapshot counters() const;

private:
    struct Lane {
        // Queue entry captures wire bytes to write to the spool. The
        // reader thread builds this; worker pops and calls SpoolWriter.
        //
        // A Record with barrier=true is a synchronization signal: the
        // worker flushes/closes the current spool and calls
        // barrier->set_value() instead of writing. This is used by
        // close_scan to drain the queue WITHOUT setting stopping=true
        // (which would reject concurrent enqueues from other threads).
        struct Record {
            uint16_t tag{0};
            std::vector<uint8_t> body;
            bool barrier{false};
            std::shared_ptr<std::promise<void>> signal;
        };

        std::filesystem::path lane_dir;   // directory under dump_dir
        std::string component_tag;        // "scanner" or "recon" for logs

        std::thread worker;
        mutable std::mutex mtx;
        std::condition_variable cv;
        std::deque<Record> queue;
        bool stopping{false};

        // Hard-error counters (spool write failed). Normal queue
        // pressure does NOT increment these; contract says no drops.
        std::atomic<bool> drop_logged{false};
        std::atomic<uint64_t> dropped_records{0};
        std::atomic<uint64_t> dropped_bytes{0};

        // Spool for the current scan (owned by the worker thread).
        std::unique_ptr<SpoolWriter> spool;
        std::string current_filename;   // "scan_<ts>.h5" (final) / .spool
        std::string current_xml;        // set by start_scan
        std::string write_error;        // last spool write error, if any

        // Snapshot of spool stats captured in close_spool_on_worker
        // before spool.reset(). Used by /debug/sinks so post-close
        // readers still see what the spool held. Zero until the
        // first scan closes. Atomic because the worker thread writes
        // and a /debug/sinks reader on the HTTP thread reads without
        // holding lane.mtx.
        std::atomic<uint64_t> last_spool_records{0};
        std::atomic<uint64_t> last_spool_bytes{0};

        // Post-conversion counters, populated in close_scan by the
        // SpoolConverter result.
        uint32_t converted_acq{0};
        uint32_t converted_img{0};
        uint32_t converted_wf{0};
        bool conversion_ok{false};
    };

    std::filesystem::path dump_dir_;
    bool delete_spool_after_convert_;
    Lane scanner_;
    Lane recon_;
    std::atomic<ConversionStatus> status_{ConversionStatus::Idle};

    // Shared scan stem. First record on EITHER lane generates it under
    // this mutex; both lanes use the same <ts> for their spool and
    // converted .h5. start_scan adopts this stem (or generates it if
    // it's empty). While close_scan_pending_ is true, the stem is
    // treated as "the scan that just closed" and any new record
    // forces generation of a fresh stem + fresh spool rather than
    // reopening (and truncating) the closed one.
    mutable std::mutex scan_stem_mtx_;
    std::string scan_stem_;
    bool close_scan_pending_{false};

    void start_lane(Lane& lane, std::filesystem::path lane_dir, std::string tag);
    void stop_lane(Lane& lane);
    DumpEnqueueResult enqueue_scanner(uint16_t tag, std::vector<uint8_t> body);
    DumpEnqueueResult enqueue_recon(uint16_t tag, std::vector<uint8_t> body);
    DumpEnqueueResult enqueue_on(Lane& lane, uint16_t tag,
                                  std::vector<uint8_t> body);
    void worker_loop(Lane& lane);
    // Lazily open the spool on first record arrival. No lock required:
    // only the worker thread touches lane.spool / current_filename.
    void ensure_spool_on_worker(Lane& lane);
    void close_spool_on_worker(Lane& lane);
    void convert_lane(Lane& lane, const std::string& stem, bool is_scanner);
};

} // namespace mrd
