/*
 * File: src/dump_recorder.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Async canonical ISMRMRD H5 dump recorder.
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "dump_recorder"
#include "logging.hpp"
#include "dump_recorder.hpp"

#include <algorithm>
#include <cstring>

#include "mrd_io.hpp"
#include "mrd_stream_tags.hpp"

namespace mrd {

DumpRecorder::DumpRecorder(std::filesystem::path dump_dir)
    : dump_dir_(std::move(dump_dir))
{
    start_lane(scanner_, dump_scanner_dir(dump_dir_), "scanner");
    start_lane(recon_,   dump_recon_dir(dump_dir_),   "recon");
}

DumpRecorder::~DumpRecorder()
{
    close_scan();
    stop_lane(scanner_);
    stop_lane(recon_);
}

void DumpRecorder::start_lane(Lane& lane, std::filesystem::path lane_dir, std::string tag)
{
    lane.lane_dir = std::move(lane_dir);
    lane.component_tag = std::move(tag);
    lane.worker = std::thread([this, &lane] { worker_loop(lane); });
}

void DumpRecorder::stop_lane(Lane& lane)
{
    {
        std::lock_guard<std::mutex> lk(lane.mtx);
        lane.stopping = true;
    }
    lane.cv.notify_all();
    if (lane.worker.joinable())
        lane.worker.join();
}

DumpEnqueueResult DumpRecorder::enqueue_scanner(size_t bytes, std::function<void()> fn)
{
    return enqueue_on(scanner_, bytes, std::move(fn), /*droppable=*/true);
}

DumpEnqueueResult DumpRecorder::enqueue_recon(size_t bytes, std::function<void()> fn)
{
    return enqueue_on(recon_, bytes, std::move(fn), /*droppable=*/true);
}

DumpEnqueueResult DumpRecorder::enqueue_on(Lane& lane, size_t bytes,
                                            std::function<void()> fn, bool droppable)
{
    DumpEnqueueResult result = DumpEnqueueResult::Accepted;

    // Oversized droppable jobs cannot fit in the cap even with an empty
    // queue. Reject them up front rather than admitting silently.
    if (droppable && bytes > kMaxQueuedBytes) {
        lane.dropped_records.fetch_add(1);
        lane.dropped_bytes.fetch_add(bytes);
        if (!lane.drop_logged.exchange(true)) {
            LOG_WARN("Dump queue rejecting oversized record on lane="
                     << lane.component_tag << " (" << bytes
                     << " bytes > cap " << kMaxQueuedBytes << ")");
        }
        return DumpEnqueueResult::Dropped;
    }

    {
        std::lock_guard<std::mutex> lk(lane.mtx);
        if (lane.stopping) return DumpEnqueueResult::Stopped;

        // Oldest-drop on overflow: shed exactly enough oldest droppable
        // jobs to make room for the new admission. Non-droppable barriers
        // are skipped so they always survive.
        for (int safety = 0; safety < static_cast<int>(kMaxQueuedJobs) + 1; ++safety) {
            if (lane.queue.size() < kMaxQueuedJobs &&
                lane.queued_bytes + bytes <= kMaxQueuedBytes) {
                break;
            }
            auto it = std::find_if(lane.queue.begin(), lane.queue.end(),
                                   [](const Lane::Job& j) { return j.droppable; });
            if (it == lane.queue.end()) break;
            lane.queued_bytes -= it->bytes;
            lane.dropped_records.fetch_add(1);
            lane.dropped_bytes.fetch_add(it->bytes);
            lane.queue.erase(it);
            if (!lane.drop_logged.exchange(true)) {
                LOG_WARN("Dump queue full on lane=" << lane.component_tag
                         << "; dropping oldest pending record to keep live MRD path moving");
            }
            result = DumpEnqueueResult::Dropped;
        }

        // After the drop loop, if a droppable job still doesn't fit (queue
        // is full of non-droppable barriers), reject it rather than blowing
        // the cap silently. Non-droppable barriers are allowed to bypass
        // the cap because they're rare, small, and required for correctness.
        const bool fits =
            lane.queue.size() < kMaxQueuedJobs &&
            lane.queued_bytes + bytes <= kMaxQueuedBytes;
        if (!fits && droppable) {
            lane.dropped_records.fetch_add(1);
            lane.dropped_bytes.fetch_add(bytes);
            if (!lane.drop_logged.exchange(true)) {
                LOG_WARN("Dump queue full on lane=" << lane.component_tag
                         << " and only barriers remain; dropping new record");
            }
            return DumpEnqueueResult::Dropped;
        }

        lane.queued_bytes += bytes;
        lane.queue.push_back(Lane::Job{bytes, std::move(fn), droppable});
    }
    lane.cv.notify_one();
    return result;
}

