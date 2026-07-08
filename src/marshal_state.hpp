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
#include <map>
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

    // --latest-dir: alternate root for the transient latest-snapshot
    // artifacts (latest_image.h5 / latest_error.png). Empty = keep them
    // under dump_dir (historical layout). Point at a tmpfs to take the
    // per-volume snapshot I/O off the archive disk.
    std::filesystem::path latest_dir;

    // --recon-url (empty = no reconstruction target)
    std::string recon_url;

    // --http and --ws-port
    std::string http_host{"0.0.0.0"};
    uint16_t http_port{8080};
    uint16_t ws_port{0}; // 0 = disabled

    // Max request body (128 MiB default)
    std::size_t max_body_bytes{128ULL * 1024ULL * 1024ULL};

    // --recon-close-timeout-ms: how long to wait, after the scanner's
    // CLOSE has been forwarded to recon, for recon to flush its tail
    // images and send its own CLOSE back. Sized for slow recons
    // (e.g. GRAPPA on a remote VM); on expiry the marshal emits its
    // own CLOSE to the scanner so the scanner never hangs.
    uint32_t recon_close_timeout_ms{30000};

    // Server uptime
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};

    // ------ Per-scan state (reset by POST /close) ------

    // True after POST /header, cleared by POST /close
    std::atomic<bool> header_received{false};
    // True after POST /config, cleared by POST /close
    std::atomic<bool> config_received{false};

    // Stored XML header and config name for current scan
    std::mutex scan_mtx;
    // Bumped (under scan_mtx) each time a new scan's METADATA_XML arrives.
    // Lets a finalizer that ran after an abnormal scanner EOF detect that a
    // NEW scan has since taken ownership of the per-scan state and skip its
    // (now stale) flush/mark — see mrd_tcp_listener.hpp done-path and the
    // *_at_epoch helpers in live_image_store.hpp.
    std::atomic<uint64_t> scan_epoch{0};
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

    // Latest slice-translation command (±1) from WebGL client; cached for
    // MRI-side reads. Empty until the first POST. GET returns the cache
    // without clearing (repeated reads return the same value).
    std::mutex slice_translation_mtx;
    std::string latest_slice_translation_json;

    // Latest absolute slice prescription (position + orientation) from the
    // UI; cached like the ±1 nudge. See POST /write/slice_target.
    std::mutex slice_target_mtx;
    std::string latest_slice_target_json;

    // Latest relative slice move (translation + rotation deltas) from the
    // UI. See POST /write/slice_delta.
    std::mutex slice_delta_mtx;
    std::string latest_slice_delta_json;

    // Slice geometry observed in image headers (position + orientation per
    // slice index), updated on every IMAGE from either lane, cleared at each
    // scan start (METADATA_XML). Embedded in the slice-translation TEXT
    // command pushed to the scanner and served at GET /read/slice_geometry.
    struct SliceGeometry {
        uint16_t slice{0};
        float position[3]{};
        float read_dir[3]{};
        float phase_dir[3]{};
        float slice_dir[3]{};
        std::string ts;   // ISO8601 of the last update
    };
    std::mutex slice_geom_mtx;
    std::map<uint16_t, SliceGeometry> slice_geom;
    int latest_slice{-1};   // slice index of the most recent image, -1 = none

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

    // MRD TCP return hook (set by MrdTcpListener, pushes recon messages to
    // scanner). Returns true if the frame was written to a connected scanner
    // socket; false when no scanner is connected or the send failed.
    std::function<bool(uint16_t, const void*, size_t)> mrd_push_message =
        [](uint16_t, const void*, size_t) { return false; };

    // Status hooks for GET /status (set by MrdTcpListener / main; the defaults
    // report "not connected" when the component is absent).
    std::function<bool()> mrd_scanner_connected = [] { return false; };
    std::function<bool()> recon_connected = [] { return false; };

    // Epoch ms of the most recent latest_image.h5 publish (0 = none yet).
    // Written on LatestImageWriter completion; read by GET /status.
    std::atomic<int64_t> last_publish_ms{0};

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
