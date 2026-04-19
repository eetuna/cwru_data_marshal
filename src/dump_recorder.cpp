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
    // Destructor path (process shutdown / DumpRecorder reset): stop
    // admissions FIRST so no racy enqueue can slip in between
    // close_scan's barrier and worker teardown. Then stop the worker,
    // which drains anything already queued and calls
    // close_spool_on_worker on exit. Finally run the converter on
    // whatever was spooled.
    //
    // We deliberately do NOT call close_scan() here. close_scan uses
    // a barrier + pending-flag protocol that assumes the worker keeps
    // running afterwards to accept the next scan. In the destructor
    // we are shutting down, so the simpler stop-first path is
    // correct and closes the "record arrives after close_scan
    // returns" window that a call to close_scan would leave open.
    std::string stem;
    {
        std::lock_guard<std::mutex> stem_lk(scan_stem_mtx_);
        stem = scan_stem_;
    }
    stop_lane(scanner_);
    stop_lane(recon_);
    if (!stem.empty()) {
        convert_lane(scanner_, stem, /*is_scanner_side=*/true);
        convert_lane(recon_,   stem, /*is_scanner_side=*/false);
    }
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
    // Generate the shared scan stem on first record of the scan so
    // both lanes use the same <ts> in their filenames. While a
    // close_scan is pending (barriers issued, convert not yet
    // complete), force a fresh stem so a post-barrier record cannot
    // reopen the closed spool path with "wb" and truncate it.
    {
        std::lock_guard<std::mutex> stem_lk(scan_stem_mtx_);
        if (scan_stem_.empty() || close_scan_pending_) {
            scan_stem_ = scan_filename_now();
            close_scan_pending_ = false;
        }
    }
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
        // Adopt the shared scan stem. enqueue_on generates it on first
        // record, so by the time the worker runs it is non-empty.
        std::lock_guard<std::mutex> stem_lk(scan_stem_mtx_);
        lane.current_filename = scan_stem_;
    }
    auto spool_path = lane.lane_dir / (lane.current_filename + ".spool");
    lane.spool = std::make_unique<SpoolWriter>(spool_path);
    if (!lane.spool->healthy()) {
        std::string err = lane.spool->last_error();
        {
            std::lock_guard<std::mutex> lk(lane.mtx);
            lane.write_error = err;
        }
        LOG_ERROR("Spool open failed on lane=" << lane.component_tag
                  << ": " << err);
        lane.spool_is_open.store(false);
    } else {
        LOG_INFO("Opened spool lane=" << lane.component_tag
                 << " path=" << spool_path.string());
        // Fresh spool: reset live counters and mark open. counters()
        // will see the new state on its next poll.
        lane.live_spool_records.store(0);
        lane.live_spool_bytes.store(0);
        lane.spool_is_open.store(true);
    }
}

