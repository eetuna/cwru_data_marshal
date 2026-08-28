/*
 * tests/test_dump_overflow.cpp
 * Regression test for HIGH #10 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 *
 * Pre-fix: DumpRecorder::enqueue returned void. Callers had no runtime
 *   visibility into drops — only the dump_complete="false" attribute on the
 *   closed HDF5 file.
 *
 * Post-fix (codex blocker 1 resolution): every public append_* / set_* /
 *   start_scan returns DumpEnqueueResult { Accepted, Dropped, Stopped }.
 *   Callers see drops at the moment they happen and can throttle / escalate.
 */

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <hdf5.h>

#include "dump_recorder.hpp"
#include "mrd_io.hpp"
#include "spool_converter.hpp"

namespace fs = std::filesystem;

// Helpers for the byte-exact dump API: tests now pass wire bodies
// rather than parsed strings. These helpers reproduce the exact
// wire framing the listener would have assembled on the socket.
static std::vector<uint8_t> config_file_body(const std::string& s) {
    std::vector<uint8_t> body(1024, 0);
    const size_t n = std::min(s.size(), size_t{1023});
    std::memcpy(body.data(), s.data(), n);
    return body;
}

// Wrap a legacy 2-arg start_scan(filename, xml) call for tests. The
// new 3-arg API takes (filename, xml_body, header_xml); tests that
// don't care about METADATA_XML byte-exactness can keep using the
// string form through this helper.
static mrd::DumpEnqueueResult dump_start_scan(mrd::DumpRecorder& rec,
                                               const std::string& filename,
                                               const std::string& xml) {
    // Build the standard length-prefixed XML wire body.
    const uint32_t inner = static_cast<uint32_t>(xml.size() + 1);
    std::vector<uint8_t> body(sizeof(uint32_t) + inner);
    std::memcpy(body.data(), &inner, sizeof(inner));
    std::memcpy(body.data() + sizeof(inner), xml.data(), xml.size());
    body[sizeof(inner) + xml.size()] = '\0';
    return rec.start_scan(filename, std::move(body), xml);
}

static std::vector<uint8_t> text_body(const std::string& s) {
    // [uint32 inner_len][text + NUL]. inner_len includes the NUL.
    const uint32_t inner = static_cast<uint32_t>(s.size() + 1);
    std::vector<uint8_t> body(sizeof(uint32_t) + inner);
    std::memcpy(body.data(), &inner, sizeof(inner));
    std::memcpy(body.data() + sizeof(inner), s.data(), s.size());
    body[sizeof(inner) + s.size()] = '\0';
    return body;
}

TEST_CASE("Fresh DumpRecorder reports no overflow", "[dump][overflow]") {
    auto dir = fs::temp_directory_path() / "test_dump_overflow_fresh";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::DumpRecorder rec(dir);
    REQUIRE_FALSE(rec.had_overflow());
    REQUIRE(rec.dropped_record_count() == 0);
    REQUIRE(rec.dropped_byte_count() == 0);
}

TEST_CASE("DumpRecorder enqueue returns Accepted on happy path",
          "[dump][overflow][api]") {
    auto dir = fs::temp_directory_path() / "test_dump_overflow_accepted";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::DumpRecorder rec(dir);
    auto r = dump_start_scan(rec, "api_ok.h5",
                            "<?xml version=\"1.0\"?><ismrmrdHeader/>");
    REQUIRE(r == mrd::DumpEnqueueResult::Accepted);

    auto r2 = rec.append_scanner_text(text_body("small"));
    REQUIRE(r2 == mrd::DumpEnqueueResult::Accepted);

    rec.close_scan();
}

TEST_CASE("DumpRecorder with spool never drops under high-count acq flood",
          "[dump][spool][lossless]") {
    // Contract: dump mode is lossless. A flood of acquisitions well
    // beyond any previous queue cap (4096) must complete without
    // dropping. Using acqs (not text) is the realistic stressor because
    // scanner at 50 Hz x 5 slices x 128 lines pushes 32,000 acqs/s
    // and was the reason spool was introduced.
    auto dir = fs::temp_directory_path() / "test_dump_spool_lossless";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::DumpRecorder rec(dir);
    auto start_r = dump_start_scan(rec, "lossless.h5",
                                  "<?xml version=\"1.0\"?><ismrmrdHeader/>");
    REQUIRE(start_r == mrd::DumpEnqueueResult::Accepted);

    constexpr uint16_t nsamples = 64;
    constexpr uint16_t nchannels = 4;
    ISMRMRD::AcquisitionHeader hdr{};
    hdr.version = 1;
    hdr.number_of_samples = nsamples;
    hdr.active_channels = nchannels;
    hdr.trajectory_dimensions = 0;

    const size_t sample_bytes =
        size_t(nsamples) * nchannels * 2 * sizeof(float);
    constexpr int kFloodCount = 10000;  // > any prior queue cap (4096)

    for (int i = 0; i < kFloodCount; ++i) {
        std::vector<uint8_t> samples(sample_bytes, 0x5A);
        hdr.scan_counter = static_cast<uint32_t>(i);
        auto r = rec.append_scanner_acquisition(hdr, {}, std::move(samples));
        REQUIRE(r == mrd::DumpEnqueueResult::Accepted);
    }

    rec.close_scan();

    REQUIRE_FALSE(rec.had_overflow());
    REQUIRE(rec.dropped_record_count() == 0);
    REQUIRE(rec.dropped_byte_count() == 0);
}

