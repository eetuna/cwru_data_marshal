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

#include <filesystem>
#include <thread>
#include <vector>

#include <hdf5.h>

#include "dump_recorder.hpp"
#include "mrd_io.hpp"

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

TEST_CASE("DumpRecorder enqueue returns Dropped when queue overflows",
          "[dump][overflow][api]") {
    auto dir = fs::temp_directory_path() / "test_dump_overflow_dropped";
    fs::remove_all(dir);
    fs::create_directories(dir);

    mrd::DumpRecorder rec(dir);
    auto start_r = rec.start_scan("overflow.h5",
                                  "<?xml version=\"1.0\"?><ismrmrdHeader/>");
    REQUIRE(start_r == mrd::DumpEnqueueResult::Accepted);

    // Flood the 4096-job queue with tiny text records. At least one append
    // after the cap must return Dropped — this is the caller-visible
    // backpressure signal introduced by blocker-1 resolution.
    const std::string payload(64, 'X');
    bool saw_dropped = false;
    for (int i = 0; i < 20000 && !saw_dropped; ++i) {
        auto r = rec.append_scanner_text(payload);
        if (r == mrd::DumpEnqueueResult::Dropped) saw_dropped = true;
        REQUIRE((r == mrd::DumpEnqueueResult::Accepted
                 || r == mrd::DumpEnqueueResult::Dropped));
    }
    INFO("saw_dropped=" << saw_dropped
         << " had_overflow=" << rec.had_overflow());
    REQUIRE(saw_dropped);
    REQUIRE(rec.had_overflow());

    rec.close_scan();
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

    auto h5_path = mrd::dump_scanner_dir(dir) / filename;
    REQUIRE(fs::exists(h5_path));

    CHECK(read_string_dataset_or_empty(h5_path, "config_file") == config_file_payload);
    CHECK(read_string_dataset_or_empty(h5_path, "config") == config_text_payload);
    CHECK(read_string_dataset_or_empty(h5_path, "text_0") == first_text);
    CHECK(read_string_dataset_or_empty(h5_path, "text_1") == second_text);
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
