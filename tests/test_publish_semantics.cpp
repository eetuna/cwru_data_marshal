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
 *
 * 2026-08-28 (dry-run "no images until scan complete"): the recon group now
 * publishes INCREMENTALLY — every recon image republishes the volume
 * accumulated so far, so /image/latest never waits for the header's full
 * expected slice count. Volume boundaries (series change, repeated slice,
 * completeness) reset the group; EOF flush only covers pre-header images
 * that were never published.
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

TEST_CASE("multislice group publishes incrementally as slices arrive",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    // Every image republishes the volume accumulated so far — the viewer
    // is never left waiting for the header's full slice count.
    append_recon(state, 1, 0);
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 1);

    append_recon(state, 1, 1);
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 2);

    append_recon(state, 1, 2);   // completes the volume
    CHECK(state.latest_image_generation.load() == 3);
    CHECK(latest_image_count(snapshot) == 3);

    // The completed volume reset the group: the next image starts a fresh
    // volume and the snapshot drops back to one slice (latest-wins).
    append_recon(state, 1, 0);
    CHECK(state.latest_image_generation.load() == 4);
    CHECK(latest_image_count(snapshot) == 1);
}

TEST_CASE("repeated slice index starts a fresh group",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 2);

    // Same series, slice 0 again: a new pass over the same prescription.
    // The old group was already published incrementally; the repeat starts
    // a fresh single-image volume.
    append_recon(state, 1, 0);
    CHECK(state.latest_image_generation.load() == 3);
    CHECK(latest_image_count(snapshot) == 1);

    append_recon(state, 1, 1);
    append_recon(state, 1, 2);
    CHECK(state.latest_image_generation.load() == 5);
    CHECK(latest_image_count(snapshot) == 3);
}

TEST_CASE("series index change starts a fresh group",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 2);

    append_recon(state, 2, 0);   // new series supersedes the partial volume
    CHECK(state.latest_image_generation.load() == 3);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 1);
}

TEST_CASE("EOF flush does not republish an incrementally-published group",
          "[live_image_store][group][eof]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 2);

    // Everything the group holds already reached /image/latest: EOF must
    // not double-publish.
    mrd::flush_live_lane(state, mrd::LiveLane::Recon);
    CHECK(state.latest_image_generation.load() == 2);
    CHECK(latest_image_count(snapshot) == 2);
}

TEST_CASE("EOF flush publishes pre-header images that never published",
          "[live_image_store][group][eof]") {
    MarshalState state;
    init_state(state, 3);
    state.header_received.store(false);   // images arrive before METADATA_XML
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    append_recon(state, 1, 1);
    CHECK(state.latest_image_generation.load() == 0);
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

TEST_CASE("finalize-after-EOF does not republish, but flushes pre-header images",
          "[live_image_store][group][eof]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    // Post-header image published incrementally: finalize must not re-publish.
    append_recon(state, 1, 0);
    CHECK(state.latest_image_generation.load() == 1);
    mrd::mark_lane_finalized_after_eof(state, mrd::LiveLane::Recon);
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 1);
}

TEST_CASE("finalize-after-EOF publishes a pre-header pending group",
          "[live_image_store][group][eof]") {
    MarshalState state;
    init_state(state, 3);
    state.header_received.store(false);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    append_recon(state, 1, 0);
    CHECK(state.latest_image_generation.load() == 0);
    mrd::mark_lane_finalized_after_eof(state, mrd::LiveLane::Recon);
    CHECK(state.latest_image_generation.load() == 1);
    REQUIRE(fs::exists(snapshot));
    CHECK(latest_image_count(snapshot) == 1);
}

