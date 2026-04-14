/*
 * File: src/dump_recorder.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async canonical ISMRMRD H5 dump recorder.
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "dump_recorder"
#include "logging.hpp"
#include "dump_recorder.hpp"

#include <cstring>

#include "mrd_io.hpp"
#include "mrd_stream_tags.hpp"

namespace mrd {

DumpRecorder::DumpRecorder(std::filesystem::path dump_dir)
    : dump_dir_(std::move(dump_dir))
{
    worker_ = std::thread(&DumpRecorder::worker_loop, this);
}

DumpRecorder::~DumpRecorder()
{
    close_scan();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void DumpRecorder::enqueue(size_t bytes, std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return;
        if (queue_.size() >= kMaxQueuedJobs || queued_bytes_ + bytes > kMaxQueuedBytes) {
            dropped_records_.fetch_add(queue_.size() + 1);
            dropped_bytes_.fetch_add(queued_bytes_ + bytes);
            queue_.clear();
            queued_bytes_ = 0;
            if (!drop_logged_.exchange(true)) {
                LOG_WARN("Dump queue full; marking dump incomplete and dropping pending records to keep live MRD path moving");
            }
            return;
        }
        queued_bytes_ += bytes;
        queue_.push_back(Job{bytes, std::move(fn)});
    }
    cv_.notify_one();
}

void DumpRecorder::worker_loop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty())
                break;
            job = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= job.bytes;
        }

        try {
            job.fn();
        } catch (const std::exception& e) {
            LOG_WARN("Dump write failed: " << e.what());
        }
    }

    close_scan_on_worker();
}

void DumpRecorder::start_scan(std::string filename, std::string xml)
{
    const size_t bytes = filename.size() + xml.size();
    enqueue(bytes, [this, filename = std::move(filename), xml = std::move(xml)] {
        close_scan_on_worker();
        current_filename_ = filename;
        current_xml_ = xml;
        auto path = dump_scanner_dir(dump_dir_) / current_filename_;
        scanner_sink_ = std::make_unique<MrdSink>(path);
        scanner_sink_->set_header(current_xml_);
        scanner_text_count_ = 0;
        recon_text_count_ = 0;
        drop_logged_.store(false);
        dropped_records_.store(0);
        dropped_bytes_.store(0);
    });
}

void DumpRecorder::close_scan()
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) {
            done->set_value();
        } else {
            queue_.push_back(Job{0, [this, done] {
                close_scan_on_worker();
                done->set_value();
            }});
        }
    }
    cv_.notify_one();
    fut.wait();
}

void DumpRecorder::close_scan_on_worker()
{
    if (scanner_sink_) {
        write_status_on_worker(scanner_sink_.get());
        scanner_sink_->close();
        scanner_sink_.reset();
    }
    if (recon_sink_) {
        write_status_on_worker(recon_sink_.get());
        recon_sink_->close();
        recon_sink_.reset();
    }
    current_filename_.clear();
    current_xml_.clear();
    scanner_text_count_ = 0;
    recon_text_count_ = 0;
}

void DumpRecorder::write_status_on_worker(MrdSink* sink)
{
    if (!sink) return;
    const auto dropped_records = dropped_records_.load();
    const auto dropped_bytes = dropped_bytes_.load();
    sink->write_string_dataset("dump_complete", dropped_records == 0 ? "true" : "false");
    sink->write_string_dataset("dropped_records", std::to_string(dropped_records));
    sink->write_string_dataset("dropped_bytes", std::to_string(dropped_bytes));
}

void DumpRecorder::ensure_recon_sink_on_worker()
{
    if (recon_sink_ || current_filename_.empty())
        return;

    auto path = dump_recon_dir(dump_dir_) / current_filename_;
        recon_sink_ = std::make_unique<MrdSink>(path);
    if (!current_xml_.empty())
        recon_sink_->set_header(current_xml_);
}

void DumpRecorder::set_scanner_config_file(std::string config)
{
    enqueue(config.size(), [this, config = std::move(config)] {
        if (!scanner_sink_) return;
        scanner_sink_->write_string_dataset("config_file", config);
    });
}

void DumpRecorder::set_scanner_config_text(std::string config)
{
    enqueue(config.size(), [this, config = std::move(config)] {
        if (!scanner_sink_) return;
        scanner_sink_->write_string_dataset("config", config);
    });
}

void DumpRecorder::append_scanner_text(std::string text)
{
    enqueue(text.size(), [this, text = std::move(text)] {
        if (!scanner_sink_) return;
        scanner_sink_->write_string_dataset("text_" + std::to_string(scanner_text_count_++), text);
    });
}

void DumpRecorder::append_recon_text(std::string text)
{
    enqueue(text.size(), [this, text = std::move(text)] {
        ensure_recon_sink_on_worker();
        if (!recon_sink_) return;
        recon_sink_->write_string_dataset("text_" + std::to_string(recon_text_count_++), text);
    });
}

void DumpRecorder::append_scanner_acquisition(const ISMRMRD::AcquisitionHeader& hdr,
                                              std::vector<uint8_t> traj,
                                              std::vector<uint8_t> samples)
{
    const size_t bytes = traj.size() + samples.size() + sizeof(hdr);
    enqueue(bytes, [this, hdr, traj = std::move(traj), samples = std::move(samples)] {
        if (!scanner_sink_) return;
        ISMRMRD::Acquisition acq(hdr.number_of_samples,
                                 hdr.active_channels,
                                 hdr.trajectory_dimensions);
        acq.setHead(hdr);
        if (!traj.empty())
            std::memcpy(acq.getTrajPtr(), traj.data(), traj.size());
        if (!samples.empty())
            std::memcpy(acq.getDataPtr(), samples.data(), samples.size());
        scanner_sink_->append_acquisition(acq);
    });
}

void DumpRecorder::append_scanner_image(const ISMRMRD::ImageHeader& hdr,
                                        std::vector<uint8_t> attr,
                                        std::vector<uint8_t> pixels)
{
    const size_t bytes = attr.size() + pixels.size() + sizeof(hdr);
    enqueue(bytes, [this, hdr, attr = std::move(attr), pixels = std::move(pixels)] {
        if (!scanner_sink_) return;
        const std::string varname = "image_" + std::to_string(hdr.image_series_index);
        scanner_sink_->append_image(varname, hdr,
                                    reinterpret_cast<const char*>(attr.data()), attr.size(),
                                    pixels.data(), pixels.size());
    });
}

void DumpRecorder::append_scanner_waveform(const ISMRMRD::WaveformHeader& hdr,
                                           std::vector<uint8_t> data)
{
    const size_t bytes = data.size() + sizeof(hdr);
    enqueue(bytes, [this, hdr, data = std::move(data)] {
        if (!scanner_sink_) return;
        ISMRMRD::Waveform wf(hdr.number_of_samples, hdr.channels);
        std::memcpy(&wf.head, &hdr, WAVEFORM_HEADER_BYTES);
        if (!data.empty())
            std::memcpy(wf.data, data.data(), data.size());
        scanner_sink_->append_waveform(wf);
    });
}

void DumpRecorder::append_recon_image(std::vector<uint8_t> body)
{
    enqueue(body.size(), [this, body = std::move(body)] {
        if (body.size() < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return;
        const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(body.data());
        uint64_t attr_len = 0;
        std::memcpy(&attr_len, body.data() + IMAGE_HEADER_BYTES, sizeof(uint64_t));
        const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
        if (body.size() < attr_off + attr_len) return;
        const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);
        ensure_recon_sink_on_worker();
        if (!recon_sink_) return;
        const std::string varname = "image_" + std::to_string(hdr->image_series_index);
        recon_sink_->append_image(varname, *hdr,
                                  reinterpret_cast<const char*>(body.data() + attr_off),
                                  static_cast<size_t>(attr_len),
                                  body.data() + pixel_off,
                                  body.size() - pixel_off);
    });
}

void DumpRecorder::append_recon_waveform(std::vector<uint8_t> body)
{
    enqueue(body.size(), [this, body = std::move(body)] {
        if (body.size() < WAVEFORM_HEADER_BYTES) return;
        const auto* hdr = reinterpret_cast<const ISMRMRD::WaveformHeader*>(body.data());
        const size_t data_bytes = size_t(hdr->number_of_samples) * hdr->channels * sizeof(uint32_t);
        if (body.size() < WAVEFORM_HEADER_BYTES + data_bytes) return;
        ensure_recon_sink_on_worker();
        if (!recon_sink_) return;
        ISMRMRD::Waveform wf(hdr->number_of_samples, hdr->channels);
        std::memcpy(&wf.head, hdr, WAVEFORM_HEADER_BYTES);
        if (data_bytes > 0)
            std::memcpy(wf.data, body.data() + WAVEFORM_HEADER_BYTES, data_bytes);
        recon_sink_->append_waveform(wf);
    });
}

} // namespace mrd