void DumpRecorder::worker_loop(Lane& lane)
{
    for (;;) {
        Lane::Job job;
        {
            std::unique_lock<std::mutex> lk(lane.mtx);
            lane.cv.wait(lk, [&lane] { return lane.stopping || !lane.queue.empty(); });
            if (lane.stopping && lane.queue.empty())
                break;
            job = std::move(lane.queue.front());
            lane.queue.pop_front();
            lane.queued_bytes -= job.bytes;
        }

        try {
            job.fn();
        } catch (const std::exception& e) {
            LOG_WARN("Dump write failed on lane=" << lane.component_tag
                     << ": " << e.what());
        }
    }

    close_scan_on_worker(lane);
}

DumpEnqueueResult DumpRecorder::start_scan(std::string filename, std::string xml)
{
    const size_t bytes = filename.size() + xml.size();
    auto fn = [this, filename, xml] {
        // Per-lane init lambda is run separately on each worker. We dispatch
        // identical bodies because each lane only manages its own sink.
    };
    // Issue start to both lanes. Use a shared payload so both workers see
    // identical filename/xml.
    auto payload = std::make_shared<std::pair<std::string, std::string>>(
        std::move(filename), std::move(xml));

    auto enq_lane = [this, bytes, payload](Lane& lane, bool is_scanner) {
        return enqueue_on(lane, bytes, [this, &lane, payload, is_scanner] {
            // Snapshot pending CONFIG/TEXT BEFORE close_scan_on_worker
            // clears them. python-ismrmrd-server's protocol allows
            // CONFIG_FILE / CONFIG_TEXT / TEXT to arrive before
            // METADATA_XML; those jobs ran on the worker with no sink
            // open and stashed their payload in pending_*. We must
            // replay them into the sink that start_scan is about to
            // open. close_scan_on_worker clears the buffers as part of
            // between-scan cleanup, so we capture first.
            std::string pending_cf       = std::move(lane.pending_config_file);
            bool        pending_cf_set   = lane.pending_config_file_set;
            std::string pending_ct       = std::move(lane.pending_config_text);
            bool        pending_ct_set   = lane.pending_config_text_set;
            std::vector<std::string> pending_txt = std::move(lane.pending_texts);

            close_scan_on_worker(lane);
            lane.current_filename = payload->first;
            lane.current_xml = payload->second;
            auto path = lane.lane_dir / lane.current_filename;
            lane.sink = std::make_unique<MrdSink>(path);
            lane.sink->set_header(lane.current_xml);

            if (pending_cf_set) {
                lane.sink->write_string_dataset("config_file", pending_cf);
            }
            if (pending_ct_set) {
                lane.sink->write_string_dataset("config", pending_ct);
            }
            for (auto& text : pending_txt) {
                lane.sink->write_string_dataset(
                    "text_" + std::to_string(lane.text_count++), text);
            }

            lane.drop_logged.store(false);
            lane.dropped_records.store(0);
            lane.dropped_bytes.store(0);
            (void)is_scanner;
        }, /*droppable=*/false);
    };

    auto r1 = enq_lane(scanner_, true);
    auto r2 = enq_lane(recon_, false);
    if (r1 == DumpEnqueueResult::Stopped || r2 == DumpEnqueueResult::Stopped)
        return DumpEnqueueResult::Stopped;
    if (r1 == DumpEnqueueResult::Dropped || r2 == DumpEnqueueResult::Dropped)
        return DumpEnqueueResult::Dropped;
    return DumpEnqueueResult::Accepted;
}

void DumpRecorder::close_scan()
{
    auto close_one = [this](Lane& lane) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        {
            std::lock_guard<std::mutex> lk(lane.mtx);
            if (lane.stopping) {
                done->set_value();
            } else {
                lane.queue.push_back(Lane::Job{0, [this, &lane, done] {
                    close_scan_on_worker(lane);
                    done->set_value();
                }, /*droppable=*/false});
            }
        }
        lane.cv.notify_one();
        fut.wait();
    };
    close_one(scanner_);
    close_one(recon_);
}