// 2026-08-28 audit blocker #2: the scan epoch used by the stale-publish
// guard must be the one observed in the SAME scan_mtx critical section that
// snapshotted the xml/images. publish_latest_snapshot used to load
// scan_epoch itself, after append_live_image had already unlocked — so a
// new scan's METADATA landing in that gap stamped scan-A pixels with
// scan-B's epoch and the guard passed. The epoch is now an explicit
// parameter captured under the lock; this pins that a snapshot carrying a
// superseded epoch is discarded even though the epoch was "current" when
// the publish was queued.
TEST_CASE("snapshot stamped with a superseded scan epoch is not advertised",
          "[live_image_store][epoch]") {
    MarshalState state;
    init_state(state, 1);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    // Scan A: snapshot + epoch captured together under scan_mtx.
    uint64_t epoch_a = 0;
    std::string xml_a;
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        state.current_xml_header = "<scanA/>";
        xml_a = state.current_xml_header;
        epoch_a = state.scan_epoch.load();
    }

    // Scan B's METADATA arrives before scan A's publish runs.
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        state.current_xml_header = "<scanB/>";
        state.scan_epoch.fetch_add(1);
    }

    mrd::publish_latest_snapshot(state, mrd::LiveLane::Recon, xml_a,
                                 {make_wire_image(1, 0)}, epoch_a);
    CHECK(state.latest_image_generation.load() == 0);
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path.empty());
    }

    // The same payload carrying the live epoch is advertised normally.
    mrd::publish_latest_snapshot(state, mrd::LiveLane::Recon, xml_a,
                                 {make_wire_image(1, 0)}, state.scan_epoch.load());
    CHECK(state.latest_image_generation.load() == 1);
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path == snapshot.string());
    }
}

// 2026-08-28 audit blocker #1: recon callbacks from a superseded scan. After
// an abnormal scanner EOF the scanner slot is released before the old recon
// session ends, so scan B's METADATA can bump scan_epoch while scan A's
// recon still emits tail images or fails. Those callbacks carry scan A's
// recon_session_epoch and must not archive/publish under scan B's state or
// replace scan B's latest image with the error marker.
TEST_CASE("recon image carrying a superseded epoch is neither archived nor published",
          "[live_image_store][epoch][recon]") {
    MarshalState state;
    init_state(state, 1);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    const uint64_t epoch_a = state.scan_epoch.load();
    state.recon_session_epoch.store(epoch_a);
    CHECK(mrd::recon_session_owns_scan(state));

    // Scan B's METADATA takes over.
    state.scan_epoch.fetch_add(1);
    CHECK_FALSE(mrd::recon_session_owns_scan(state));

    auto wire = make_wire_image(1, 0);
    mrd::append_live_image(state, mrd::LiveLane::Recon, wire.data(), wire.size(), epoch_a);
    CHECK(state.latest_image_generation.load() == 0);
    CHECK_FALSE(fs::exists(snapshot));
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        CHECK_FALSE(state.recon_latest_group.active);
        CHECK(state.recon_live.recorder == nullptr);   // no archive touched
    }

    // The same image stamped with the live epoch goes through.
    mrd::append_live_image(state, mrd::LiveLane::Recon, wire.data(), wire.size(),
                           state.scan_epoch.load());
    CHECK(state.latest_image_generation.load() == 1);
    CHECK(fs::exists(snapshot));
}

TEST_CASE("recon failure marker carrying a superseded epoch leaves the current image alone",
          "[live_image_store][epoch][recon][error]") {
    MarshalState state;
    init_state(state, 1);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);
    const uint64_t epoch_a = state.scan_epoch.load();

    // Scan B publishes a healthy image.
    state.scan_epoch.fetch_add(1);
    auto wire = make_wire_image(1, 0);
    mrd::append_live_image(state, mrd::LiveLane::Recon, wire.data(), wire.size());
    REQUIRE(state.latest_image_generation.load() == 1);

    // Scan A's recon fails late.
    const fs::path png = fs::path(state.dump_dir) / "latest_error.png";
    CHECK_FALSE(mrd::set_latest_image_error_at_epoch(state, png, epoch_a));
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path == snapshot.string());
        CHECK_FALSE(state.latest_image_error);
    }
    CHECK(state.latest_image_generation.load() == 1);

    // Scan B's own recon failing is still reported.
    CHECK(mrd::set_latest_image_error_at_epoch(state, png, state.scan_epoch.load()));
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        CHECK(state.latest_image_path == png.string());
        CHECK(state.latest_image_error);
    }
    CHECK(state.latest_image_generation.load() == 2);
}

