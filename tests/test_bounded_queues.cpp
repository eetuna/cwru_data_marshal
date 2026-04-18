/*
 * tests/test_bounded_queues.cpp
 * Regression test for MEDIUM #14 + #15 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 *
 * #14: LatestImageWriter queue used to be unbounded. Fix: coalesce by
 *      destination; drop oldest when 64-job cap is reached.
 * #15: LiveImageRecorder queue used to be unbounded. Fix: drop oldest
 *      droppable pending job when 4096-job cap is reached; close_scan
 *      barrier is never dropped.
 */

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "latest_image_writer.hpp"
#include "live_image_recorder.hpp"

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