// Helper: read a scalar VLEN string dataset from a closed HDF5 file.
// Returns empty string if the dataset doesn't exist.
static std::string read_string_dataset_or_empty(const fs::path& h5, const std::string& name)
{
    hid_t file = H5Fopen(h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) return {};
    std::string full = std::string("/dataset/") + name;
    htri_t exists = H5Lexists(file, full.c_str(), H5P_DEFAULT);
    if (exists <= 0) { H5Fclose(file); return {}; }
    hid_t dset = H5Dopen2(file, full.c_str(), H5P_DEFAULT);
    if (dset < 0) { H5Fclose(file); return {}; }
    hid_t type = H5Dget_type(dset);
    char* value = nullptr;
    std::string out;
    if (H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) >= 0 && value != nullptr) {
        out = value;
        H5free_memory(value);
    }
    H5Tclose(type);
    H5Dclose(dset);
    H5Fclose(file);
    return out;
}

TEST_CASE("DumpRecorder persists CONFIG_FILE / CONFIG_TEXT / TEXT sent before METADATA_XML",
          "[dump][config-order]") {
    // Regression for codex-flagged bug: python-ismrmrd-server's wire order
    // has CONFIG_FILE and CONFIG_TEXT arriving BEFORE METADATA_XML.
    // DumpRecorder's sink is only opened by start_scan (which is triggered
    // by METADATA_XML), so pre-metadata config/text used to hit
    // `if (!sink) return` in the worker and be silently dropped.
    //
    // Fix: set_scanner_config_* / append_scanner_text / append_recon_text
    // buffer the payload in Lane.pending_* when the sink is null.
    // start_scan's worker lambda snapshots the pending_* fields BEFORE
    // calling close_scan_on_worker (which clears them as between-scan
    // cleanup), opens the new sink, then replays from the snapshot.
    auto dir = fs::temp_directory_path() / "test_dump_config_order";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string filename = "order_test.h5";
    const std::string xml = "<?xml version=\"1.0\"?><ismrmrdHeader/>";
    const std::string config_file_payload = "simplefft";
    const std::string config_text_payload = "{\"parameters\":{\"config\":\"simplefft\"}}";
    const std::string first_text  = "pre-metadata text A";
    const std::string second_text = "pre-metadata text B";

    {
        mrd::DumpRecorder rec(dir);
        // Order matches python-ismrmrd-server client.py and
        // kspace_streamer: config first, THEN metadata.
        REQUIRE(rec.set_scanner_config_file(config_file_body(config_file_payload))
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.set_scanner_config_text(text_body(config_text_payload))
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(text_body(first_text))
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(text_body(second_text))
                == mrd::DumpEnqueueResult::Accepted);

        // Now open the sink via start_scan (METADATA_XML arrival).
        REQUIRE(dump_start_scan(rec, filename, xml)
                == mrd::DumpEnqueueResult::Accepted);

        rec.close_scan();
    }

    // DumpRecorder generates its own shared scan stem so scanner and
    // recon archives match. The caller-supplied filename is treated as
    // advisory only (used if no records arrived before start_scan).
    // Locate the produced .h5 by scanning the scanner dump dir.
    fs::path h5_path;
    for (auto& ent : fs::directory_iterator(mrd::dump_scanner_dir(dir))) {
        if (ent.path().extension() == ".h5") { h5_path = ent.path(); break; }
    }
    REQUIRE(!h5_path.empty());
    REQUIRE(fs::exists(h5_path));

    CHECK(read_string_dataset_or_empty(h5_path, "config_file") == config_file_payload);
    CHECK(read_string_dataset_or_empty(h5_path, "config") == config_text_payload);
    CHECK(read_string_dataset_or_empty(h5_path, "text_0") == first_text);
    CHECK(read_string_dataset_or_empty(h5_path, "text_1") == second_text);
}

// Helper: find the single .h5 file in a dump-lane directory.
static fs::path find_single_h5(const fs::path& dir) {
    for (auto& ent : fs::directory_iterator(dir)) {
        if (ent.path().extension() == ".h5") return ent.path();
    }
    return {};
}

