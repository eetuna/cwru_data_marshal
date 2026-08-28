/*
 * File: src/latest_image_writer.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async writer for the live latest-image H5 file.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

namespace mrd {

void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images);

// Final outcome of one enqueued snapshot. Committed: the H5 is on disk at
// dest. Dropped: evicted under queue overload. Failed: every write attempt
// threw. (Audit 2026-08-28 #3: publishers used to learn only of success,
// so a dropped/failed FINAL snapshot vanished silently.)
enum class LatestWriteOutcome { Committed, Dropped, Failed };

class LatestImageWriter {
public:
    using Completion =
        std::function<void(const std::filesystem::path&, LatestWriteOutcome)>;

    LatestImageWriter();
    ~LatestImageWriter();

    LatestImageWriter(const LatestImageWriter&) = delete;
    LatestImageWriter& operator=(const LatestImageWriter&) = delete;

    // complete: the snapshot is a whole volume (audit 2026-08-28 #6).
    // Under overload the queue evicts superseded PARTIAL snapshots first,
    // then the oldest superseded complete one, so complete volumes are
    // what survives when the writer falls behind.
    void enqueue(std::filesystem::path dest,
                 std::string xml,
                 std::vector<std::vector<uint8_t>> images,
                 Completion completion,
                 bool complete = true);

    struct PerfSnapshot {
        uint64_t enqueued{0};
        uint64_t coalesced{0};
        uint64_t dropped_oldest{0};
        uint64_t completed{0};
        uint64_t failed{0};
        // Write attempts re-queued after a failure (newest job per dest only).
        uint64_t retried{0};
        // Superseded COMPLETE volumes evicted under overload (partials are
        // always evicted first; this counts the cases where none was left).
        uint64_t evicted_complete{0};
        // Snapshots that never reached disk: newest-per-dest jobs dropped
        // with nothing to supersede them, or failed on every attempt.
        uint64_t lost{0};
        uint64_t max_queue_depth{0};
        uint64_t last_write_us{0};
        uint64_t max_write_us{0};
        uint64_t last_drain_lag_us{0};
        uint64_t max_drain_lag_us{0};
    };
    PerfSnapshot perf() const;

private:
    struct Job;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mrd
