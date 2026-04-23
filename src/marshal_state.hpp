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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/pose.hpp"
#include "dump_recorder.hpp"
#include "latest_image_writer.hpp"
#include "live_image_recorder.hpp"
#include "mrd_sink.hpp"

struct LiveImageLaneState {
    std::unique_ptr<mrd::LiveImageRecorder> recorder;

    void close() {
        if (recorder) {
            recorder->close_scan();
        }
    }
};

struct ReconLatestGroupState {
    bool active{false};
    bool published{false};
    uint16_t image_series_index{0};
    std::vector<std::vector<uint8_t>> images;
    std::vector<uint16_t> seen_slices;

    void reset() {
        active = false;
        published = false;
        image_series_index = 0;
        images.clear();
        seen_slices.clear();
    }
};

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
    uint16_t recon_expected_slices{0};
    bool scanner_lane_finalized{false};
    bool recon_lane_finalized{false};

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
    std::atomic<uint64_t> latest_image_generation{0};
    std::atomic<bool> recon_failure_reported{false};
    std::unique_ptr<mrd::LatestImageWriter> latest_writer;

    // Async live image-only history writers per provenance lane.
    LiveImageLaneState scanner_live;
    LiveImageLaneState recon_live;

    // Current logical recon result for /image/latest publication.
    ReconLatestGroupState recon_latest_group;

    // ------ Perf instrumentation (exposed via GET /debug/perf) ------
    // Bumped in append_live_image / append_live_waveform / publish_latest_snapshot.
    // Free-running totals since process start. Rates are computed by the
    // caller (or by subtracting two snapshots).
    std::atomic<uint64_t> perf_scanner_images_received{0};
    std::atomic<uint64_t> perf_recon_images_received{0};
    std::atomic<uint64_t> perf_scanner_waveforms_received{0};
    std::atomic<uint64_t> perf_publish_attempts_scanner{0};
    std::atomic<uint64_t> perf_publish_attempts_recon{0};

    // WS emit hook (set by WsServer on init, optional)
    std::function<void(const std::string&)> ws_emit = [](const std::string&) {};

    // MRD TCP return hook (set by MrdTcpListener, pushes recon messages to scanner)
    std::function<void(uint16_t, const void*, size_t)> mrd_push_message =
        [](uint16_t, const void*, size_t) {};

    // ------ Methods ------

    // Close both sinks and clear per-scan state. Ready for next POST /header.
    void close_scan() {
        std::lock_guard<std::mutex> lk(scan_mtx);
        if (dump_recorder) dump_recorder->close_scan();
        scanner_live.close();
        recon_live.close();
        current_xml_header.clear();
        current_config.clear();
        current_scan_filename.clear();
        recon_expected_slices = 0;
        scanner_lane_finalized = false;
        recon_lane_finalized = false;
        header_received.store(false);
        config_received.store(false);
        recon_failure_reported.store(false);
        recon_latest_group.reset();
    }
};
