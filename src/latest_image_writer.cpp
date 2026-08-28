/*
 * File: src/latest_image_writer.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async writer for the live latest-image H5 file.
 */

#include "latest_image_writer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

#undef LOG_COMPONENT
#define LOG_COMPONENT "latest_image"

namespace mrd {
namespace {

constexpr const char* kImageVarName = "image_0";

struct ParsedLatestWireImage {
    ISMRMRD::ImageHeader header{};
    std::string attributes;
    const uint8_t* pixel_data{nullptr};
    size_t pixel_bytes{0};
};

bool parse_latest_wire_image(const std::vector<uint8_t>& image,
                             ParsedLatestWireImage& parsed)
{
    if (image.size() < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;

    std::memcpy(&parsed.header, image.data(), sizeof(parsed.header));

    uint64_t attr_len = 0;
    std::memcpy(&attr_len, image.data() + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    // MEDIUM #13: overflow-safe bound check.
    if (image.size() < attr_off) return false;
    if (attr_len > image.size() - attr_off) return false;

    const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);
    parsed.attributes.assign(reinterpret_cast<const char*>(image.data() + attr_off),
                             static_cast<size_t>(attr_len));
    // LOW/NIT #20: attribute_string_len is uint32_t. Reject attributes
    // ≥ 4 GiB rather than silently truncating. Wire_guards already caps at
    // 16 MiB upstream; this is a defense in depth.
    if (parsed.attributes.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    parsed.header.attribute_string_len = static_cast<uint32_t>(parsed.attributes.size());
    parsed.pixel_data = image.data() + pixel_off;
    parsed.pixel_bytes = image.size() - pixel_off;
    return true;
}

bool append_latest_wire_image(MrdSink& sink, const ParsedLatestWireImage& image)
{
    sink.append_image(kImageVarName, image.header,
                      image.attributes.data(), image.attributes.size(),
                      image.pixel_data, image.pixel_bytes);
    return true;
}

void write_latest_image_h5_file_append(const std::filesystem::path& path,
                                       const std::string& xml,
                                       const std::vector<std::vector<uint8_t>>& images)
{
    MrdSink sink(path);
    if (!xml.empty()) sink.set_header(xml);
    for (const auto& image : images) {
        ParsedLatestWireImage parsed;
        if (!parse_latest_wire_image(image, parsed)) {
            LOG_WARN("Skipping malformed image while writing latest H5");
            continue;
        }
        append_latest_wire_image(sink, parsed);
    }
    sink.close();
}

} // namespace

void write_latest_image_h5_file(const std::filesystem::path& dest,
                                const std::string& xml,
                                const std::vector<std::vector<uint8_t>>& images)
{
    if (images.empty()) return;

    auto tmp = dest;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    ec.clear();
    std::filesystem::remove(tmp, ec);

    // Always use ISMRMRD::Dataset::appendImage per image. Produces the
    // canonical ISMRMRD HDF5 layout readable by any standard consumer
    // (real console, python-ismrmrd-server/client.py, h5py). An earlier
    // "bulk" path wrote a custom 5D layout that only some readers
    // handled, which silently broke N>1 multi-slice publishes. See
    // docs/fps-regression-2026-04-22.md.
    write_latest_image_h5_file_append(tmp, xml, images);

    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(tmp, dest, ec);
        if (ec) throw std::runtime_error("rename latest H5 failed: " + ec.message());
    }
}

struct LatestImageWriter::Job {
    std::filesystem::path dest;
    std::string xml;
    std::vector<std::vector<uint8_t>> images;
    Completion completion;
    std::chrono::steady_clock::time_point enqueued_at{};
    uint64_t seq{0};
    unsigned attempts{0};
    bool complete{true};
};

struct LatestImageWriter::Impl {
    // MEDIUM #14: bound the queue. Latest-image publication is naturally
    // coalescible: a newer job for the same destination supersedes any
    // older pending job for that destination. If the cap is hit and no
    // coalesce opportunity exists, drop the oldest and log once.
    static constexpr size_t kMaxQueuedJobs = 64;
    // Audit 2026-08-28 #3: the NEWEST job per destination is the one the
    // viewer will see last, so it must not be lost silently. Under
    // overload we evict the oldest job that a newer job for the same
    // dest already supersedes (latest-wins; nothing visible is lost).
    // Only if every queued job is the newest for its dest do we fall
    // back to dropping the oldest and report it as Dropped. A newest job
    // whose write fails (ENOSPC, transient I/O) is re-queued up to
    // kMaxWriteAttempts times before being reported as Failed.
    static constexpr unsigned kMaxWriteAttempts = 3;

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    uint64_t next_seq{1};
    std::map<std::string, uint64_t> newest_seq;   // dest -> seq
    bool stopping{false};
    std::atomic<bool> drop_logged{false};
    std::atomic<uint64_t> dropped_count{0};
    // Perf counters exposed via perf(). All free-running; computed deltas
    // between two snapshots give rates. max_* are high-watermarks.
    std::atomic<uint64_t> perf_enqueued{0};
    std::atomic<uint64_t> perf_coalesced{0};
    std::atomic<uint64_t> perf_completed{0};
    std::atomic<uint64_t> perf_failed{0};
    std::atomic<uint64_t> perf_retried{0};
    std::atomic<uint64_t> perf_evicted_complete{0};
    std::atomic<uint64_t> perf_lost{0};
    std::atomic<uint64_t> perf_max_queue_depth{0};
    std::atomic<uint64_t> perf_last_write_us{0};
    std::atomic<uint64_t> perf_max_write_us{0};
    std::atomic<uint64_t> perf_last_drain_lag_us{0};
    std::atomic<uint64_t> perf_max_drain_lag_us{0};
    std::thread worker;