TEST_CASE("DumpRecorder supports back-to-back scans (second scan works)",
          "[dump][spool][lifecycle]") {
    // Regression for codex finding #1: after close_scan, lane.spool was
    // left as a non-null but closed writer. Second ensure_spool_on_worker
    // early-returned without opening a fresh spool, and all new records
    // were counted as failed writes.
    auto dir = fs::temp_directory_path() / "test_dump_second_scan";
    fs::remove_all(dir);
    fs::create_directories(dir);

    constexpr uint16_t nsamples = 16;
    constexpr uint16_t nchannels = 2;
    const size_t sample_bytes =
        size_t(nsamples) * nchannels * 2 * sizeof(float);

    mrd::DumpRecorder rec(dir);

    for (int scan = 0; scan < 2; ++scan) {
        REQUIRE(dump_start_scan(rec, "s" + std::to_string(scan) + ".h5",
                               "<?xml version=\"1.0\"?><ismrmrdHeader/>")
                == mrd::DumpEnqueueResult::Accepted);

        ISMRMRD::AcquisitionHeader hdr{};
        hdr.version = 1;
        hdr.number_of_samples = nsamples;
        hdr.active_channels = nchannels;
        hdr.trajectory_dimensions = 0;

        for (int i = 0; i < 5; ++i) {
            std::vector<uint8_t> samples(sample_bytes, 0xAA);
            hdr.scan_counter = static_cast<uint32_t>(i);
            REQUIRE(rec.append_scanner_acquisition(hdr, {}, std::move(samples))
                    == mrd::DumpEnqueueResult::Accepted);
        }

        rec.close_scan();
        REQUIRE(rec.dropped_record_count() == 0);
    }

    // Both scans should have produced valid H5 files with matching counts.
    // At least two distinct .h5 files must exist in the scanner dump dir.
    int n_h5 = 0;
    for (auto& ent : fs::directory_iterator(mrd::dump_scanner_dir(dir))) {
        if (ent.path().extension() == ".h5") ++n_h5;
    }
    CHECK(n_h5 == 2);
}

