/*
 * tests/test_publish_semantics.cpp
 * Regression tests for the 2026-08-18 scanner-test failures:
 *
 *  - latest_image_generation used to bump only at scan start, so
 *    /image/latest (and the /image/latest.h5 ETag) could not signal "a new
 *    image was published" — the front-end fell back to wall-clock
 *    timestamps and re-rendered the same snapshot forever. Post-fix the
 *    counter bumps once per successful publish.
 *
 *  - The recon latest-group only published on group-complete or a series
 *    index change. A recon that repeats slice indices without bumping the
 *    series (multiple passes over the same prescription) buffered forever:
 *    frozen display during the scan, burst afterwards. Post-fix a repeated
 *    slice index flushes the buffered group.
 *
 *  - A partial group still buffering at end-of-stream was silently dropped.
 *    Post-fix flush_live_lane / mark_lane_finalized publish it.
 */

#include <catch2/catch_all.hpp>

#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>

#include "marshal_state.hpp"
#include "live_image_store.hpp"

namespace fs = std::filesystem;

static std::string unique_temp_dir() {
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_publish_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

static std::vector<uint8_t> make_wire_image(uint16_t series,
                                            uint16_t slice = 0,
                                            float value = 1.0f,
                                            uint16_t nz = 1) {
    ISMRMRD::ImageHeader hdr{};
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
    hdr.matrix_size[0] = 4;
    hdr.matrix_size[1] = 4;
    hdr.matrix_size[2] = nz;
    hdr.channels = 1;
    hdr.image_series_index = series;
    hdr.slice = slice;

    std::string attr = "test";
    uint64_t attr_len = attr.size();
    std::vector<float> pixels(16 * nz, value);
    std::vector<uint8_t> body(mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) + attr.size() + pixels.size() * sizeof(float));
    size_t off = 0;
    std::memcpy(body.data() + off, &hdr, mrd::IMAGE_HEADER_BYTES);
    off += mrd::IMAGE_HEADER_BYTES;
    std::memcpy(body.data() + off, &attr_len, sizeof(uint64_t));
    off += sizeof(uint64_t);
    std::memcpy(body.data() + off, attr.data(), attr.size());
    off += attr.size();
    std::memcpy(body.data() + off, pixels.data(), pixels.size() * sizeof(float));
    return body;
}

static uint32_t latest_image_count(const fs::path& path) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    return ds.getNumberOfImages("image_0");
}

static void append_recon(MarshalState& state, uint16_t series, uint16_t slice) {
    auto wire = make_wire_image(series, slice);
    mrd::append_live_image(state, mrd::LiveLane::Recon, wire.data(), wire.size());
}

static void init_state(MarshalState& state, uint16_t expected_slices) {
    state.dump_dir = unique_temp_dir();
    state.header_received.store(true);
    state.recon_expected_slices = expected_slices;
}

TEST_CASE("generation bumps once per successful publish", "[live_image_store][generation]") {
    MarshalState state;
    init_state(state, 1);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Scanner);

    REQUIRE(state.latest_image_generation.load() == 0);

    auto wire = make_wire_image(1, 0);
    mrd::append_live_image(state, mrd::LiveLane::Scanner, wire.data(), wire.size());

    // Scanner lane publishes every image immediately (no latest_writer in
    // tests, so the write + on_complete run synchronously).
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 1);
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path == snapshot.string());
    }

    mrd::append_live_image(state, mrd::LiveLane::Scanner, wire.data(), wire.size());
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 1);   // rewritten, not appended
}

TEST_CASE("multislice group buffers without publishing until complete",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK(state.latest_image_generation.load() == 0);
    CHECK_FALSE(fs::exists(snapshot));

    append_recon(state, 1, 2);   // completes the group
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 3);
}

TEST_CASE("repeated slice index flushes the buffered group",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK_FALSE(fs::exists(snapshot));

    // Same series, slice 0 again: a new pass over the same prescription.
    // Pre-fix this buffered forever; post-fix the two-image group flushes.
    append_recon(state, 1, 0);
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 2);

    // The repeated image started a fresh group; completing it publishes.
    append_recon(state, 1, 1);
    append_recon(state, 1, 2);
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 3);
}

TEST_CASE("series index change still flushes a partial group",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    append_recon(state, 2, 0);   // new series flushes the two buffered images
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 2);
}

TEST_CASE("EOF flush publishes a pending partial group instead of dropping it",
          "[live_image_store][group][eof]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK_FALSE(fs::exists(snapshot));

    mrd::flush_live_lane(state, mrd::LiveLane::Recon);
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 2);

    // A second flush has nothing pending: no double publish.
    mrd::flush_live_lane(state, mrd::LiveLane::Recon);
    CHECK(state.latest_image_generation.load() == 1);
}

TEST_CASE("recon-failure error marker survives a queued publish",
          "[live_image_store][generation][error]") {
    MarshalState state;
    init_state(state, 1);

    // Simulate the on_failure path having set the error marker while a
    // publish sat in the writer queue: the publish must NOT overwrite the
    // marker with a healthy-looking image, and must not bump generation.
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        state.latest_image_path = "/tmp/latest_error.png";
        state.latest_image_error = true;
    }
    const auto gen_before = state.latest_image_generation.load();

    auto wire = make_wire_image(1, 0);
    mrd::append_live_image(state, mrd::LiveLane::Scanner, wire.data(), wire.size());

    CHECK(state.latest_image_generation.load() == gen_before);
    std::lock_guard<std::mutex> lk(state.latest_image_mtx);
    CHECK(state.latest_image_error);
    CHECK(state.latest_image_path == "/tmp/latest_error.png");
}

TEST_CASE("finalize-after-EOF also publishes a pending partial group",
          "[live_image_store][group][eof]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    mrd::mark_lane_finalized_after_eof(state, mrd::LiveLane::Recon);
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 1);
}