// 2026-08-28 audit #7: volume grouping bounds and identity.
TEST_CASE("expected_slices_from_xml validates before narrowing", "[live_image_store][xml]") {
    CHECK(mrd::expected_slices_from_xml("<slice><minimum>0</minimum><maximum>4</maximum></slice>") == 5);
    CHECK(mrd::expected_slices_from_xml("<encodedSpace><matrixSize><z>3</z></matrixSize></encodedSpace>") == 3);
    CHECK(mrd::expected_slices_from_xml("<nothing/>") == 1);
    // Used to wrap 65535+1 -> 0 -> forced to 1 silently; now rejected explicitly.
    CHECK(mrd::expected_slices_from_xml("<slice><minimum>0</minimum><maximum>65535</maximum></slice>") == 1);
    CHECK(mrd::expected_slices_from_xml("<slice><minimum>0</minimum><maximum>99999999999999999999</maximum></slice>") == 1);
    CHECK(mrd::expected_slices_from_xml("<z>0</z>") == 1);
    CHECK(mrd::expected_slices_from_xml("<z>4096</z>") == 4096);
    CHECK(mrd::expected_slices_from_xml("<z>4097</z>") == 1);
}

static std::vector<uint8_t> make_wire_image_rep(uint16_t series, uint16_t slice, uint16_t rep) {
    auto wire = make_wire_image(series, slice);
    ISMRMRD::ImageHeader hdr;
    std::memcpy(&hdr, wire.data(), mrd::IMAGE_HEADER_BYTES);
    hdr.repetition = rep;
    std::memcpy(wire.data(), &hdr, mrd::IMAGE_HEADER_BYTES);
    return wire;
}

TEST_CASE("repetition change starts a fresh group even with the same series",
          "[live_image_store][group]") {
    MarshalState state;
    init_state(state, 3);
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    auto a = make_wire_image_rep(1, 0, 0);
    auto b = make_wire_image_rep(1, 1, 0);
    auto c = make_wire_image_rep(1, 0, 1);   // rep 1 slice 0: not the same volume
    mrd::append_live_image(state, mrd::LiveLane::Recon, a.data(), a.size());
    mrd::append_live_image(state, mrd::LiveLane::Recon, b.data(), b.size());
    CHECK(latest_image_count(snapshot) == 2);
    mrd::append_live_image(state, mrd::LiveLane::Recon, c.data(), c.size());
    CHECK(latest_image_count(snapshot) == 1);
    auto d = make_wire_image_rep(1, 1, 1);
    mrd::append_live_image(state, mrd::LiveLane::Recon, d.data(), d.size());
    CHECK(latest_image_count(snapshot) == 2);   // rep 1 accumulates on its own
}

TEST_CASE("recon group is bounded by image count and bytes regardless of the header",
          "[live_image_store][group][cap]") {
    MarshalState state;
    init_state(state, 100);   // header promises far more slices than arrive
    const auto snapshot = mrd::lane_latest_path(state, mrd::LiveLane::Recon);

    SECTION("image cap") {
        state.recon_group_max_images = 3;
        for (uint16_t s = 0; s < 3; ++s) append_recon(state, 1, s);
        CHECK(latest_image_count(snapshot) == 3);
        append_recon(state, 1, 3);   // would be the 4th: group restarts
        CHECK(latest_image_count(snapshot) == 1);
        append_recon(state, 1, 4);
        CHECK(latest_image_count(snapshot) == 2);
    }
    SECTION("byte cap") {
        const size_t one = make_wire_image(1, 0).size();
        state.recon_group_max_bytes = one * 2 + one / 2;   // room for two, not three
        append_recon(state, 1, 0);
        append_recon(state, 1, 1);
        CHECK(latest_image_count(snapshot) == 2);
        append_recon(state, 1, 2);
        CHECK(latest_image_count(snapshot) == 1);
    }
}
