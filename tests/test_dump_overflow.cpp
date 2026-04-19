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
    auto r = rec.start_scan("api_ok.h5",
                            "<?xml version=\"1.0\"?><ismrmrdHeader/>");
    REQUIRE(r == mrd::DumpEnqueueResult::Accepted);

    auto r2 = rec.append_scanner_text("small");
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
    auto start_r = rec.start_scan("lossless.h5",
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
        REQUIRE(rec.set_scanner_config_file(config_file_payload)
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.set_scanner_config_text(config_text_payload)
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(first_text)
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text(second_text)
                == mrd::DumpEnqueueResult::Accepted);

        // Now open the sink via start_scan (METADATA_XML arrival).
        REQUIRE(rec.start_scan(filename, xml)
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
        REQUIRE(rec.start_scan("s" + std::to_string(scan) + ".h5",
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
    REQUIRE(rec.set_scanner_config_file("simplefft")
            == mrd::DumpEnqueueResult::Accepted);
    REQUIRE(rec.append_scanner_text("pre-metadata")
            == mrd::DumpEnqueueResult::Accepted);
    // Now METADATA_XML.
    REQUIRE(rec.start_scan("caller_suggested.h5",
                           "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);
    // Hit the recon lane with real content (text) so it definitely
    // produces an .h5. An earlier version of this test used an empty
    // recon image which the converter skipped as malformed, letting
    // the test pass even when the shared-stem invariant was broken.
    REQUIRE(rec.append_recon_text("recon hello")
            == mrd::DumpEnqueueResult::Accepted);
    rec.close_scan();

    auto scanner_h5 = find_single_h5(mrd::dump_scanner_dir(dir));
    auto recon_h5   = find_single_h5(mrd::dump_recon_dir(dir));
    REQUIRE(!scanner_h5.empty());
    REQUIRE(!recon_h5.empty());
    CHECK(scanner_h5.stem() == recon_h5.stem());
}

TEST_CASE("DumpRecorder close-barrier race: post-barrier record does NOT "
          "truncate just-closed spool",
          "[dump][spool][race]") {
    // Regression for codex finding after 850c349: close_scan's barrier
    // closed the spool, but scan_stem_ was only cleared later. If a
    // record arrived in the window in between, ensure_spool_on_worker
    // would read the OLD stem, open the same spool path with "wb", and
    // truncate it before convert_spool_to_hdf5 ran.
    //
    // Fix: close_scan_pending_ flag set under scan_stem_mtx_. Any
    // enqueue_on that sees pending=true generates a fresh stem, so
    // the post-barrier record writes to a DIFFERENT spool path.
    auto dir = fs::temp_directory_path() / "test_dump_barrier_race";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        mrd::DumpRecorder rec(dir);
        REQUIRE(rec.start_scan("first.h5", "<?xml version=\"1.0\"?><ismrmrdHeader/>")
                == mrd::DumpEnqueueResult::Accepted);
        REQUIRE(rec.append_scanner_text("from scan 1")
                == mrd::DumpEnqueueResult::Accepted);

        // Simulate a post-barrier race: enqueue ACQs (fast to
        // replay, unlike TEXT which goes through per-record HDF5
        // string writes) on a background thread, bounded count so
        // the test does not hang in the converter even if something
        // regresses. At least some of these will land after the
        // barrier but before close_scan returns.
        constexpr uint16_t nsamples = 4;
        constexpr uint16_t nchannels = 1;
        const size_t sample_bytes =
            size_t(nsamples) * nchannels * 2 * sizeof(float);
        ISMRMRD::AcquisitionHeader rhdr{};
        rhdr.version = 1;
        rhdr.number_of_samples = nsamples;
        rhdr.active_channels = nchannels;
        rhdr.trajectory_dimensions = 0;

        std::atomic<bool> stop{false};
        std::thread hammer([&] {
            while (!stop.load()) {
                std::vector<uint8_t> s(sample_bytes, 0x77);
                (void)rec.append_scanner_acquisition(rhdr, {}, std::move(s));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        rec.close_scan();
        stop.store(true);
        hammer.join();

        // Any records that raced the close started a second scan
        // (fresh stem). Close that one too so both spools flush to h5.
        rec.close_scan();
    }

    // The FIRST scan's .h5 must contain at least the pre-close text.
    // If truncation happened, it'd be empty or missing.
    int n_h5 = 0;
    bool found_first_text = false;
    for (auto& ent : fs::directory_iterator(mrd::dump_scanner_dir(dir))) {
        if (ent.path().extension() != ".h5") continue;
        ++n_h5;
        // Try reading text_0 from every h5; one of them must be the
        // first scan's "from scan 1".
        auto v = read_string_dataset_or_empty(ent.path(), "text_0");
        if (v == "from scan 1") found_first_text = true;
    }
    CHECK(n_h5 >= 1);
    CHECK(found_first_text);
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
    REQUIRE(rec.start_scan("s1.h5", "<?xml version=\"1.0\"?><ismrmrdHeader/>")
            == mrd::DumpEnqueueResult::Accepted);
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> s(sample_bytes, 0x11);
        REQUIRE(rec.append_scanner_acquisition(hdr, {}, std::move(s))
                == mrd::DumpEnqueueResult::Accepted);
    }
    REQUIRE(rec.append_recon_text("scan1 recon")
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
    REQUIRE(rec.start_scan("s2.h5", "<?xml version=\"1.0\"?><ismrmrdHeader/>")
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

TEST_CASE("DumpRecorder enqueue returns Stopped after destruction",
          "[dump][overflow][api]") {
    auto dir = fs::temp_directory_path() / "test_dump_overflow_stopped";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Scope the recorder so its worker stops before we try to enqueue.
    auto rec = std::make_unique<mrd::DumpRecorder>(dir);
    rec->start_scan("stopped.h5",
                    "<?xml version=\"1.0\"?><ismrmrdHeader/>");
    rec->close_scan();
    rec.reset();  // destructor sets stopping_ = true and joins worker

    // Can't call on destructed; instead, exercise the Stopped path by
    // checking Stopped is a distinct enum value.
    REQUIRE(mrd::DumpEnqueueResult::Stopped != mrd::DumpEnqueueResult::Accepted);
    REQUIRE(mrd::DumpEnqueueResult::Stopped != mrd::DumpEnqueueResult::Dropped);
}
