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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ismrmrd/ismrmrd.h>

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

TEST_CASE("LiveImageRecorder is lossless past the old 4096-job cap",
          "[live][lossless]") {
    // Regression for the 2026-04-19 lossless-live change. Under the
    // old design, LiveImageRecorder::enqueue erased the oldest
    // droppable pending job once queue_.size() >= 4096. That was a
    // latent contract violation; it only mattered if the enqueuer
    // outpaced the worker. Post-fix: no drop path, period.
    //
    // To deterministically exercise the old failure mode, we:
    //   (a) use a large valid HDF5 image body so each worker write
    //       takes measurable time (tens of microseconds per VLEN
    //       pixel write), forcing queue backlog when the enqueuer
    //       pushes at memory speed;
    //   (b) push records fast in a single thread and sample
    //       counters().queued_jobs; require the depth exceeds the
    //       old 4096 cap at some point -- that proves the enqueuer
    //       really did outpace the worker, which is the exact
    //       scenario that used to trigger drop-oldest;
    //   (c) require dropped_count stays 0 and had_overflow stays
    //       false throughout.
    auto dir = fs::temp_directory_path() / "test_live_lossless";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::LiveImageRecorder rec(dir);
    REQUIRE(rec.dropped_count() == 0);

    constexpr int kEnqueueCount = 20000;
    constexpr uint64_t kOldCap  = 4096;

    uint64_t peak_queue_depth = 0;
    for (int i = 0; i < kEnqueueCount; ++i) {
        auto body = make_valid_wire_image(static_cast<uint16_t>(i & 0xFFFF));
        rec.append_image("lossless.h5", kLossTestXml, std::move(body));
        // Sample every N enqueues to avoid burning cycles on counter
        // reads. Depth must exceed the old cap somewhere in the loop.
        if ((i & 0xFF) == 0) {
            auto c = rec.counters();
            if (c.queued_jobs > peak_queue_depth) peak_queue_depth = c.queued_jobs;
        }
        // Contract: dropped_count never rises, even mid-loop.
        REQUIRE(rec.dropped_count() == 0);
    }

    INFO("peak queue depth during enqueue = " << peak_queue_depth
         << " (must exceed old cap of " << kOldCap << ")");
    // Backlog must have exceeded the old cap for the test to be a
    // meaningful regression: if queued_jobs never went past 4096,
    // the drop-oldest path was never reachable either, and this test
    // would pass against the OLD buggy code too.
    REQUIRE(peak_queue_depth > kOldCap);

    REQUIRE(rec.dropped_count() == 0);
    REQUIRE_FALSE(rec.had_overflow());

    rec.close_scan();
    REQUIRE(rec.dropped_count() == 0);
}
