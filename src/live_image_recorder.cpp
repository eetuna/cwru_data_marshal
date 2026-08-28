/*
 * File: src/live_image_recorder.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Live history recorder using the same raw-MRD-spool +
 *          post-close-convert model as DumpRecorder. See
 *          live_image_recorder.hpp header for the architecture note.
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "live_image_recorder"
#include "logging.hpp"
#include "live_image_recorder.hpp"

#include <cstring>
#include <future>
#include <system_error>

#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"
#include "spool_converter.hpp"

namespace mrd {

LiveImageRecorder::LiveImageRecorder(std::filesystem::path lane_dir)
    : lane_dir_(std::move(lane_dir))
    , component_tag_(lane_dir_.filename().string())
{
    // Start the worker in the constructor BODY, after every member is
    // constructed. worker_ is declared before mtx_/cv_/queue_/stopping_,
    // so starting it in the member-init list launched worker_loop()
    // against unconstructed synchronization state (UB). Under the CPU
    // contention of a full-stack recreate, the racing worker could read
    // garbage stopping_=true and exit at birth — the first scan's
    // close_scan() barrier then waited forever (the intermittent
    // freeze-after-METADATA wedge). Same pattern as DumpRecorder's
    // start_lane.
    worker_ = std::thread([this] { worker_loop(); });
}

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

void LiveImageRecorder::enqueue_record(uint16_t tag,
                                       std::string filename,
                                       std::string xml,
                                       std::vector<uint8_t> body)
{
    size_t size_after = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return;
        queue_.push_back(Record{tag, std::move(body), std::move(filename),
                                std::move(xml), /*barrier=*/false, nullptr});
        size_after = queue_.size();
    }
    cv_.notify_one();

    if (size_after >= kHighWatermarkJobs &&
        !high_watermark_hit_.exchange(true)) {
        LOG_WARN("LiveImageRecorder queue depth " << size_after
                 << " >= " << kHighWatermarkJobs
                 << " on lane " << lane_dir_.string()
                 << "; spool writer is falling behind. No records are "
                 << "dropped; memory will grow until the worker catches up.");
    }
}

void LiveImageRecorder::append_image(std::string filename,
                                     std::string xml,
                                     std::vector<uint8_t> body)
{
    enqueue_record(MRD_MESSAGE_ISMRMRD_IMAGE,
                   std::move(filename), std::move(xml), std::move(body));
}

void LiveImageRecorder::append_waveform(std::string filename,
                                        std::string xml,
                                        std::vector<uint8_t> body)
{
    enqueue_record(MRD_MESSAGE_ISMRMRD_WAVEFORM,
                   std::move(filename), std::move(xml), std::move(body));
}

void LiveImageRecorder::close_scan(bool wait)
{
    auto signal = std::make_shared<std::promise<void>>();
    auto fut = signal->get_future();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) {
            signal->set_value();
            return;
        }
        Record barrier;
        barrier.barrier = true;
        barrier.signal = signal;
        queue_.push_back(std::move(barrier));
    }
    cv_.notify_one();
    if (wait) fut.wait();
}

void LiveImageRecorder::ensure_spool_on_worker()
{
    if (spool_) return;
    if (current_filename_.empty()) return; // wait for first record
    auto spool_path = lane_dir_ / (current_filename_ + ".spool");
    std::error_code dir_ec;
    std::filesystem::create_directories(lane_dir_, dir_ec);   // SpoolWriter reports failure
    spool_ = std::make_unique<SpoolWriter>(spool_path);
    if (!spool_->healthy()) {
        LOG_ERROR("Live spool open failed on lane=" << component_tag_
                  << ": " << spool_->last_error());
        pub_sink_open_.store(false);
    } else {
        LOG_INFO("Opened live spool lane=" << component_tag_
                 << " path=" << spool_path.string());
        pub_sink_open_.store(true);
        xml_spooled_ = false;
        pub_spool_records_.store(0);
        pub_spool_bytes_.store(0);
    }
}

// Audit 2026-08-28 #11: the MRD XML header was remembered (current_xml_)
// but never reached the converter, so live per-scan H5 files carried
// images/waveforms with no header. Spool it once as a METADATA_XML_TEXT
// record ([uint32 len][xml NUL], the wire body the converter already
// understands) as soon as both the spool and the header exist — the
// header can arrive after the first image on the recon lane.
void LiveImageRecorder::spool_xml_once_on_worker()
{
    if (xml_spooled_ || !spool_ || !spool_->healthy()) return;
    const std::string& xml = current_xml_;   // worker-owned
    if (xml.empty()) return;
    const uint32_t inner = static_cast<uint32_t>(xml.size() + 1);
    std::vector<uint8_t> body(sizeof(uint32_t) + inner);
    std::memcpy(body.data(), &inner, sizeof(inner));
    std::memcpy(body.data() + sizeof(inner), xml.data(), xml.size());
    body[body.size() - 1] = '\0';
    if (!spool_->append(MRD_MESSAGE_METADATA_XML_TEXT, body.data(),
                        static_cast<uint32_t>(body.size()))) {
        LOG_ERROR("Live spool XML append failed on lane=" << component_tag_
                  << ": " << spool_->last_error());
        return;
    }
    xml_spooled_ = true;
}