void DumpRecorder::close_spool_on_worker(Lane& lane)
{
    if (lane.spool) {
        // Mark closed BEFORE we start tearing down the SpoolWriter so
        // a concurrent counters() reader does not interpret the atomic
        // live counters as "open" while we are in the process of
        // destroying the writer.
        lane.spool_is_open.store(false);
        // Snapshot counters BEFORE reset so /debug/sinks can keep
        // reporting them post-close. Atomic store so HTTP-thread
        // reads get a well-ordered value without holding lane.mtx.
        lane.last_spool_records.store(lane.spool->records());
        lane.last_spool_bytes.store(lane.spool->bytes());
        // close() flushes then fcloses, surfacing errors from either.
        if (!lane.spool->close()) {
            std::string err = lane.spool->last_error();
            {
                std::lock_guard<std::mutex> lk(lane.mtx);
                lane.write_error = err;
            }
            LOG_ERROR("Dump spool close failed on lane="
                      << lane.component_tag << ": " << err);
        }
        // Reset so the NEXT scan can open a fresh spool. Without this
        // the next ensure_spool_on_worker() early-returns on non-null
        // spool and writes go to a closed file.
        lane.spool.reset();
    }
    // Clear per-scan stem so next start_scan picks a fresh filename.
    lane.current_filename.clear();
    lane.current_xml.clear();
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

        if (rec.barrier) {
            // Flush and close the spool so the converter sees a
            // complete file. Worker keeps running so new enqueues
            // after the barrier are NOT rejected with Stopped.
            // scan_stem_ is protected from post-barrier truncation by
            // close_scan_pending_ (set by close_scan before draining);
            // any record that races with the barrier will generate
            // a fresh stem via enqueue_on rather than reusing this one.
            close_spool_on_worker(lane);
            if (rec.signal) rec.signal->set_value();
            continue;
        }

        ensure_spool_on_worker(lane);
        if (!lane.spool || !lane.spool->healthy()) {
            // Disk-write failure. Count as a hard drop.
            lane.dropped_records.fetch_add(1);
            lane.dropped_bytes.fetch_add(rec.body.size());
            if (!lane.drop_logged.exchange(true)) {
                std::string err;
                {
                    std::lock_guard<std::mutex> lk(lane.mtx);
                    err = lane.write_error;
                }
                LOG_ERROR("Dump spool write unavailable on lane="
                          << lane.component_tag << " (" << err << ")");
            }
            continue;
        }
        if (!lane.spool->append(rec.tag, rec.body.data(),
                                 static_cast<uint32_t>(rec.body.size()))) {
            lane.dropped_records.fetch_add(1);
            lane.dropped_bytes.fetch_add(rec.body.size());
            std::string err = lane.spool->last_error();
            {
                std::lock_guard<std::mutex> lk(lane.mtx);
                lane.write_error = err;
            }
            if (!lane.drop_logged.exchange(true)) {
                LOG_ERROR("Dump spool write failed on lane="
                          << lane.component_tag << ": " << err);
            }
        } else {
            // Publish fresh counters so /debug/sinks reflects the
            // write without needing to read lane.spool directly.
            lane.live_spool_records.store(lane.spool->records());
            lane.live_spool_bytes.store(lane.spool->bytes());
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
    // Seed the shared stem if no record has arrived yet. If CONFIG or
    // TEXT already arrived before METADATA, enqueue_on has already set
    // scan_stem_; we keep that stem so the spool that the worker
    // already opened (or is about to) uses consistent names.
    {
        std::lock_guard<std::mutex> stem_lk(scan_stem_mtx_);
        if (scan_stem_.empty()) {
            // Honor caller-supplied filename if reasonable; strip .h5 suffix.
            std::string stem = filename;
            if (stem.size() > 3 &&
                stem.compare(stem.size() - 3, 3, ".h5") == 0)
                stem.resize(stem.size() - 3);
            scan_stem_ = stem.empty() ? scan_filename_now() : stem;
        }
    }
    auto align = [&xml](Lane& lane) {
        std::lock_guard<std::mutex> lk(lane.mtx);
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
    // Inject a barrier into each lane. The worker drains everything
    // enqueued so far, then flushes/closes the spool, then signals
    // the promise. We wait on the future here. The worker thread is
    // never stopped, so concurrent enqueues after the barrier are
    // admitted (they'll open a new spool when the next scan starts).
    // Atomic barrier-insertion under scan_stem_mtx_: any concurrent
    // enqueue_on sees close_scan_pending_=true ONLY after both lane
    // barriers are already queued. Without this, a record that arrived
    // between "set pending" and "push barriers" would see pending=true,
    // generate a fresh stem, clear pending, and then land on the lane
    // queue in front of the (not-yet-pushed) barrier -- polluting the
    // old scan's spool and/or orphaning a fresh stem.
    //
    // Two futures so we can wait on both AFTER releasing the stem lock.
    std::string stem;
    auto scanner_signal = std::make_shared<std::promise<void>>();
    auto scanner_fut    = scanner_signal->get_future();
    auto recon_signal   = std::make_shared<std::promise<void>>();
    auto recon_fut      = recon_signal->get_future();
    {
        std::lock_guard<std::mutex> stem_lk(scan_stem_mtx_);
        stem = scan_stem_;
        close_scan_pending_ = true;

        auto queue_barrier = [](Lane& lane,
                                std::shared_ptr<std::promise<void>> sig) {
            std::lock_guard<std::mutex> lk(lane.mtx);
            if (lane.stopping) {
                // Destructor path: worker is exiting. Satisfy the
                // promise now so the wait below returns.
                sig->set_value();
                return;
            }
            Lane::Record barrier;
            barrier.barrier = true;
            barrier.signal = std::move(sig);
            lane.queue.push_back(std::move(barrier));
        };
        queue_barrier(scanner_, scanner_signal);
        queue_barrier(recon_,   recon_signal);
    }
    scanner_.cv.notify_one();
    recon_.cv.notify_one();
    scanner_fut.wait();
    recon_fut.wait();

    status_.store(ConversionStatus::Converting);
    LOG_INFO("Converting dump spool to HDF5");
    convert_lane(scanner_, stem, /*is_scanner_side=*/true);
    convert_lane(recon_,   stem, /*is_scanner_side=*/false);

    // Clear the old stem and the close_scan_pending_ guard. If a
    // post-barrier record already generated a fresh stem (because it
    // raced the barrier), scan_stem_ has been overwritten and we
    // leave it alone. Otherwise we reset so the next scan starts
    // clean.
    {
        std::lock_guard<std::mutex> stem_lk(scan_stem_mtx_);
        close_scan_pending_ = false;
        if (scan_stem_ == stem) {
            scan_stem_.clear();
        }
    }

    const bool ok = scanner_.conversion_ok && recon_.conversion_ok;
    status_.store(ok ? ConversionStatus::Complete : ConversionStatus::Failed);
    LOG_INFO("Dump conversion " << (ok ? "complete" : "FAILED"));
}

void DumpRecorder::convert_lane(Lane& lane, const std::string& stem,
                                 bool is_scanner)
{
    // Reset telemetry at the top under lane.mtx so a concurrent
    // /debug/sinks reader sees a coherent {converted_*, conversion_ok,
    // write_error} snapshot even mid-convert. counters() takes the
    // same lock on the read side.
    {
        std::lock_guard<std::mutex> lk(lane.mtx);
        lane.converted_acq = 0;
        lane.converted_img = 0;
        lane.converted_wf  = 0;
        lane.conversion_ok = true; // overwritten on failure below
    }

    if (stem.empty()) {
        // No records were ever spooled in this scan.
        return;
    }
    auto spool_path = lane.lane_dir / (stem + ".spool");
    auto h5_path    = lane.lane_dir / (stem + ".h5");
    if (!std::filesystem::exists(spool_path)) {
        // The lane never received any record this scan (e.g. recon
        // lane on a scanner-only probe).
        return;
    }
    // Conversion itself runs WITHOUT lane.mtx -- it can take seconds
    // and must not block HTTP telemetry reads. Only the telemetry
    // publish is locked.
    auto stats = convert_spool_to_hdf5(spool_path, h5_path, is_scanner);
    {
        std::lock_guard<std::mutex> lk(lane.mtx);
        lane.converted_acq = stats.acq_written;
        lane.converted_img = stats.img_written;
        lane.converted_wf  = stats.wf_written;
        lane.conversion_ok = stats.ok();
    }
    if (!stats.ok()) {
        LOG_ERROR("Spool->HDF5 conversion failed on lane="
                  << lane.component_tag
                  << " records_read=" << stats.records_read
                  << " truncated=" << stats.truncated
                  << " error=" << stats.error);
    } else if (delete_spool_after_convert_) {
        std::error_code ec;
        std::filesystem::remove(spool_path, ec);
    }
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
        // Read spool liveness + counters from atomics only. Worker
        // mutates lane.spool (unique_ptr) without holding lane.mtx,
        // so we MUST NOT dereference it here.
        const bool open_now = lane.spool_is_open.load();
        if (open_now) {
            out.spool_records = lane.live_spool_records.load();
            out.spool_bytes   = lane.live_spool_bytes.load();
            out.spool_open    = true;
        } else {
            out.spool_records = lane.last_spool_records.load();
            out.spool_bytes   = lane.last_spool_bytes.load();
            out.spool_open    = false;
        }
        // write_error and converted_* live in the coherent telemetry
        // group under lane.mtx (Fix #1).
        std::lock_guard<std::mutex> lk(lane.mtx);
        out.write_error   = lane.write_error;
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
