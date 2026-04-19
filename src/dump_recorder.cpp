/*
 * File: src/dump_recorder.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Raw-MRD spool recorder. Writes incoming wire bytes to a
 *          flat append-only file during the scan; on close, converts
 *          to the canonical ISMRMRD HDF5 artifact.
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "dump_recorder"
#include "logging.hpp"
#include "dump_recorder.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "mrd_io.hpp"
#include "mrd_stream_tags.hpp"
#include "spool_converter.hpp"

namespace mrd {

namespace {

// Scan filename uses the current wall-clock time so the spool and the
// eventually-converted .h5 are both named "scan_<ts>". First record of
// a scan triggers this; start_scan() uses the scanner lane's filename
// to name the recon lane's artifacts too.
std::string scan_filename_now()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm_utc;
    gmtime_r(&tt, &tm_utc);
    char buf[64];
    std::strftime(buf, sizeof(buf), "scan_%Y-%m-%dT%H:%M:%S", &tm_utc);
    char out[80];
    std::snprintf(out, sizeof(out), "%s.%03ldZ", buf, (long)ms.count());
    return std::string(out);
}

} // namespace

DumpRecorder::DumpRecorder(std::filesystem::path dump_dir,
                           bool delete_spool_after_convert)
    : dump_dir_(std::move(dump_dir))
    , delete_spool_after_convert_(delete_spool_after_convert)
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

void DumpRecorder::start_lane(Lane& lane, std::filesystem::path lane_dir,
                              std::string tag)
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

DumpEnqueueResult DumpRecorder::enqueue_scanner(uint16_t tag,
                                                 std::vector<uint8_t> body)
{
    return enqueue_on(scanner_, tag, std::move(body));
}

DumpEnqueueResult DumpRecorder::enqueue_recon(uint16_t tag,
                                               std::vector<uint8_t> body)
{
    return enqueue_on(recon_, tag, std::move(body));
}

DumpEnqueueResult DumpRecorder::enqueue_on(Lane& lane, uint16_t tag,
                                            std::vector<uint8_t> body)
{
    // Contract: no drop on queue pressure. We append unconditionally;
    // memory pressure is bounded by the scanner's actual throughput
    // times the worker-drain latency (microseconds), not queue cap.
    {
        std::lock_guard<std::mutex> lk(lane.mtx);
        if (lane.stopping) return DumpEnqueueResult::Stopped;
        lane.queue.push_back(Lane::Record{tag, std::move(body)});
    }
    lane.cv.notify_one();
    return DumpEnqueueResult::Accepted;
}

void DumpRecorder::ensure_spool_on_worker(Lane& lane)
{
    if (lane.spool) return;
    if (lane.current_filename.empty()) {
        // First record for a scan arrived before start_scan's filename
        // was available. Assign one now; start_scan() will override
        // with the scanner-chosen stem so both lanes agree.
        lane.current_filename = scan_filename_now();
    }
    auto spool_path = lane.lane_dir / (lane.current_filename + ".spool");
    lane.spool = std::make_unique<SpoolWriter>(spool_path);
    if (!lane.spool->healthy()) {
        lane.write_error = lane.spool->last_error();
        LOG_ERROR("Spool open failed on lane=" << lane.component_tag
                  << ": " << lane.write_error);
    } else {
        LOG_INFO("Opened spool lane=" << lane.component_tag
                 << " path=" << spool_path.string());
    }
}

void DumpRecorder::close_spool_on_worker(Lane& lane)
{
    if (lane.spool) {
        lane.spool->flush();
        lane.spool->close();
    }
}

void DumpRecorder::worker_loop(Lane& lane)
{
    for (;;) {
        Lane::Record rec;
        {
            std::unique_lock<std::mutex> lk(lane.mtx);
            lane.cv.wait(lk, [&lane] {
                return lane.stopping || !lane.queue.empty();
            });
            if (lane.stopping && lane.queue.empty())
                break;
            rec = std::move(lane.queue.front());
            lane.queue.pop_front();
        }

        ensure_spool_on_worker(lane);
        if (!lane.spool || !lane.spool->healthy()) {
            // Disk-write failure. Count as a hard drop.
            lane.dropped_records.fetch_add(1);
            lane.dropped_bytes.fetch_add(rec.body.size());
            if (!lane.drop_logged.exchange(true)) {
                LOG_ERROR("Dump spool write unavailable on lane="
                          << lane.component_tag << " ("
                          << lane.write_error << ")");
            }
            continue;
        }
        if (!lane.spool->append(rec.tag, rec.body.data(),
                                 static_cast<uint32_t>(rec.body.size()))) {
            lane.dropped_records.fetch_add(1);
            lane.dropped_bytes.fetch_add(rec.body.size());
            lane.write_error = lane.spool->last_error();
            if (!lane.drop_logged.exchange(true)) {
                LOG_ERROR("Dump spool write failed on lane="
                          << lane.component_tag << ": "
                          << lane.write_error);
            }
        }
    }

    close_spool_on_worker(lane);
}

// Build wire body for TEXT-class messages. Format:
//   [uint32 length][text + NUL]
static std::vector<uint8_t> build_length_prefixed_text_body(const std::string& s)
{
    const uint32_t inner = static_cast<uint32_t>(s.size() + 1);
    std::vector<uint8_t> body(sizeof(uint32_t) + inner);
    std::memcpy(body.data(), &inner, sizeof(uint32_t));
    std::memcpy(body.data() + sizeof(uint32_t), s.data(), s.size());
    body[sizeof(uint32_t) + s.size()] = '\0';
    return body;
}

DumpEnqueueResult DumpRecorder::start_scan(std::string filename, std::string xml)
{
    // Align both lanes on the scan identity. If the spool has already
    // been opened (because CONFIG_FILE / TEXT arrived before METADATA),
    // we keep using the existing stem; otherwise adopt the caller's.
    auto align = [this, &filename, &xml](Lane& lane) {
        std::lock_guard<std::mutex> lk(lane.mtx);
        // We can set current_filename safely only if the worker hasn't
        // opened a spool yet. If spool is already open, the filename is
        // whatever the worker chose; the caller's filename is ignored
        // to avoid renaming an in-flight file. In practice the caller's
        // filename matches the one scan_filename_now() would generate
        // within the same scan window, so this is rarely observable.
        if (!lane.spool && lane.current_filename.empty()) {
            // Strip ".h5" suffix from filename if present -- we want the
            // stem to which we append ".spool" or ".h5".
            std::string stem = filename;
            if (stem.size() > 3 &&
                stem.compare(stem.size() - 3, 3, ".h5") == 0)
                stem.resize(stem.size() - 3);
            lane.current_filename = stem;
        }
        lane.current_xml = xml;
    };
    align(scanner_);
    align(recon_);
    status_.store(ConversionStatus::Spooling);

    // Spool the METADATA_XML record itself so the converter can set
    // the HDF5 header on replay.
    auto body = build_length_prefixed_text_body(xml);
    return enqueue_scanner(MRD_MESSAGE_METADATA_XML_TEXT, std::move(body));
}

void DumpRecorder::close_scan()
{
    // Drain both lanes by briefly stopping their workers and
    // re-starting them for the next scan. Stop signal sets
    // lane.stopping=true, worker finishes current record + drains the
    // queue, closes the spool, then exits. After join() we clear
    // stopping and start a fresh worker thread.
    auto drain_and_restart = [this](Lane& lane) {
        stop_lane(lane);
        // Worker has exited; spool closed in worker_loop()'s exit path.
        lane.stopping = false;
        lane.worker = std::thread([this, &lane] { worker_loop(lane); });
    };
    drain_and_restart(scanner_);
    drain_and_restart(recon_);

    status_.store(ConversionStatus::Converting);
    LOG_INFO("Converting dump spool to HDF5");
    convert_lane(scanner_, /*is_scanner_side=*/true);
    convert_lane(recon_,   /*is_scanner_side=*/false);

    const bool ok = scanner_.conversion_ok && recon_.conversion_ok;
    status_.store(ok ? ConversionStatus::Complete : ConversionStatus::Failed);
    LOG_INFO("Dump conversion " << (ok ? "complete" : "FAILED"));
}