void DumpRecorder::close_scan_on_worker(Lane& lane)
{
    if (lane.sink) {
        // Snapshot counts before close so /debug/sinks can report final
        // retention after the sink is gone.
        lane.last_closed_acq = lane.sink->acquisition_count();
        lane.last_closed_img = lane.sink->image_count();
        lane.last_closed_wf  = lane.sink->waveform_count();
        lane.ever_closed = true;
        write_status_on_worker(lane.sink.get(), lane);
        lane.sink->close();
        lane.sink.reset();
    }
    lane.current_filename.clear();
    lane.current_xml.clear();
    lane.text_count = 0;
    lane.pending_config_file.clear();
    lane.pending_config_file_set = false;
    lane.pending_config_text.clear();
    lane.pending_config_text_set = false;
    lane.pending_texts.clear();
}

bool DumpRecorder::had_overflow() const noexcept
{
    return scanner_.drop_logged.load() || recon_.drop_logged.load();
}

uint64_t DumpRecorder::dropped_record_count() const noexcept
{
    return scanner_.dropped_records.load() + recon_.dropped_records.load();
}

uint64_t DumpRecorder::dropped_byte_count() const noexcept
{
    return scanner_.dropped_bytes.load() + recon_.dropped_bytes.load();
}

DumpRecorder::CounterSnapshot DumpRecorder::counters() const
{
    auto fill = [](const Lane& lane, SinkCounters& out) {
        std::lock_guard<std::mutex> lk(lane.mtx);
        if (lane.sink) {
            out.acq = lane.sink->acquisition_count();
            out.img = lane.sink->image_count();
            out.wf  = lane.sink->waveform_count();
            out.open = true;
        } else if (lane.ever_closed) {
            // Final counters from the most-recently-closed sink, so
            // /debug/sinks remains a valid retention metric after the
            // scan has closed.
            out.acq = lane.last_closed_acq;
            out.img = lane.last_closed_img;
            out.wf  = lane.last_closed_wf;
            out.open = false;
        }
    };
    CounterSnapshot snap;
    fill(scanner_, snap.scanner);
    fill(recon_,   snap.recon);
    snap.dropped_records = dropped_record_count();
    snap.dropped_bytes = dropped_byte_count();
    snap.had_overflow = had_overflow();
    return snap;
}

void DumpRecorder::write_status_on_worker(MrdSink* sink, const Lane& lane)
{
    if (!sink) return;
    const auto dropped_records = lane.dropped_records.load();
    const auto dropped_bytes = lane.dropped_bytes.load();
    sink->write_string_dataset("dump_complete", dropped_records == 0 ? "true" : "false");
    sink->write_string_dataset("dropped_records", std::to_string(dropped_records));
    sink->write_string_dataset("dropped_bytes", std::to_string(dropped_bytes));
}

DumpEnqueueResult DumpRecorder::set_scanner_config_file(std::string config)
{
    return enqueue_scanner(config.size(), [this, config = std::move(config)] {
        if (!scanner_.sink) {
            // Sink not open yet — buffer for replay by start_scan.
            scanner_.pending_config_file = config;
            scanner_.pending_config_file_set = true;
            return;
        }
        scanner_.sink->write_string_dataset("config_file", config);
    });
}

DumpEnqueueResult DumpRecorder::set_scanner_config_text(std::string config)
{
    return enqueue_scanner(config.size(), [this, config = std::move(config)] {
        if (!scanner_.sink) {
            scanner_.pending_config_text = config;
            scanner_.pending_config_text_set = true;
            return;
        }
        scanner_.sink->write_string_dataset("config", config);
    });
}

DumpEnqueueResult DumpRecorder::append_scanner_text(std::string text)
{
    return enqueue_scanner(text.size(), [this, text = std::move(text)] {
        if (!scanner_.sink) {
            scanner_.pending_texts.push_back(text);
            return;
        }
        scanner_.sink->write_string_dataset(
            "text_" + std::to_string(scanner_.text_count++), text);
    });
}

DumpEnqueueResult DumpRecorder::append_recon_text(std::string text)
{
    return enqueue_recon(text.size(), [this, text = std::move(text)] {
        if (!recon_.sink) {
            recon_.pending_texts.push_back(text);
            return;
        }
        recon_.sink->write_string_dataset(
            "text_" + std::to_string(recon_.text_count++), text);
    });
}

