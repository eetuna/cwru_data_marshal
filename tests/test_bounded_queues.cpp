/*
 * tests/test_bounded_queues.cpp
 * Regression test for MEDIUM #14 + #15 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md.
 *
 * #14: LatestImageWriter queue used to be unbounded. Fix: coalesce by
 *      destination; drop oldest when 64-job cap is reached. The
 *      latest-snapshot path stays bounded — coalescing preserves only
 *      the most recent per-dest image, so "dropping" an older snapshot
 *      that was already superseded is not a correctness issue.
 *
 * #15: LiveImageRecorder queue used to be unbounded AND later briefly
 *      dropped-oldest (4096-cap). Superseded by the lossless-live
 *      change (2026-04-19): live history is now unbounded and NEVER
 *      drops, matching the dump-lossless contract clause. A one-shot
 *      high-watermark warning fires if the queue grows past 10k jobs
 *      without being drained.
 */

#include <catch2/catch_all.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>

#include "latest_image_writer.hpp"
#include "live_image_recorder.hpp"
#include "mrd_stream_tags.hpp"

namespace fs = std::filesystem;

TEST_CASE("LatestImageWriter coalesces by destination (#14)", "[latest][bounded]") {
    auto dir = fs::temp_directory_path() / "test_bounded_latest";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::LatestImageWriter writer;

    auto dest = dir / "latest_image.h5";
    // Hammer with 1000 enqueues for the same destination. Pre-fix would
    // queue all 1000; post-fix coalesces to at most 1 pending at any time.
    // Any sane implementation completes in well under a second since each
    // enqueue is a coalesce.
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        writer.enqueue(dest, "<xml/>",
                       std::vector<std::vector<uint8_t>>{{0, 1, 2, 3}},
                       nullptr);
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // 1000 coalesce operations should be fast (cap is 64; coalesce keeps
    // pending at 1 by dest). Allow 2 seconds to be extremely permissive.
    INFO("Elapsed: " << elapsed_ms << " ms for 1000 coalesced enqueues");
    REQUIRE(elapsed_ms < 2000);
}

TEST_CASE("LiveImageRecorder exposes overflow accessors (#15)",
          "[live][bounded]") {
    auto dir = fs::temp_directory_path() / "test_bounded_live";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::LiveImageRecorder rec(dir);
    REQUIRE_FALSE(rec.had_overflow());
    REQUIRE(rec.dropped_count() == 0);

    // Fresh recorder with no work cannot overflow.
    rec.close_scan();
    REQUIRE_FALSE(rec.had_overflow());
}

// Build a valid wire image body so the worker has real content to
// process rather than logging "skipping malformed live image" (noise
// in CI logs and irrelevant to the enqueue-path assertion).
static std::vector<uint8_t> make_valid_wire_image(uint16_t series,
                                                   uint16_t matrix = 16) {
    ISMRMRD::ImageHeader hdr{};
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
    hdr.matrix_size[0] = matrix;
    hdr.matrix_size[1] = matrix;
    hdr.matrix_size[2] = 1;
    hdr.channels = 1;
    hdr.image_series_index = series;
    hdr.slice = 0;

    const std::string attr;
    const uint64_t attr_len = 0;
    std::vector<float> pixels(size_t(matrix) * matrix, 1.0f);
    std::vector<uint8_t> body(mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) +
                              pixels.size() * sizeof(float));
    size_t off = 0;
    std::memcpy(body.data() + off, &hdr, mrd::IMAGE_HEADER_BYTES);
    off += mrd::IMAGE_HEADER_BYTES;
    std::memcpy(body.data() + off, &attr_len, sizeof(uint64_t));
    off += sizeof(uint64_t);
    std::memcpy(body.data() + off, pixels.data(), pixels.size() * sizeof(float));
    return body;
}

static const std::string kLossTestXml = R"(<?xml version="1.0"?>
<ismrmrdHeader xmlns="http://www.ismrmrd.org/ISMRMRD">
    <encoding><encodedSpace><matrixSize><x>256</x><y>256</y><z>1</z></matrixSize></encodedSpace></encoding>
</ismrmrdHeader>)";