void DumpRecorder::convert_lane(Lane& lane, bool is_scanner)
{
    if (lane.current_filename.empty()) {
        // No records were ever spooled for this lane.
        lane.conversion_ok = true;
        return;
    }
    auto spool_path = lane.lane_dir / (lane.current_filename + ".spool");
    auto h5_path    = lane.lane_dir / (lane.current_filename + ".h5");
    if (!std::filesystem::exists(spool_path)) {
        lane.conversion_ok = true;
        return;
    }
    auto stats = convert_spool_to_hdf5(spool_path, h5_path, is_scanner);
    lane.converted_acq = stats.acq_written;
    lane.converted_img = stats.img_written;
    lane.converted_wf  = stats.wf_written;
    lane.conversion_ok = stats.ok();
    if (!lane.conversion_ok) {
        LOG_ERROR("Spool->HDF5 conversion failed on lane="
                  << lane.component_tag
                  << " records_read=" << stats.records_read
                  << " truncated=" << stats.truncated
                  << " error=" << stats.error);
    } else if (delete_spool_after_convert_) {
        std::error_code ec;
        std::filesystem::remove(spool_path, ec);
    }
    // Reset for next scan.
    lane.current_filename.clear();
    lane.current_xml.clear();
}

// ----- Post-hoc accessors -----

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
    auto snap_lane = [](const Lane& lane, LaneSnapshot& out) {
        std::lock_guard<std::mutex> lk(lane.mtx);
        if (lane.spool) {
            out.spool_records = lane.spool->records();
            out.spool_bytes   = lane.spool->bytes();
            out.spool_open    = lane.spool->healthy();
            out.write_error   = lane.spool->last_error();
        } else {
            out.write_error = lane.write_error;
        }
        out.converted_acq = lane.converted_acq;
        out.converted_img = lane.converted_img;
        out.converted_wf  = lane.converted_wf;
        out.conversion_ok = lane.conversion_ok;
    };
    CounterSnapshot snap;
    snap_lane(scanner_, snap.scanner);
    snap_lane(recon_,   snap.recon);
    snap.dropped_records = dropped_record_count();
    snap.dropped_bytes   = dropped_byte_count();
    snap.had_overflow    = had_overflow();
    snap.status          = status_.load();
    return snap;
}

