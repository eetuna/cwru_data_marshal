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
#include <optional>
#include <string>
#include <vector>

#include "common/pose.hpp"
#include "dump_recorder.hpp"
#include "latest_image_writer.hpp"
#include "mrd_sink.hpp"
#include "slice_math.hpp"

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

    // Archive of everything received from the scanner and from recon
    // (dump/from_scanner, dump/from_reconstruction). Always on.
    std::unique_ptr<mrd::DumpRecorder> dump_recorder;

    // The viewer's copy of the current scan's images (GET /image/latest).
    std::unique_ptr<mrd::LatestSnapshot> snapshot;
    // --dump: archive only, viewer off (no images go to the snapshot).
    bool dump_mode{false};

    // What the marshal is doing, in words, for /status and the viewer:
    // "waiting for scan", "scanning", "waiting for recon to finish",
    // "scan finished", "recon failed". `status_detail` says why on failure.
    std::mutex status_mtx;
    std::string status{"waiting for scan"};
    std::string status_detail;
    void set_status(std::string s, std::string detail = {}) {
        std::lock_guard<std::mutex> lk(status_mtx);
        status = std::move(s);
        status_detail = std::move(detail);
    }
    std::pair<std::string, std::string> get_status() {
        std::lock_guard<std::mutex> lk(status_mtx);
        return {status, status_detail};
    }

    // Build identity (MARSHAL_VERSION, baked in at image build time).
    std::string version{"unknown"};
    // Startup check of the recon address: "reachable" / "unreachable: ...".
    std::string recon_check{"not checked"};

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
    // scan start (METADATA_XML). Served at GET /read/slice_geometry (display /
    // the UI's "Send Absolute Position"). Slice commands do NOT start from
    // it — they accumulate from zero like slice_control (see slice_state).
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

    // ------ Slice agent (scanner-side `slice_agent --listen`) ------
    //
    // The marshal behaves like Andrew's slice_control tool: six absolute
    // numbers (tx ty tz mm, rx ry rz deg) starting at zero; UI moves add to
    // them (slice_math::apply_step) and the totals are sent as 56-byte
    // SliceCommand packets (slice_agent_client.hpp). Cleared at scan start
    // so a new prescription never inherits the previous scan's slice.
    struct SliceAgentSettings {
        bool enabled{false};
    };
    SliceAgentSettings slice_agent_cfg;

    // slice_state_mtx serializes "accumulate + post" across HTTP threads so
    // the agent receives commands in the order the state was updated. Only
    // the non-blocking post happens under it; the wait for delivery is done
    // after it is released.
    std::mutex slice_state_mtx;
    std::optional<slice_math::SliceState> slice_state;   // nullopt = nothing sent this scan
    std::string slice_state_ts;                          // ISO8601 of the last command
    uint64_t slice_state_count{0};                       // commands this scan

    // Hooks set by main when the agent client is configured. Defaults:
    // disabled / not connected / nothing sent.
    std::function<uint64_t(const slice_math::WireCommand&)> slice_agent_post =
        [](const slice_math::WireCommand&) { return uint64_t{0}; };   // 0 = not queued
    std::function<bool(uint64_t)> slice_agent_wait = [](uint64_t) { return false; };
    std::function<bool()> slice_agent_connected = [] { return false; };

    // Scan start / scan close: the six numbers go back to zero and the agent
    // is told so (identity — what it publishes on connect anyway), so the
    // previous scan's slice never survives into the next prescription. The
    // post is non-blocking; safe on the scanner reader thread.
    void reset_slice_state() {
        std::lock_guard<std::mutex> lk(slice_state_mtx);
        const bool had_state = slice_state.has_value();
        slice_state.reset();
        slice_state_ts.clear();
        slice_state_count = 0;
        if (had_state && slice_agent_cfg.enabled)
            slice_agent_post(slice_math::command_from_state(slice_math::SliceState{}));
    }

    // Pose cache
    PoseStore poses;

    // ------ Perf instrumentation (exposed via GET /debug/perf) ------
    // Free-running totals since process start.
    std::atomic<uint64_t> perf_scanner_images_received{0};
    std::atomic<uint64_t> perf_recon_images_received{0};
    std::atomic<uint64_t> perf_scanner_waveforms_received{0};

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

    // ------ Methods ------

    // End of a scan: finish the archive and clear per-scan state.
    void close_scan() {
        std::lock_guard<std::mutex> lk(scan_mtx);
        if (dump_recorder) dump_recorder->close_scan();
        current_xml_header.clear();
        current_config.clear();
        current_scan_filename.clear();
        header_received.store(false);
        config_received.store(false);
        reset_slice_state();
    }
};