TEST_CASE("LiveImageRecorder counters() is safe under concurrent polling",
          "[live][race]") {
    // Regression for codex round-5 live-path race finding:
    // LiveImageRecorder's worker dropped the queue lock before
    // running each job's lambda, then the lambda mutated sink_
    // (ensure_sink_on_worker / close_scan_on_worker). counters()
    // used to read sink_ under the queue lock -- the lock gave
    // false safety because the worker wasn't holding it.
    //
    // Post-fix: worker publishes atomics (pub_acq/img/wf_count,
    // pub_sink_open) after each successful append and at close.
    // counters() reads ONLY those atomics + queue depth under
    // mtx_. Never touches sink_ directly. This test hammers
    // counters() from a background thread while the worker is
    // opening/writing/closing the sink. Under TSan it would fire
    // at a sink_ read during worker mutation if the fix regressed.
    auto dir = fs::temp_directory_path() / "test_live_race";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::LiveImageRecorder rec(dir);

    std::atomic<bool> stop{false};
    std::atomic<size_t> poll_count{0};
    std::thread poller([&] {
        while (!stop.load()) {
            auto c = rec.counters();
            (void)c.acq; (void)c.img; (void)c.wf;
            (void)c.sink_open; (void)c.queued_jobs;
            (void)c.high_watermark_hit;
            poll_count.fetch_add(1);
        }
    });

    // Enqueue several hundred valid image records. Worker opens,
    // writes, and eventually closes sink_ at close_scan. counters()
    // must never touch sink_ during any of that.
    for (int i = 0; i < 500; ++i) {
        rec.append_image("race.h5", kLossTestXml,
                         make_valid_wire_image(static_cast<uint16_t>(i)));
    }
    rec.close_scan();

    stop.store(true);
    poller.join();

    INFO("polled " << poll_count.load() << " times");
    REQUIRE(poll_count.load() > 0);
    REQUIRE(rec.dropped_count() == 0);
}

TEST_CASE("LiveImageRecorder is lossless past the old 4096-job cap",
          "[live][lossless]") {
    // Regression for the 2026-04-19 lossless-live change.
    //
    // Round 5 design (pre-round-7): LiveImageRecorder had a bounded
    // queue that dropped oldest at 4096 entries. Round 6 removed the
    // drop path and made the queue unbounded. Round 7 replaced the
    // per-record HDF5 append worker with a raw-MRD spool + close-time
    // convert, same model as dump.
    //
    // With the spool, the worker drains at disk bandwidth so queue
    // depth stays near zero even under enqueue burst -- the old
    // "peak > 4096" proxy for "enqueuer outpaced worker" no longer
    // applies. The lossless guarantee is now verified end-to-end:
    //   (a) enqueue N records
    //   (b) close_scan runs the spool->HDF5 converter
    //   (c) the final HDF5 file has exactly N images
    //   (d) dropped_count stays 0 throughout
    auto dir = fs::temp_directory_path() / "test_live_lossless";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::LiveImageRecorder rec(dir);
    REQUIRE(rec.dropped_count() == 0);

    constexpr int kEnqueueCount = 20000;

    for (int i = 0; i < kEnqueueCount; ++i) {
        auto body = make_valid_wire_image(static_cast<uint16_t>(i & 0xFFFF));
        rec.append_image("lossless.h5", kLossTestXml, std::move(body));
        REQUIRE(rec.dropped_count() == 0);
    }

    rec.close_scan();

    REQUIRE(rec.dropped_count() == 0);
    REQUIRE_FALSE(rec.had_overflow());

    // End-to-end lossless: the converted HDF5 file should have
    // exactly kEnqueueCount images. counters() publishes the final
    // converted counts after close_scan runs convert.
    auto snap = rec.counters();
    CHECK(snap.img == static_cast<uint32_t>(kEnqueueCount));
    // +1: the scan XML header is spooled once as a METADATA_XML_TEXT
    // record (audit 2026-08-28 #11) ahead of the images.
    CHECK(snap.spool_records == static_cast<uint64_t>(kEnqueueCount) + 1);
}

// ---------------------------------------------------------------------------
// Audit 2026-08-28 #3: "published" used to mean "queued". The newest snapshot
// per destination must survive overload (superseded jobs are evicted
// instead) and must be retried on write failure; genuine loss is reported
// through the completion outcome rather than swallowed.
// ---------------------------------------------------------------------------
namespace {
std::vector<uint8_t> tiny_wire_image() {
    ISMRMRD::ImageHeader hdr{};
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
    hdr.matrix_size[0] = 2; hdr.matrix_size[1] = 2; hdr.matrix_size[2] = 1;
    hdr.channels = 1;
    std::vector<uint8_t> body(mrd::IMAGE_HEADER_BYTES + 8 + 4 * sizeof(float), 0);
    std::memcpy(body.data(), &hdr, mrd::IMAGE_HEADER_BYTES);
    return body;
}
} // namespace