void LiveImageRecorder::close_and_convert_on_worker()
{
    if (!spool_) return;
    const auto stem = current_filename_;
    const auto xml  = current_xml_;
    auto spool_path = lane_dir_ / (stem + ".spool");
    auto h5_path    = lane_dir_ / stem;

    // Flush and close spool.
    if (!spool_->close()) {
        LOG_ERROR("Live spool close failed on lane=" << component_tag_
                  << ": " << spool_->last_error());
    }
    spool_.reset();
    pub_sink_open_.store(false);

    // Convert. is_scanner_side=true so the converter accepts all
    // MRD message kinds we might encounter (live mode today only
    // spools IMAGE + WAVEFORM, but passing true costs nothing).
    auto stats = convert_spool_to_hdf5(spool_path, h5_path, /*is_scanner_side=*/true);
    // Remove the spool whenever the converter did not hit a replay
    // error. A tail-truncated spool (last record cut by SIGTERM /
    // container stop) is the common shutdown case: every complete
    // record made it into the H5, only a partial record is lost,
    // and that record would be lost either way. Only keep the spool
    // on a genuine replay error — that's the case where the H5 is
    // missing records that still exist in the spool.
    const bool replay_error = !stats.error.empty();
    if (replay_error) {
        LOG_ERROR("Live spool->HDF5 convert failed on lane=" << component_tag_
                  << " records=" << stats.records_read
                  << " truncated=" << stats.truncated
                  << " error=" << stats.error
                  << " (spool kept for forensics at " << spool_path.string() << ")");
    } else {
        if (stats.truncated) {
            LOG_INFO("Live spool convert finished on lane=" << component_tag_
                     << " records=" << stats.records_read
                     << " (truncated tail — normal on shutdown)");
        }
        std::error_code ec;
        std::filesystem::remove(spool_path, ec);
        if (ec) {
            LOG_WARN("Spool remove failed on lane=" << component_tag_
                     << " path=" << spool_path.string()
                     << " error=" << ec.message());
        }
    }
    // Publish final converted counts for /debug/sinks.
    pub_acq_count_.store(stats.acq_written);
    pub_img_count_.store(stats.img_written);
    pub_wf_count_.store(stats.wf_written);

    // Reset for next scan.
    current_filename_.clear();
    current_xml_.clear();
    xml_spooled_ = false;
    high_watermark_hit_.store(false);
}

void LiveImageRecorder::worker_loop()
{
    for (;;) {
        Record rec;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) break;
            rec = std::move(queue_.front());
            queue_.pop_front();
        }

        if (rec.barrier) {
            close_and_convert_on_worker();
            if (rec.signal) rec.signal->set_value();
            continue;
        }

        // Worker-owned scan identity: first record of a scan defines it.
        if (current_filename_.empty() && !rec.filename.empty()) {
            current_filename_ = std::move(rec.filename);
        }
        if (current_xml_.empty() && !rec.xml.empty()) {
            current_xml_ = std::move(rec.xml);
        }
        ensure_spool_on_worker();
        spool_xml_once_on_worker();
        if (!spool_ || !spool_->healthy()) {
            // Disk failure: count as dropped so it shows up in
            // /debug/sinks. Contract keeps us from failing the
            // scan, but the retention counter makes the failure
            // visible.
            dropped_count_.fetch_add(1);
            if (!drop_logged_.exchange(true)) {
                LOG_ERROR("Live spool write unavailable on lane="
                          << component_tag_);
            }
            continue;
        }
        if (!spool_->append(rec.tag, rec.body.data(),
                            static_cast<uint32_t>(rec.body.size()))) {
            dropped_count_.fetch_add(1);
            if (!drop_logged_.exchange(true)) {
                LOG_ERROR("Live spool append failed on lane="
                          << component_tag_ << ": " << spool_->last_error());
            }
            continue;
        }
        pub_spool_records_.store(spool_->records());
        pub_spool_bytes_.store(spool_->bytes());
    }

    // Process remaining barriers / flush on shutdown.
    close_and_convert_on_worker();
}

LiveImageRecorder::CounterSnapshot LiveImageRecorder::counters() const
{
    CounterSnapshot snap;
    snap.acq           = pub_acq_count_.load();
    snap.img           = pub_img_count_.load();
    snap.wf            = pub_wf_count_.load();
    snap.sink_open     = pub_sink_open_.load();
    snap.spool_records = pub_spool_records_.load();
    snap.spool_bytes   = pub_spool_bytes_.load();
    snap.high_watermark_hit = high_watermark_hit_.load();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap.queued_jobs = queue_.size();
    }
    return snap;
}

} // namespace mrd
