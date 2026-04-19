/*
 * File: src/live_image_recorder.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async per-lane live image history recorder.
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "live_image_recorder"
#include "logging.hpp"
#include "live_image_recorder.hpp"

#include <cstring>

#include <ismrmrd/ismrmrd.h>

#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

namespace mrd {
namespace {

bool append_wire_image(MrdSink& sink, const uint8_t* data, size_t size)
{
    if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return false;

    const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data);
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, data + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    // MEDIUM #13: overflow-safe bound check.
    if (size < attr_off) return false;
    if (attr_len > size - attr_off) return false;
    const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);

    sink.append_image("image_" + std::to_string(hdr->image_series_index),
                      *hdr,
                      reinterpret_cast<const char*>(data + attr_off),
                      static_cast<size_t>(attr_len),
                      data + pixel_off,
                      size - pixel_off);
    return true;
}

bool append_wire_waveform(MrdSink& sink, const uint8_t* data, size_t size)
{
    if (size < WAVEFORM_HEADER_BYTES) return false;
    const auto* hdr = reinterpret_cast<const ISMRMRD::WaveformHeader*>(data);
    const size_t data_bytes =
        size_t(hdr->number_of_samples) * hdr->channels * sizeof(uint32_t);
    if (size < WAVEFORM_HEADER_BYTES + data_bytes) return false;

    ISMRMRD::Waveform wf(hdr->number_of_samples, hdr->channels);
    std::memcpy(&wf.head, hdr, WAVEFORM_HEADER_BYTES);
    if (data_bytes > 0)
        std::memcpy(wf.data, data + WAVEFORM_HEADER_BYTES, data_bytes);
    sink.append_waveform(wf);
    return true;
}

} // namespace

LiveImageRecorder::LiveImageRecorder(std::filesystem::path lane_dir)
    : lane_dir_(std::move(lane_dir))
    , worker_([this] { worker_loop(); })
{}

LiveImageRecorder::~LiveImageRecorder()
{
    close_scan();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LiveImageRecorder::enqueue(std::function<void()> fn, bool droppable)
{
    size_t queue_size_after = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return;
        // Lossless: no drop under queue pressure. The contract says
        // live history must never drop records. Under normal live-mode
        // rates (~65 records/s vs ~1000/s HDF5 ceiling) the queue
        // stays near-empty. If it ever grows dramatically we log a
        // one-shot high-watermark warning but still admit the job.
        queue_.push_back(Job{std::move(fn), droppable});
        queue_size_after = queue_.size();
    }
    cv_.notify_one();

    if (queue_size_after >= kHighWatermarkJobs &&
        !high_watermark_hit_.exchange(true)) {
        LOG_WARN("LiveImageRecorder queue depth " << queue_size_after
                 << " >= " << kHighWatermarkJobs << " on lane "
                 << lane_dir_.string()
                 << "; live HDF5 worker is falling behind. "
                 << "This does NOT drop records; memory will grow until "
                 << "the worker catches up.");
    }
}

void LiveImageRecorder::append_image(std::string filename,
                                     std::string xml,
                                     std::vector<uint8_t> body)
{
    enqueue([this,
             filename = std::move(filename),
             xml = std::move(xml),
             body = std::move(body)]() mutable {
        ensure_sink_on_worker(filename, xml);
        if (!sink_) return;
        if (!append_wire_image(*sink_, body.data(), body.size())) {
            LOG_WARN("Skipping malformed live image append for " << lane_dir_.string());
            return;
        }
        // Publish counters AFTER successful append. Readers (counters()
        // from the HTTP thread) only consult these atomics; sink_ stays
        // worker-exclusive so there's no read/write race on it.
        pub_img_count_.store(sink_->image_count());
        pub_acq_count_.store(sink_->acquisition_count());
        pub_wf_count_.store(sink_->waveform_count());
    });
}

void LiveImageRecorder::append_waveform(std::string filename,
                                        std::string xml,
                                        std::vector<uint8_t> body)
{
    enqueue([this,
             filename = std::move(filename),
             xml = std::move(xml),
             body = std::move(body)]() mutable {
        ensure_sink_on_worker(filename, xml);
        if (!sink_) return;
        if (!append_wire_waveform(*sink_, body.data(), body.size())) {
            LOG_WARN("Skipping malformed live waveform append for " << lane_dir_.string());
            return;
        }
        pub_wf_count_.store(sink_->waveform_count());
        pub_acq_count_.store(sink_->acquisition_count());
        pub_img_count_.store(sink_->image_count());
    });
}

void LiveImageRecorder::close_scan()
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) {
            done->set_value();
        } else {
            // MEDIUM #15: close_scan is a barrier; mark non-droppable so the
            // bounded-queue path never removes it.
            queue_.push_back(Job{[this, done] {
                close_scan_on_worker();
                done->set_value();
            }, /*droppable=*/false});
        }
    }
    cv_.notify_one();
    fut.wait();
}

void LiveImageRecorder::worker_loop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            job.fn();
        } catch (const std::exception& e) {
            LOG_WARN("Live image append failed: " << e.what());
        }
    }

    close_scan_on_worker();
}

LiveImageRecorder::CounterSnapshot LiveImageRecorder::counters() const
{
    // IMPORTANT: this reads ONLY atomics + queue depth. It never
    // touches sink_ because the worker thread owns that pointer
    // exclusively and mutates it (ensure_sink_on_worker,
    // close_scan_on_worker) WITHOUT holding mtx_. Reading sink_ here
    // would race with those mutations even under lock, because the
    // worker does not take the lock while using sink_.
    CounterSnapshot snap;
    snap.acq       = pub_acq_count_.load();
    snap.img       = pub_img_count_.load();
    snap.wf        = pub_wf_count_.load();
    snap.sink_open = pub_sink_open_.load();
    snap.high_watermark_hit = high_watermark_hit_.load();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap.queued_jobs = queue_.size();
    }
    return snap;
}

void LiveImageRecorder::close_scan_on_worker()
{
    if (sink_) {
        // Publish final counters BEFORE the sink is destroyed, so
        // counters() keeps reporting the final state after close.
        pub_acq_count_.store(sink_->acquisition_count());
        pub_img_count_.store(sink_->image_count());
        pub_wf_count_.store(sink_->waveform_count());
        sink_->close();
        sink_.reset();
    }
    pub_sink_open_.store(false);
    current_filename_.clear();
    // Reset per-scan watermark so the next scan starts clean.
    high_watermark_hit_.store(false);
}

void LiveImageRecorder::ensure_sink_on_worker(const std::string& filename,
                                              const std::string& xml)
{
    if (filename.empty()) return;

    if (sink_ && current_filename_ != filename) {
        close_scan_on_worker();
    }

    if (!sink_) {
        current_filename_ = filename;
        sink_ = std::make_unique<MrdSink>(lane_dir_ / current_filename_);
        if (!xml.empty()) sink_->set_header(xml);
        // Fresh sink for a new scan: reset published counters and
        // mark as open.
        pub_acq_count_.store(0);
        pub_img_count_.store(0);
        pub_wf_count_.store(0);
        pub_sink_open_.store(true);
    }
}

} // namespace mrd
