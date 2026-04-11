/*
 * File: src/marshal_state.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Global mutable state shared across HTTP handlers
 *
 * Two sinks (scanner + recon), transform delta, pose cache,
 * stored XML + config per scan, optional recon forwarding.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/pose.hpp"
#include "mrd_sink.hpp"

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

    // --recon-url (empty = archival-only mode)
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

    // Scanner-side HDF5 sink (from_scanner/)
    std::unique_ptr<mrd::MrdSink> scanner_sink;

    // Recon-side HDF5 sink (from_reconstruction/), opened lazily on first POST /image
    std::unique_ptr<mrd::MrdSink> recon_sink;

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
    std::atomic<bool> recon_failure_reported{false};

    // Multi-slice buffering: collect all spatial slices before writing to file
    uint16_t expected_slices{0};  // From METADATA_XML <z> field; 0 = unknown
    std::map<uint16_t, std::vector<uint8_t>> slice_buffer;  // slice_idx → wire-format image bytes

    // WS emit hook (set by WsServer on init, optional)
    std::function<void(const std::string&)> ws_emit = [](const std::string&) {};

    // MRD TCP return hook (set by MrdTcpListener, pushes recon messages to scanner)
    std::function<void(uint16_t, const void*, size_t)> mrd_push_message =
        [](uint16_t, const void*, size_t) {};

    // ------ Methods ------

    // Close both sinks and clear per-scan state. Ready for next POST /header.
    void close_scan() {
        std::lock_guard<std::mutex> lk(scan_mtx);
        if (scanner_sink) { scanner_sink->close(); scanner_sink.reset(); }
        if (recon_sink)   { recon_sink->close();   recon_sink.reset(); }
        current_xml_header.clear();
        current_config.clear();
        header_received.store(false);
        config_received.store(false);
        recon_failure_reported.store(false);
        expected_slices = 0;
        slice_buffer.clear();
    }
};