    Impl()
        : worker([this] { run(); })
    {}

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stopping = true;
        }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }

    void enqueue(Job job)
    {
        job.enqueued_at = std::chrono::steady_clock::now();
        perf_enqueued.fetch_add(1, std::memory_order_relaxed);
        Job evicted_newest;   // completion runs outside the lock
        bool evicted_newest_set = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            job.seq = next_seq++;
            newest_seq[job.dest.string()] = job.seq;
            // No same-dest coalescing: silently replacing a pending payload
            // with a newer one skips published snapshots between viz polls,
            // which violates the losslessness contract. Writer stalls (seen
            // as multi-second max_write_us on Docker bind mounts) used to
            // make coalesce fire on ~34% of attempts, invisibly dropping
            // frames. Instead, queue each publish and rely on the 64-entry
            // bound + eviction as overload backstop. Under normal load
            // the writer drains fast and depth stays near 1.
            if (jobs.size() >= kMaxQueuedJobs) {
                dropped_count.fetch_add(1);
                if (!drop_logged.exchange(true)) {
                    LOG_WARN("LatestImageWriter queue exceeded " << kMaxQueuedJobs
                             << " jobs; evicting superseded pending snapshots");
                }
                // Eviction order: oldest superseded partial → oldest
                // superseded complete → (last resort) the newest.
                auto victim = std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) {
                    return !is_newest_locked(j) && !j.complete;
                });
                if (victim == jobs.end()) {
                    victim = std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) {
                        return !is_newest_locked(j);
                    });
                    if (victim != jobs.end())
                        perf_evicted_complete.fetch_add(1, std::memory_order_relaxed);
                }
                if (victim != jobs.end()) {
                    jobs.erase(victim);   // superseded: latest-wins
                } else {
                    evicted_newest = std::move(jobs.front());
                    evicted_newest_set = true;
                    jobs.pop_front();
                    perf_lost.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN("LatestImageWriter overload: dropping newest snapshot for "
                             << evicted_newest.dest);
                }
            }
            jobs.push_back(std::move(job));
            const auto depth = static_cast<uint64_t>(jobs.size());
            uint64_t prev = perf_max_queue_depth.load(std::memory_order_relaxed);
            while (depth > prev &&
                   !perf_max_queue_depth.compare_exchange_weak(
                       prev, depth, std::memory_order_relaxed)) {}
        }
        cv.notify_one();
        if (evicted_newest_set && evicted_newest.completion) {
            evicted_newest.completion(evicted_newest.dest, LatestWriteOutcome::Dropped);
        }
    }

    bool is_newest_locked(const Job& j) const
    {
        auto it = newest_seq.find(j.dest.string());
        return it != newest_seq.end() && it->second == j.seq;
    }

    void run()
    {
        using clk = std::chrono::steady_clock;
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            auto t0 = clk::now();
            const auto drain_lag_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    t0 - job.enqueued_at).count());
            perf_last_drain_lag_us.store(drain_lag_us, std::memory_order_relaxed);
            {
                uint64_t prev = perf_max_drain_lag_us.load(std::memory_order_relaxed);
                while (drain_lag_us > prev &&
                       !perf_max_drain_lag_us.compare_exchange_weak(
                           prev, drain_lag_us, std::memory_order_relaxed)) {}
            }
            try {
                ++job.attempts;
                write_latest_image_h5_file(job.dest, job.xml, job.images);
                auto t1 = clk::now();
                if (job.completion) job.completion(job.dest, LatestWriteOutcome::Committed);
                const auto write_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
                perf_last_write_us.store(write_us, std::memory_order_relaxed);
                {
                    uint64_t prev = perf_max_write_us.load(std::memory_order_relaxed);
                    while (write_us > prev &&
                           !perf_max_write_us.compare_exchange_weak(
                               prev, write_us, std::memory_order_relaxed)) {}
                }
                perf_completed.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                perf_failed.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("Latest H5 write failed (attempt " << job.attempts
                         << "/" << kMaxWriteAttempts << "): " << e.what());
                bool requeued = false;
                if (job.attempts < kMaxWriteAttempts) {
                    std::lock_guard<std::mutex> lk(mtx);
                    // Only the newest snapshot for this dest is worth
                    // retrying; a superseded one would be evicted anyway.
                    if (!stopping && is_newest_locked(job)) {
                        perf_retried.fetch_add(1, std::memory_order_relaxed);
                        jobs.push_back(std::move(job));
                        requeued = true;
                    }
                }
                if (!requeued) {
                    perf_lost.fetch_add(1, std::memory_order_relaxed);
                    if (job.completion) job.completion(job.dest, LatestWriteOutcome::Failed);
                }
            }
        }
    }
};