TEST_CASE("LatestImageWriter keeps the newest snapshot per destination under overload",
          "[latest][bounded][audit3]") {
    auto dir = fs::temp_directory_path() / "test_bounded_latest_newest";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path dests[2] = {dir / "a" / "latest_image.h5", dir / "b" / "latest_image.h5"};

    std::mutex m;
    std::map<std::string, std::pair<int, mrd::LatestWriteOutcome>> last_outcome;  // dest -> (idx, outcome)
    int committed = 0, dropped = 0;
    mrd::LatestImageWriter::PerfSnapshot perf;
    {
        mrd::LatestImageWriter writer;
        const int N = 2000;   // >> 64-job cap; each H5 write is ~ms, so overload is certain
        for (int i = 0; i < N; ++i) {
            const auto& dest = dests[i % 2];
            writer.enqueue(dest, "<xml/>", {tiny_wire_image()},
                [&, i, dest](const fs::path&, mrd::LatestWriteOutcome o) {
                    std::lock_guard<std::mutex> lk(m);
                    if (o == mrd::LatestWriteOutcome::Committed) ++committed; else ++dropped;
                    auto& slot = last_outcome[dest.string()];
                    if (i >= slot.first) slot = {i, o};
                });
        }
        perf = writer.perf();
        // destructor drains the queue
    }
    std::lock_guard<std::mutex> lk(m);
    INFO("committed=" << committed << " dropped=" << dropped
         << " evictions=" << perf.dropped_oldest);
    REQUIRE(perf.dropped_oldest > 0);        // overload really happened
    CHECK(dropped == 0);                     // superseded jobs are evicted silently, never the newest
    for (const auto& d : dests) {
        auto it = last_outcome.find(d.string());
        REQUIRE(it != last_outcome.end());
        CHECK(it->second.first == (d == dests[0] ? 1998 : 1999));   // the final enqueue for that dest
        CHECK(it->second.second == mrd::LatestWriteOutcome::Committed);
        CHECK(fs::exists(d));
    }
}

TEST_CASE("LatestImageWriter retries the newest snapshot on write failure, then reports Failed",
          "[latest][bounded][audit3]") {
    auto dir = fs::temp_directory_path() / "test_bounded_latest_fail";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // A regular file where the parent directory should be: every write
    // attempt throws (create_directories fails, H5 open fails).
    { std::ofstream(dir / "blocker") << "x"; }
    const fs::path dest = dir / "blocker" / "latest_image.h5";

    std::mutex m;
    std::vector<mrd::LatestWriteOutcome> outcomes;
    mrd::LatestImageWriter::PerfSnapshot perf;
    {
        mrd::LatestImageWriter writer;
        writer.enqueue(dest, "<xml/>", {tiny_wire_image()},
            [&](const fs::path&, mrd::LatestWriteOutcome o) {
                std::lock_guard<std::mutex> lk(m);
                outcomes.push_back(o);
            });
        // Wait for the retries to run out.
        for (int i = 0; i < 200; ++i) {
            { std::lock_guard<std::mutex> lk(m); if (!outcomes.empty()) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        perf = writer.perf();
    }
    std::lock_guard<std::mutex> lk(m);
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0] == mrd::LatestWriteOutcome::Failed);
    CHECK(perf.failed == 3);
    CHECK(perf.retried == 2);
    CHECK(perf.lost == 1);
    CHECK(perf.completed == 0);
}

// Audit 2026-08-28 #11: live per-scan H5 files carried no MRD XML header.
TEST_CASE("LiveImageRecorder writes the scan XML header into the converted H5",
          "[live][recorder][xml]") {
    auto dir = fs::temp_directory_path() / "test_live_recorder_xml";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string xml = "<?xml version=\"1.0\"?><ismrmrdHeader><experimentalConditions>"
                            "<H1resonanceFrequency_Hz>63870000</H1resonanceFrequency_Hz>"
                            "</experimentalConditions></ismrmrdHeader>";
    {
        mrd::LiveImageRecorder rec(dir);
        // Header known only from the second record (recon lane: first image
        // can precede METADATA).
        rec.append_image("scan_xml.h5", "", tiny_wire_image());
        rec.append_image("scan_xml.h5", xml, tiny_wire_image());
        rec.close_scan();
    }
    const auto h5 = dir / "scan_xml.h5";
    REQUIRE(fs::exists(h5));
    ISMRMRD::Dataset ds(h5.c_str(), "/dataset", false);
    std::string read_back;
    ds.readHeader(read_back);
    CHECK(read_back == xml);
    CHECK(ds.getNumberOfImages("image_0") == 2);
}