// ----- Per-kind append APIs (build wire body, enqueue) -----

DumpEnqueueResult DumpRecorder::set_scanner_config_file(std::string config)
{
    // CONFIG_FILE wire body is a fixed 1024-byte C string on the wire
    // (padded with NUL). kspace_streamer and the reference python server
    // both send 1024 bytes.
    constexpr size_t kConfigFileBytes = 1024;
    std::vector<uint8_t> body(kConfigFileBytes, 0);
    const size_t n = std::min(config.size(), kConfigFileBytes - 1);
    std::memcpy(body.data(), config.data(), n);
    return enqueue_scanner(MRD_MESSAGE_CONFIG_FILE, std::move(body));
}

DumpEnqueueResult DumpRecorder::set_scanner_config_text(std::string config)
{
    return enqueue_scanner(MRD_MESSAGE_CONFIG_TEXT,
                           build_length_prefixed_text_body(config));
}

DumpEnqueueResult DumpRecorder::append_scanner_text(std::string text)
{
    return enqueue_scanner(MRD_MESSAGE_TEXT,
                           build_length_prefixed_text_body(text));
}

DumpEnqueueResult DumpRecorder::append_recon_text(std::string text)
{
    return enqueue_recon(MRD_MESSAGE_TEXT,
                         build_length_prefixed_text_body(text));
}

DumpEnqueueResult DumpRecorder::append_scanner_acquisition(
    const ISMRMRD::AcquisitionHeader& hdr,
    std::vector<uint8_t> traj,
    std::vector<uint8_t> samples)
{
    // Wire body: [AcquisitionHeader][traj bytes][sample bytes]
    const size_t hdr_bytes = sizeof(hdr);
    std::vector<uint8_t> body(hdr_bytes + traj.size() + samples.size());
    std::memcpy(body.data(), &hdr, hdr_bytes);
    if (!traj.empty())
        std::memcpy(body.data() + hdr_bytes, traj.data(), traj.size());
    if (!samples.empty())
        std::memcpy(body.data() + hdr_bytes + traj.size(),
                    samples.data(), samples.size());
    return enqueue_scanner(MRD_MESSAGE_ISMRMRD_ACQUISITION, std::move(body));
}

DumpEnqueueResult DumpRecorder::append_scanner_image(
    const ISMRMRD::ImageHeader& hdr,
    std::vector<uint8_t> attr,
    std::vector<uint8_t> pixels)
{
    // Wire body: [ImageHeader][uint64 attr_len][attr][pixels]
    const size_t hdr_bytes = sizeof(hdr);
    const uint64_t attr_len = attr.size();
    std::vector<uint8_t> body(hdr_bytes + sizeof(attr_len) +
                              attr.size() + pixels.size());
    size_t off = 0;
    std::memcpy(body.data() + off, &hdr, hdr_bytes); off += hdr_bytes;
    std::memcpy(body.data() + off, &attr_len, sizeof(attr_len));
    off += sizeof(attr_len);
    if (!attr.empty()) { std::memcpy(body.data() + off, attr.data(), attr.size()); off += attr.size(); }
    if (!pixels.empty()) std::memcpy(body.data() + off, pixels.data(), pixels.size());
    return enqueue_scanner(MRD_MESSAGE_ISMRMRD_IMAGE, std::move(body));
}

DumpEnqueueResult DumpRecorder::append_scanner_waveform(
    const ISMRMRD::WaveformHeader& hdr,
    std::vector<uint8_t> data)
{
    // Wire body: [WaveformHeader][sample bytes]
    std::vector<uint8_t> body(WAVEFORM_HEADER_BYTES + data.size());
    std::memcpy(body.data(), &hdr, WAVEFORM_HEADER_BYTES);
    if (!data.empty())
        std::memcpy(body.data() + WAVEFORM_HEADER_BYTES, data.data(), data.size());
    return enqueue_scanner(MRD_MESSAGE_ISMRMRD_WAVEFORM, std::move(body));
}

DumpEnqueueResult DumpRecorder::append_recon_image(std::vector<uint8_t> body)
{
    return enqueue_recon(MRD_MESSAGE_ISMRMRD_IMAGE, std::move(body));
}

DumpEnqueueResult DumpRecorder::append_recon_waveform(std::vector<uint8_t> body)
{
    return enqueue_recon(MRD_MESSAGE_ISMRMRD_WAVEFORM, std::move(body));
}

} // namespace mrd