DumpEnqueueResult DumpRecorder::append_scanner_acquisition(const ISMRMRD::AcquisitionHeader& hdr,
                                                            std::vector<uint8_t> traj,
                                                            std::vector<uint8_t> samples)
{
    const size_t bytes = traj.size() + samples.size() + sizeof(hdr);
    auto acq = std::make_shared<ISMRMRD::Acquisition>(
        hdr.number_of_samples, hdr.active_channels, hdr.trajectory_dimensions);
    acq->setHead(hdr);
    if (!traj.empty())
        std::memcpy(acq->getTrajPtr(), traj.data(), traj.size());
    if (!samples.empty())
        std::memcpy(acq->getDataPtr(), samples.data(), samples.size());
    return enqueue_scanner(bytes, [this, acq = std::move(acq)] {
        if (!scanner_.sink) return;
        scanner_.sink->append_acquisition(*acq);
    });
}

DumpEnqueueResult DumpRecorder::append_scanner_image(const ISMRMRD::ImageHeader& hdr,
                                                     std::vector<uint8_t> attr,
                                                     std::vector<uint8_t> pixels)
{
    const size_t bytes = attr.size() + pixels.size() + sizeof(hdr);
    struct Payload {
        ISMRMRD::ImageHeader hdr;
        std::vector<uint8_t> attr;
        std::vector<uint8_t> pixels;
    };
    auto p = std::make_shared<Payload>(Payload{hdr, std::move(attr), std::move(pixels)});
    return enqueue_scanner(bytes, [this, p = std::move(p)] {
        if (!scanner_.sink) return;
        const std::string varname = "image_" + std::to_string(p->hdr.image_series_index);
        scanner_.sink->append_image(varname, p->hdr,
                                    reinterpret_cast<const char*>(p->attr.data()), p->attr.size(),
                                    p->pixels.data(), p->pixels.size());
    });
}

DumpEnqueueResult DumpRecorder::append_scanner_waveform(const ISMRMRD::WaveformHeader& hdr,
                                                        std::vector<uint8_t> data)
{
    const size_t bytes = data.size() + sizeof(hdr);
    auto wf = std::make_shared<ISMRMRD::Waveform>(hdr.number_of_samples, hdr.channels);
    std::memcpy(&wf->head, &hdr, WAVEFORM_HEADER_BYTES);
    if (!data.empty())
        std::memcpy(wf->data, data.data(), data.size());
    return enqueue_scanner(bytes, [this, wf = std::move(wf)] {
        if (!scanner_.sink) return;
        scanner_.sink->append_waveform(*wf);
    });
}

DumpEnqueueResult DumpRecorder::append_recon_image(std::vector<uint8_t> body)
{
    return enqueue_recon(body.size(), [this, body = std::move(body)] {
        if (body.size() < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return;
        const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(body.data());
        uint64_t attr_len = 0;
        std::memcpy(&attr_len, body.data() + IMAGE_HEADER_BYTES, sizeof(uint64_t));
        const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
        // MEDIUM #13: overflow-safe bound check.
        if (body.size() < attr_off) return;
        if (attr_len > body.size() - attr_off) return;
        const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);
        if (!recon_.sink) return;
        const std::string varname = "image_" + std::to_string(hdr->image_series_index);
        recon_.sink->append_image(varname, *hdr,
                                  reinterpret_cast<const char*>(body.data() + attr_off),
                                  static_cast<size_t>(attr_len),
                                  body.data() + pixel_off,
                                  body.size() - pixel_off);
    });
}

DumpEnqueueResult DumpRecorder::append_recon_waveform(std::vector<uint8_t> body)
{
    return enqueue_recon(body.size(), [this, body = std::move(body)] {
        if (body.size() < WAVEFORM_HEADER_BYTES) return;
        const auto* hdr = reinterpret_cast<const ISMRMRD::WaveformHeader*>(body.data());
        const size_t data_bytes = size_t(hdr->number_of_samples) * hdr->channels * sizeof(uint32_t);
        if (body.size() < WAVEFORM_HEADER_BYTES + data_bytes) return;
        if (!recon_.sink) return;
        ISMRMRD::Waveform wf(hdr->number_of_samples, hdr->channels);
        std::memcpy(&wf.head, hdr, WAVEFORM_HEADER_BYTES);
        if (data_bytes > 0)
            std::memcpy(wf.data, body.data() + WAVEFORM_HEADER_BYTES, data_bytes);
        recon_.sink->append_waveform(wf);
    });
}

} // namespace mrd