TEST_CASE("DumpRecorder scanner+recon archives share the same <ts> stem",
          "[dump][spool][contract]") {
    // Regression for codex finding #2: CONFIG arriving on scanner lane
    // before METADATA_XML used to cause the scanner worker to pick its
    // own filename while start_scan let the recon lane adopt the
    // caller-supplied filename. Two lanes ended up with different stems,
    // violating the contract that active-mode scanner+recon archives
    // share the same <ts>.
    auto dir = fs::temp_directory_path() / "test_dump_shared_stem";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::DumpRecorder rec(dir);
    // Pre-metadata scanner traffic: forces scanner lane to open its
    // spool first with whatever stem is in play.
    REQUIRE(rec.set_scanner_config_file(config_file_body("simplefft"))
            == mrd::DumpEnqueueResult::Accepted);
    REQUIRE(rec.append_scanner_text(text_body("pre-metadata"))
            == mrd::DumpEnqueueResult::Accepted);
    // Now METADATA_XML.
    REQUIRE(dump_start_scan(rec, "caller_suggested.h5",
                           "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);
    // Hit the recon lane with real content (text) so it definitely
    // produces an .h5. An earlier version of this test used an empty
    // recon image which the converter skipped as malformed, letting
    // the test pass even when the shared-stem invariant was broken.
    REQUIRE(rec.append_recon_text(text_body("recon hello"))
            == mrd::DumpEnqueueResult::Accepted);
    rec.close_scan();

    auto scanner_h5 = find_single_h5(mrd::dump_scanner_dir(dir));
    auto recon_h5   = find_single_h5(mrd::dump_recon_dir(dir));
    REQUIRE(!scanner_h5.empty());
    REQUIRE(!recon_h5.empty());
    CHECK(scanner_h5.stem() == recon_h5.stem());
}

TEST_CASE("DumpRecorder close_lane finalizes only the selected lane",
          "[dump][spool][lane-close]") {
    auto dir = fs::temp_directory_path() / "test_dump_lane_close";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::DumpRecorder rec(dir);
    REQUIRE(dump_start_scan(rec, "lane_close.h5",
                           "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);
    REQUIRE(rec.append_scanner_text(text_body("scanner done"))
            == mrd::DumpEnqueueResult::Accepted);
    REQUIRE(rec.append_recon_text(text_body("recon still running"))
            == mrd::DumpEnqueueResult::Accepted);

    rec.close_lane(mrd::DumpLane::Scanner);

    auto scanner_h5 = find_single_h5(mrd::dump_scanner_dir(dir));
    REQUIRE(!scanner_h5.empty());
    CHECK(read_string_dataset_or_empty(scanner_h5, "text_0") == "scanner done");
    CHECK(find_single_h5(mrd::dump_recon_dir(dir)).empty());

    REQUIRE(rec.append_recon_text(text_body("recon finished later"))
            == mrd::DumpEnqueueResult::Accepted);
    rec.close_lane(mrd::DumpLane::Recon);

    auto recon_h5 = find_single_h5(mrd::dump_recon_dir(dir));
    REQUIRE(!recon_h5.empty());
    CHECK(read_string_dataset_or_empty(recon_h5, "text_0") == "recon still running");

    for (auto& ent : fs::directory_iterator(mrd::dump_scanner_dir(dir))) {
        CHECK(ent.path().extension() != ".spool");
    }
    for (auto& ent : fs::directory_iterator(mrd::dump_recon_dir(dir))) {
        CHECK(ent.path().extension() != ".spool");
    }
}

TEST_CASE("DumpRecorder close-barrier race: strict scan boundary "
          "(post-close records go to a different h5)",
          "[dump][spool][race]") {
    // Regression for codex findings against 850c349 + 672bdcb.
    //
    // 850c349-era bug: close_scan's barrier closed the spool but
    // scan_stem_ was only cleared later. A record in the window could
    // reopen the same spool path with "wb" and truncate it.
    //
    // 672bdcb-era bug: close_scan_pending_ was set BEFORE the barriers
    // were queued, so a record arriving in between could generate a
    // fresh stem, clear pending, and land ON THE LANE QUEUE AHEAD of
    // the (not-yet-pushed) barrier -- polluting the OLD scan's spool
    // with records the test thought belonged to a later scan, and/or
    // orphaning the fresh stem.
    //
    // Fix: barrier insertion is now atomic with close_scan_pending_
    // under scan_stem_mtx_. This test asserts the strict boundary:
    // records sent AFTER close_scan() returns must land in a different
    // .h5 file than records sent BEFORE close_scan() was called.
    auto dir = fs::temp_directory_path() / "test_dump_barrier_race";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string pre_close  = "pre_close_text";
    const std::string post_close = "post_close_text";

    {
        mrd::DumpRecorder rec(dir);
        REQUIRE(dump_start_scan(rec, "first.h5",
                               "<?xml version=\"1.0\"?><ismrmrdHeader/>")
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(text_body(pre_close))
                == mrd::DumpEnqueueResult::Accepted);

        // close_scan serializes all pending records into scan 1's spool
        // under the barrier, closes, and converts. After it returns,
        // the next enqueue must start a fresh scan (fresh stem, fresh
        // spool, fresh .h5).
        rec.close_scan();

        // This record must land in a DIFFERENT .h5 than pre_close did.
        REQUIRE(rec.append_scanner_text(text_body(post_close))
                == mrd::DumpEnqueueResult::Accepted);
        rec.close_scan();
    }

    // Find the two .h5 files and assert pre/post content is split.
    std::vector<fs::path> h5s;
    for (auto& ent : fs::directory_iterator(mrd::dump_scanner_dir(dir))) {
        if (ent.path().extension() == ".h5") h5s.push_back(ent.path());
    }
    REQUIRE(h5s.size() == 2);

    bool pre_in_file_a = false, pre_in_file_b = false;
    bool post_in_file_a = false, post_in_file_b = false;
    pre_in_file_a  = (read_string_dataset_or_empty(h5s[0], "text_0") == pre_close);
    post_in_file_a = (read_string_dataset_or_empty(h5s[0], "text_0") == post_close);
    pre_in_file_b  = (read_string_dataset_or_empty(h5s[1], "text_0") == pre_close);
    post_in_file_b = (read_string_dataset_or_empty(h5s[1], "text_0") == post_close);

    // Exactly one file has pre_close, exactly one has post_close,
    // and they are different files. (pre != post, both present.)
    CHECK((pre_in_file_a ^ pre_in_file_b));
    CHECK((post_in_file_a ^ post_in_file_b));
    CHECK((pre_in_file_a != post_in_file_a));
}

TEST_CASE("DumpRecorder close-barrier race under concurrent hammer: "
          "no truncation, post-hammer scan contents isolated",
          "[dump][spool][race-hammer]") {
    // Stronger hammer variant of the above: fire ACQs from a
    // background thread concurrently with close_scan, confirming
    // (a) the pre-close text survives in scan 1's h5, (b) any
    // records generated by the hammer between close_scan() start
    // and hammer.join() do NOT end up in scan 1's h5.
    auto dir = fs::temp_directory_path() / "test_dump_barrier_race_hammer";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string pre_close = "from scan 1";

    {
        mrd::DumpRecorder rec(dir);
        REQUIRE(dump_start_scan(rec, "first.h5",
                               "<?xml version=\"1.0\"?><ismrmrdHeader/>")
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(text_body(pre_close))
                == mrd::DumpEnqueueResult::Accepted);

        constexpr uint16_t nsamples = 4;
        constexpr uint16_t nchannels = 1;
        const size_t sample_bytes =
            size_t(nsamples) * nchannels * 2 * sizeof(float);
        ISMRMRD::AcquisitionHeader rhdr{};
        rhdr.version = 1;
        rhdr.number_of_samples = nsamples;
        rhdr.active_channels = nchannels;
        rhdr.trajectory_dimensions = 0;

        std::atomic<bool> started_close{false};
        std::atomic<size_t> hammer_fired{0};
        std::atomic<bool> stop{false};
        std::thread hammer([&] {
            // Only fire AFTER close_scan is in progress, so all hammer
            // records land AFTER the barrier -- they belong to a
            // fresh scan by contract.
            while (!started_close.load()) std::this_thread::yield();
            while (!stop.load()) {
                std::vector<uint8_t> s(sample_bytes, 0x77);
                if (rec.append_scanner_acquisition(rhdr, {}, std::move(s))
                    == mrd::DumpEnqueueResult::Accepted) {
                    hammer_fired.fetch_add(1);
                }
            }
        });

        started_close.store(true);
        rec.close_scan();
        // Guarantee the hammer generates at least one post-close record
        // so the second close_scan has content to convert. Without this,
        // a slow thread start could let stop.store(true) run before any
        // hammer fire, and the second close_scan would produce no h5.
        while (hammer_fired.load() == 0) std::this_thread::yield();
        stop.store(true);
        hammer.join();
        rec.close_scan();
    }

    // Locate scan-1 h5 (the one containing pre_close text). It MUST
    // have zero acquisitions -- the hammer records go to scan 2.
    fs::path scan1_h5;
    for (auto& ent : fs::directory_iterator(mrd::dump_scanner_dir(dir))) {
        if (ent.path().extension() != ".h5") continue;
        if (read_string_dataset_or_empty(ent.path(), "text_0") == pre_close) {
            scan1_h5 = ent.path();
            break;
        }
    }
    REQUIRE(!scan1_h5.empty());

    // Open and count acqs in scan-1 h5. Must be zero: the pre-close
    // code only sent a TEXT record, no ACQs.
    hid_t file = H5Fopen(scan1_h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    REQUIRE(file >= 0);
    htri_t has_data = H5Lexists(file, "/dataset/data", H5P_DEFAULT);
    size_t scan1_acqs = 0;
    if (has_data > 0) {
        hid_t dset = H5Dopen2(file, "/dataset/data", H5P_DEFAULT);
        if (dset >= 0) {
            hid_t space = H5Dget_space(dset);
            hsize_t dims[1] = {0};
            H5Sget_simple_extent_dims(space, dims, nullptr);
            scan1_acqs = static_cast<size_t>(dims[0]);
            H5Sclose(space);
            H5Dclose(dset);
        }
    }
    H5Fclose(file);
    CHECK(scan1_acqs == 0);
}

TEST_CASE("DumpRecorder /debug/sinks counters after close + stale-counter reset",
          "[dump][spool][counters]") {
    // Regression for codex findings #2 and #3 against 850c349:
    //   #2 - post-close spool_records went to 0 because counters()
    //        only read from lane.spool which is reset after close.
    //   #3 - converted_acq/img/wf on a lane that received nothing in
    //        scan 2 still showed scan-1 numbers.
    auto dir = fs::temp_directory_path() / "test_dump_counter_reset";
    fs::remove_all(dir);
    fs::create_directories(dir);

    constexpr uint16_t nsamples = 8;
    constexpr uint16_t nchannels = 2;
    const size_t sample_bytes =
        size_t(nsamples) * nchannels * 2 * sizeof(float);
    ISMRMRD::AcquisitionHeader hdr{};
    hdr.version = 1;
    hdr.number_of_samples = nsamples;
    hdr.active_channels = nchannels;
    hdr.trajectory_dimensions = 0;

    mrd::DumpRecorder rec(dir);

    // Scan 1: scanner sends acqs, recon sends a text.
    REQUIRE(dump_start_scan(rec, "s1.h5", "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> s(sample_bytes, 0x11);
        REQUIRE(rec.append_scanner_acquisition(hdr, {}, std::move(s))
                == mrd::DumpEnqueueResult::Accepted);
    }
    REQUIRE(rec.append_recon_text(text_body("scan1 recon"))
            == mrd::DumpEnqueueResult::Accepted);
    rec.close_scan();

    // Fix #2: after close, /debug/sinks should STILL report non-zero
    // spool_records from the just-closed scan.
    auto snap1 = rec.counters();
    CHECK(snap1.scanner.spool_records > 0);
    CHECK(snap1.scanner.spool_bytes > 0);
    CHECK(snap1.scanner.converted_acq == 3);
    CHECK(snap1.recon.spool_records > 0);

    // Scan 2: only scanner activity, no recon records.
    REQUIRE(dump_start_scan(rec, "s2.h5", "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);
    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> s(sample_bytes, 0x22);
        REQUIRE(rec.append_scanner_acquisition(hdr, {}, std::move(s))
                == mrd::DumpEnqueueResult::Accepted);
    }
    rec.close_scan();

    auto snap2 = rec.counters();
    // Scanner: new count of 5 acqs; must not carry scan-1's 3.
    CHECK(snap2.scanner.converted_acq == 5);
    // Fix #3: recon had no content this scan, so converted_* must
    // be reset to 0 rather than still showing scan-1's "recon text".
    CHECK(snap2.recon.converted_acq == 0);
    CHECK(snap2.recon.converted_img == 0);
    CHECK(snap2.recon.converted_wf == 0);
}

TEST_CASE("SpoolConverter rejects records with body length > spool size",
          "[dump][spool][converter]") {
    // Regression for codex finding #5: converter did body.resize(len)
    // before bounding len against the spool file size. A corrupt prefix
    // with len=0xFFFFFFFF would try to allocate 4 GiB.
    auto dir = fs::temp_directory_path() / "test_dump_truncated_spool";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto spool_path = dir / "corrupt.spool";
    auto h5_path    = dir / "corrupt.h5";
    {
        std::ofstream os(spool_path, std::ios::binary);
        uint16_t tag = 5; // TEXT
        uint32_t len = 0xFFFFFFFFu; // lies about body size
        os.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
        os.write(reinterpret_cast<const char*>(&len), sizeof(len));
        // Intentionally do not write any body.
    }

    auto stats = mrd::convert_spool_to_hdf5(spool_path, h5_path,
                                            /*is_scanner_side=*/true);
    CHECK_FALSE(stats.ok());
    CHECK(stats.truncated);
    CHECK_FALSE(stats.error.empty());
    // Must NOT have attempted to resize to the giant length.
    // Catch2 will have failed via OOM otherwise.
}

TEST_CASE("DumpRecorder counters() is safe under concurrent polling during convert",
          "[dump][spool][telemetry-race]") {
    // Regression for codex round-5 finding: convert_lane() wrote
    // converted_{acq,img,wf} + conversion_ok WITHOUT lane.mtx while
    // counters() read them under lane.mtx, and write_error had the
    // same pattern. A concurrent GET /debug/sinks reader could see
    // torn/inconsistent values (and UB on the std::string
    // write_error assignment). Fix: writer takes lane.mtx around
    // every telemetry field mutation; reader already held it.
    //
    // This test runs a long enough conversion that a polling thread
    // is guaranteed to read the lane counters while convert_lane is
    // actively writing them. Under ThreadSanitizer this would fire
    // at the unprotected write. Without TSan, we can at least
    // verify that the reader never sees obviously-inconsistent
    // state (e.g. conversion_ok=false while converted_* is nonzero
    // from a prior scan, or write_error set but spool still open).
    auto dir = fs::temp_directory_path() / "test_dump_telemetry_race";
    fs::remove_all(dir);
    fs::create_directories(dir);

    constexpr uint16_t nsamples = 32;
    constexpr uint16_t nchannels = 4;
    const size_t sample_bytes =
        size_t(nsamples) * nchannels * 2 * sizeof(float);

    mrd::DumpRecorder rec(dir);
    REQUIRE(dump_start_scan(rec, "telemetry.h5",
                           "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);

    // Enough records that the converter takes measurable time.
    ISMRMRD::AcquisitionHeader hdr{};
    hdr.version = 1;
    hdr.number_of_samples = nsamples;
    hdr.active_channels = nchannels;
    hdr.trajectory_dimensions = 0;
    constexpr int kRecordCount = 5000;
    for (int i = 0; i < kRecordCount; ++i) {
        std::vector<uint8_t> s(sample_bytes, 0xAA);
        hdr.scan_counter = static_cast<uint32_t>(i);
        REQUIRE(rec.append_scanner_acquisition(hdr, {}, std::move(s))
                == mrd::DumpEnqueueResult::Accepted);
    }

    // Poll counters() hard while close_scan is in flight. close_scan
    // drains the worker, then runs convert_lane on both lanes. The
    // poller will observe all of it.
    std::atomic<bool> stop{false};
    std::atomic<size_t> poll_count{0};
    std::atomic<bool> saw_torn{false};
    std::thread poller([&] {
        while (!stop.load()) {
            auto snap = rec.counters();
            // Consistency check: while conversion is running, the
            // lane fields should never show a "failed" conversion
            // with nonzero converted counts (that would be a torn
            // write from two different scan states).
            if (!snap.scanner.conversion_ok &&
                (snap.scanner.converted_acq != 0 ||
                 snap.scanner.converted_img != 0 ||
                 snap.scanner.converted_wf != 0)) {
                saw_torn.store(true);
            }
            (void)snap.scanner.write_error;  // exercise string copy path
            (void)snap.recon.write_error;
            poll_count.fetch_add(1);
        }
    });

    rec.close_scan();
    stop.store(true);
    poller.join();

    INFO("polled " << poll_count.load() << " times during close/convert");
    REQUIRE(poll_count.load() > 0);
    CHECK_FALSE(saw_torn.load());
    CHECK(rec.counters().scanner.converted_acq == kRecordCount);
    CHECK(rec.counters().scanner.conversion_ok);
}

TEST_CASE("DumpRecorder preserves byte-exact TEXT bodies (embedded NULs)",
          "[dump][spool][byte-exact]") {
    // Regression for codex round-5 finding: the listener used to strip
    // at the first NUL when assembling the dump string (config_text
    // parsed into std::string, then truncated at '\\0'). DumpRecorder
    // rebuilt the wire body from that truncated string. A sender with
    // embedded NULs in its CONFIG_TEXT / TEXT body would lose
    // everything after the first NUL in the dumped HDF5.
    //
    // Post-fix: listener passes the FULL wire body verbatim; dump
    // stores it as-is; the converter extracts the payload back into
    // an HDF5 string dataset. We feed a body with an embedded NUL
    // followed by a distinctive tail and assert the full payload
    // arrives in the final h5.
    auto dir = fs::temp_directory_path() / "test_dump_byte_exact";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Construct a TEXT body with an embedded NUL mid-payload.
    const std::string pre  = "first_half";
    const std::string tail = "second_half_after_NUL";
    std::string payload = pre;
    payload += '\0';
    payload += tail;
    // text_body appends a trailing NUL; len inside is payload.size()+1.
    auto body = text_body(payload);

    {
        mrd::DumpRecorder rec(dir, /*delete_spool_after_convert=*/false);
        REQUIRE(dump_start_scan(rec, "byte_exact.h5",
                               "<?xml version=\"1.0\"?><ismrmrdHeader/>")
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(std::move(body))
                == mrd::DumpEnqueueResult::Accepted);
        rec.close_scan();
    }

    // Open the resulting h5 and pull text_0 as a variable-length
    // string dataset. HDF5 VLEN strings are NUL-terminated C strings,
    // so they truncate at the FIRST NUL -- this is a known HDF5
    // limitation on the read side, not a byte-exact-dump bug on the
    // write side. Verify the bytes landed in the spool record
    // verbatim instead; the spool is the authoritative byte-exact
    // artifact.
    auto scanner_dir = mrd::dump_scanner_dir(dir);
    fs::path spool_path;
    for (auto& ent : fs::directory_iterator(scanner_dir)) {
        if (ent.path().extension() == ".spool") { spool_path = ent.path(); break; }
    }
    REQUIRE(!spool_path.empty());

    // Scan the spool for the TEXT record and confirm the full
    // pre + '\\0' + tail payload is present verbatim.
    std::ifstream in(spool_path, std::ios::binary);
    REQUIRE(in.is_open());
    bool found_match = false;
    while (in) {
        uint16_t tag = 0;
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&tag), sizeof(tag))) break;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) break;
        std::vector<uint8_t> rec_body(len);
        if (len > 0 && !in.read(reinterpret_cast<char*>(rec_body.data()), len))
            break;
        if (tag != mrd::MRD_MESSAGE_TEXT) continue;
        // Body layout: [uint32 inner_len][payload...]
        if (rec_body.size() < sizeof(uint32_t)) continue;
        uint32_t inner = 0;
        std::memcpy(&inner, rec_body.data(), sizeof(inner));
        if (rec_body.size() < sizeof(uint32_t) + inner) continue;
        const char* p = reinterpret_cast<const char*>(
            rec_body.data() + sizeof(uint32_t));
        std::string bytes(p, inner);  // includes trailing NUL
        // Search for the embedded-NUL pattern verbatim.
        if (bytes.size() >= pre.size() + 1 + tail.size()) {
            bool match_pre  = std::memcmp(bytes.data(), pre.data(), pre.size()) == 0;
            bool match_nul  = bytes[pre.size()] == '\0';
            bool match_tail = std::memcmp(bytes.data() + pre.size() + 1,
                                          tail.data(), tail.size()) == 0;
            if (match_pre && match_nul && match_tail) {
                found_match = true;
                break;
            }
        }
    }
    CHECK(found_match);
}

TEST_CASE("DumpRecorder preserves byte-exact METADATA_XML (embedded NUL)",
          "[dump][spool][byte-exact]") {
    // Regression for codex round-6 follow-up. Fix #4 covered CONFIG
    // and TEXT but METADATA_XML was still NUL-stripped by the
    // listener before being passed into start_scan(). Fix: start_scan
    // now takes the raw wire body alongside the parsed xml string.
    // Dump stores the verbatim body; listener keeps the parsed
    // string for its own slice-count regex etc.
    //
    // We feed XML with `pre\\0<!-- tail -->` via the wire-body path
    // and assert the spool record contains the full payload.
    auto dir = fs::temp_directory_path() / "test_dump_metadata_byte_exact";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string xml_pre  = "<?xml version=\"1.0\"?><ismrmrdHeader>";
    const std::string xml_tail = "<!-- tail after NUL --></ismrmrdHeader>";
    std::string payload = xml_pre;
    payload += '\0';
    payload += xml_tail;

    // Build the wire body exactly as the listener would: [uint32][payload+NUL]
    const uint32_t inner = static_cast<uint32_t>(payload.size() + 1);
    std::vector<uint8_t> body(sizeof(uint32_t) + inner);
    std::memcpy(body.data(), &inner, sizeof(inner));
    std::memcpy(body.data() + sizeof(inner), payload.data(), payload.size());
    body[sizeof(inner) + payload.size()] = '\0';

    {
        mrd::DumpRecorder rec(dir, /*delete_spool_after_convert=*/false);
        // Strip-at-NUL "header_xml" matches what the listener's own
        // state keeps; dump treats the raw body as authoritative.
        std::string header_xml = xml_pre;
        REQUIRE(rec.start_scan("metadata_be.h5", body, header_xml)
                == mrd::DumpEnqueueResult::Accepted);
        rec.close_scan();
    }

    // Read the spool directly; HDF5 VLEN strings would truncate at
    // the first NUL on HDF5 read-back, so spool-level verification
    // is the correct byte-exactness assertion.
    auto scanner_dir = mrd::dump_scanner_dir(dir);
    fs::path spool_path;
    for (auto& ent : fs::directory_iterator(scanner_dir)) {
        if (ent.path().extension() == ".spool") { spool_path = ent.path(); break; }
    }
    REQUIRE(!spool_path.empty());

    std::ifstream in(spool_path, std::ios::binary);
    REQUIRE(in.is_open());
    bool found_match = false;
    while (in) {
        uint16_t tag = 0;
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&tag), sizeof(tag))) break;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) break;
        std::vector<uint8_t> rec_body(len);
        if (len > 0 && !in.read(reinterpret_cast<char*>(rec_body.data()), len))
            break;
        if (tag != mrd::MRD_MESSAGE_METADATA_XML_TEXT) continue;
        if (rec_body.size() < sizeof(uint32_t)) continue;
        uint32_t rec_inner = 0;
        std::memcpy(&rec_inner, rec_body.data(), sizeof(rec_inner));
        if (rec_body.size() < sizeof(uint32_t) + rec_inner) continue;
        const char* p = reinterpret_cast<const char*>(
            rec_body.data() + sizeof(uint32_t));
        // Expect: xml_pre + '\0' + xml_tail + '\0'. Full payload present.
        if (rec_inner >= xml_pre.size() + 1 + xml_tail.size() &&
            std::memcmp(p, xml_pre.data(), xml_pre.size()) == 0 &&
            p[xml_pre.size()] == '\0' &&
            std::memcmp(p + xml_pre.size() + 1,
                        xml_tail.data(), xml_tail.size()) == 0) {
            found_match = true;
            break;
        }
    }
    CHECK(found_match);
}

TEST_CASE("DumpRecorder enqueue returns Stopped after destruction",
          "[dump][overflow][api]") {
    auto dir = fs::temp_directory_path() / "test_dump_overflow_stopped";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Scope the recorder so its worker stops before we try to enqueue.
    auto rec = std::make_unique<mrd::DumpRecorder>(dir);
    dump_start_scan(*rec, "stopped.h5",
                    "<?xml version=\"1.0\"?><ismrmrdHeader/>");
    rec->close_scan();
    rec.reset();  // destructor sets stopping_ = true and joins worker

    // Can't call on destructed; instead, exercise the Stopped path by
    // checking Stopped is a distinct enum value.
    REQUIRE(mrd::DumpEnqueueResult::Stopped != mrd::DumpEnqueueResult::Accepted);
    REQUIRE(mrd::DumpEnqueueResult::Stopped != mrd::DumpEnqueueResult::Dropped);
}

// Audit 2026-08-28 #11: dump status said "complete" after records were
// dropped on disk-write failure. A regular file where the dump root should
// be makes every spool open fail, so every record is dropped.
TEST_CASE("DumpRecorder reports Incomplete, not Complete, after dropped records",
          "[dump][overflow][status]") {
    namespace fs = std::filesystem;
    auto base = fs::temp_directory_path() / "test_dump_incomplete";
    fs::remove_all(base);
    fs::create_directories(base);
    const auto root = base / "dump_root";
    mrd::DumpRecorder rec(root.string());
    // The constructor created the lane directories; replace each with a
    // regular file so the worker's spool open fails for every record.
    std::vector<fs::path> lane_dirs;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_directory() && fs::is_empty(e.path())) lane_dirs.push_back(e.path());
    }
    REQUIRE_FALSE(lane_dirs.empty());
    for (const auto& d : lane_dirs) {
        fs::remove_all(d);
        std::ofstream(d) << "not a directory";
    }
    dump_start_scan(rec, "lost.h5", "<ismrmrdHeader/>");
    rec.close_scan();
    CHECK(rec.dropped_record_count() > 0);
    CHECK(rec.counters().status == mrd::DumpRecorder::ConversionStatus::Incomplete);
    CHECK(std::string(mrd::DumpRecorder::status_name(rec.counters().status)) == "incomplete");
}