LatestImageWriter::LatestImageWriter()
    : impl_(std::make_unique<Impl>())
{}

LatestImageWriter::~LatestImageWriter() = default;

void LatestImageWriter::enqueue(std::filesystem::path dest,
                                std::string xml,
                                std::vector<std::vector<uint8_t>> images,
                                Completion completion,
                                bool complete)
{
    Job job{std::move(dest), std::move(xml), std::move(images),
            std::move(completion), {}};
    job.complete = complete;
    impl_->enqueue(std::move(job));
}

LatestImageWriter::PerfSnapshot LatestImageWriter::perf() const
{
    PerfSnapshot s;
    s.enqueued          = impl_->perf_enqueued.load(std::memory_order_relaxed);
    s.coalesced         = impl_->perf_coalesced.load(std::memory_order_relaxed);
    s.dropped_oldest    = impl_->dropped_count.load(std::memory_order_relaxed);
    s.completed         = impl_->perf_completed.load(std::memory_order_relaxed);
    s.failed            = impl_->perf_failed.load(std::memory_order_relaxed);
    s.retried           = impl_->perf_retried.load(std::memory_order_relaxed);
    s.evicted_complete  = impl_->perf_evicted_complete.load(std::memory_order_relaxed);
    s.lost              = impl_->perf_lost.load(std::memory_order_relaxed);
    s.max_queue_depth   = impl_->perf_max_queue_depth.load(std::memory_order_relaxed);
    s.last_write_us     = impl_->perf_last_write_us.load(std::memory_order_relaxed);
    s.max_write_us      = impl_->perf_max_write_us.load(std::memory_order_relaxed);
    s.last_drain_lag_us = impl_->perf_last_drain_lag_us.load(std::memory_order_relaxed);
    s.max_drain_lag_us  = impl_->perf_max_drain_lag_us.load(std::memory_order_relaxed);
    return s;
}

} // namespace mrd
