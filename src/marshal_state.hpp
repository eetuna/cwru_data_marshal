/*
 * File: src/marshal_state.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Global mutable state shared across HTTP handlers
 *
 * Dump recorder, transform delta, pose cache, stored XML + config per scan,
 * optional recon forwarding.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/pose.hpp"
#include "dump_recorder.hpp"
#include "latest_image_writer.hpp"

struct SliceTransform {
    double through_plane_mm{0};
    double readout_mm{0};
    double phase_mm{0};
    double rotation_rad{0};

    bool is_zero() const {
        return through_plane_mm == 0 && readout_mm == 0
            && phase_mm == 0 && rotation_rad == 0;
    }

    void clear() {
        through_plane_mm = 0;
        readout_mm = 0;
        phase_mm = 0;
        rotation_rad = 0;
    }
};

struct MarshalState {
    // --dump-dir root
    std::filesystem::path dump_dir{"./data"};
    bool dump_enabled{false};

    // --recon-url (empty = no reconstruction target)
    std::string recon_url;

    // --http and --ws-port
    std::string http_host{"0.0.0.0"};
    uint16_t http_port{8080};
    uint16_t ws_port{0}; // 0 = disabled

    // Max request body (128 MiB default)
    std::size_t max_body_bytes{128ULL * 1024ULL * 1024ULL};

    // Server uptime
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};

    // ------ Per-scan state (reset by POST /close) ------

    // True after POST /header, cleared by POST /close
    std::atomic<bool> header_received{false};
    // True after POST /config, cleared by POST /close
    std::atomic<bool> config_received{false};

    // Stored XML header and config name for current scan
    std::mutex scan_mtx;
    std::string current_xml_header;
    std::string current_config;
    std::string current_scan_filename;

    // Async H5 dump recorder, present only when --dump is enabled.
    std::unique_ptr<mrd::DumpRecorder> dump_recorder;

    // ------ Persistent state (survives across scans) ------

    // Slice transform delta (GET zeros after returning)
    std::mutex transform_mtx;
    SliceTransform transform;

    // Pose cache
    PoseStore poses;

    // Latest image path for GET /image/latest
    std::mutex latest_image_mtx;
    std::string latest_image_path;
    bool latest_image_error{false};
    uint32_t latest_image_count{0};
    // Highest image_series_index seen this scan. Viz opens image_<this> directly.
    uint32_t latest_series_index{0};
    std::atomic<uint64_t> latest_image_generation{0};
    std::atomic<bool> recon_failure_reported{false};

    // Per-scan live appenders. One per side so a slow scanner-side write does
    // not block the recon side (and vice versa). Opened on scan start,
    // closed on scan close.
    std::unique_ptr<mrd::LatestImageWriter> live_scanner_writer;
    std::unique_ptr<mrd::LatestImageWriter> live_recon_writer;

    // WS emit hook (set by WsServer on init, optional)
    std::function<void(const std::string&)> ws_emit = [](const std::string&) {};

    // MRD TCP return hook (set by MrdTcpListener, pushes recon messages to scanner)
    std::function<void(uint16_t, const void*, size_t)> mrd_push_message =
        [](uint16_t, const void*, size_t) {};

    // ------ Methods ------

    // Close both sinks and clear per-scan state. Ready for next POST /header.
    //
    // Clears /image/latest-visible fields so a viz client that restarts (or
    // first connects) after a scan ends sees "nothing to show" instead of a
    // pointer into the just-closed scan file. Viz clients already running
    // during the CLOSE keep their in-memory frame — marshal just stops
    // advertising a pointer.
    void close_scan() {
        if (live_scanner_writer) live_scanner_writer->close_scan();
        if (live_recon_writer) live_recon_writer->close_scan();
        std::lock_guard<std::mutex> lk(scan_mtx);
        if (dump_recorder) dump_recorder->close_scan();
        current_xml_header.clear();
        current_config.clear();
        current_scan_filename.clear();
        header_received.store(false);
        config_received.store(false);
        recon_failure_reported.store(false);
        {
            std::lock_guard<std::mutex> img_lk(latest_image_mtx);
            latest_image_path.clear();
            latest_image_error = false;
            latest_image_count = 0;
            latest_series_index = 0;
        }
    }
};
