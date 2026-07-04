/*
 * File: src/marshal_http.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: HTTP routing — query/control endpoints only
 *
 * Scanner data arrives via MRD TCP (mrd_tcp_listener.hpp), NOT HTTP.
 * Recon images arrive via MRD TCP (recon_forwarder.hpp reader thread), NOT HTTP.
 *
 * HTTP endpoints (for non-scanner clients: viz, pose, tracker):
 *   GET  /image/latest, /transform, /pose, /health, /dump/scanner, /dump/recon
 *   PUT  /transform
 *   POST /pose
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_http"
#include "logging.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "marshal_state.hpp"
#include "live_image_store.hpp"
#include "mrd_io.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <class Body>
static auto json_response(const http::request<Body>& req,
                          http::status status,
                          const nlohmann::json& j)
{
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::content_type, "application/json");
    res.keep_alive(req.keep_alive());
    res.body() = j.dump();
    res.prepare_payload();
    return res;
}

template <class Body>
static auto text_response(const http::request<Body>& req,
                          http::status status,
                          const std::string& text)
{
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::content_type, "text/plain");
    res.keep_alive(req.keep_alive());
    res.body() = text;
    res.prepare_payload();
    return res;
}

// ---------------------------------------------------------------------------
// handle_recon_image: archive recon image + update latest-file side channel.
// Called from ReconForwarder callback (MRD TCP path). Scanner return is handled
// by the callback in marshal_main.cpp and must not wait on latest-file HDF5 I/O.
// ---------------------------------------------------------------------------
inline void handle_recon_image(MarshalState& state, const void* data, size_t size)
{
    // Dump mode is exclusive of the live snapshot/history pipeline. Gate
    // purely on the mode flag so a missing dump_recorder does not silently
    // fall back to writing live output.
    if (state.dump_enabled) {
        if (state.dump_recorder) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            state.dump_recorder->append_recon_image(
                std::vector<uint8_t>(bytes, bytes + size));
        }
        return;
    }

    mrd::append_live_image(state, mrd::LiveLane::Recon,
                           static_cast<const uint8_t*>(data), size);
}

inline void handle_recon_waveform(MarshalState& state, const void* data, size_t size)
{
    if (size < mrd::WAVEFORM_HEADER_BYTES) return;
    const auto* whdr = static_cast<const ISMRMRD::WaveformHeader*>(data);
    size_t data_bytes = size_t(whdr->number_of_samples) * whdr->channels * sizeof(uint32_t);
    if (size < mrd::WAVEFORM_HEADER_BYTES + data_bytes) return;
    if (state.dump_enabled) {
        if (state.dump_recorder) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            state.dump_recorder->append_recon_waveform(
                std::vector<uint8_t>(bytes, bytes + size));
        }
        return;
    }
    // Live mode: persist recon-side waveforms via the per-lane history
    // recorder so default-mode runs capture ECG too (not just images).
    mrd::append_live_waveform(state, mrd::LiveLane::Recon,
                              static_cast<const uint8_t*>(data), size);
}

// ---------------------------------------------------------------------------
// GET /image/latest
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_image_latest(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    if (state.dump_enabled) {
        return json_response(req, http::status::not_found,
                             {{"error", "dump mode; no live snapshot"}});
    }
    std::lock_guard<std::mutex> lk(state.latest_image_mtx);
    if (state.latest_image_path.empty()) {
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        return res;
    }
    nlohmann::json j;
    j["path"] = state.latest_image_path;
    j["error"] = state.latest_image_error;
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// GET /transform — returns and zeros
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_transform(const http::request<Body>& req, MarshalState& state)
{
    std::lock_guard<std::mutex> lk(state.transform_mtx);
    nlohmann::json j;
    j["through_plane_mm"] = state.transform.through_plane_mm;
    j["readout_mm"] = state.transform.readout_mm;
    j["phase_mm"] = state.transform.phase_mm;
    j["rotation_rad"] = state.transform.rotation_rad;
    // Atomically zero after returning
    state.transform.clear();
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// PUT /transform
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_put_transform(const http::request<Body>& req, MarshalState& state)
{
    try {
        auto j = nlohmann::json::parse(req.body());
        std::lock_guard<std::mutex> lk(state.transform_mtx);
        state.transform.through_plane_mm = j.value("through_plane_mm", 0.0);
        state.transform.readout_mm = j.value("readout_mm", 0.0);
        state.transform.phase_mm = j.value("phase_mm", 0.0);
        state.transform.rotation_rad = j.value("rotation_rad", 0.0);
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }
    return json_response(req, http::status::ok, {{"status", "ok"}});
}

// ---------------------------------------------------------------------------
// POST /write/file_slice_translation and GET /read/file_slice_translation
//
// Command channel: the WebGL client nudges the MRI imaging slice by ±1.
// Body (JSON): { "client_id": "...", "sent_at": 123, "values": [ ±1 ] }
// The latest command is cached in memory; GET returns it without clearing.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_slice_translation(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }

    if (!body.contains("values") || !body["values"].is_array() || body["values"].size() != 1) {
        return json_response(req, http::status::bad_request,
                             {{"error", "missing or invalid values"}, {"required", "values[1]"}});
    }
    const auto& direction_value = body["values"][0];
    if (!direction_value.is_number()) {
        return json_response(req, http::status::bad_request,
                             {{"error", "values[0] must be numeric"}});
    }
    const double direction = direction_value.get<double>();
    if (direction != 1.0 && direction != -1.0) {
        return json_response(req, http::status::bad_request,
                             {{"error", "values[0] must be +1 or -1"}, {"got", direction}});
    }

    body["ts"] = mrd::iso8601_now_ms();
    {
        std::lock_guard<std::mutex> lk(state.slice_translation_mtx);
        state.latest_slice_translation_json = body.dump();
    }
    return json_response(req, http::status::ok,
                         {{"file", "file_slice_translation"}, {"direction", direction}});
}

template <class Body>
static auto handle_get_slice_translation(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    std::string cached;
    {
        std::lock_guard<std::mutex> lk(state.slice_translation_mtx);
        cached = state.latest_slice_translation_json;
    }
    if (cached.empty()) {
        // 204 must be body-less; json_response would set a body and
        // prepare_payload() would throw.
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return res;
    }
    try {
        return json_response(req, http::status::ok, nlohmann::json::parse(cached));
    } catch (...) {
        return json_response(req, http::status::internal_server_error,
                             {{"error", "failed to parse slice translation cache"}});
    }
}

// ---------------------------------------------------------------------------
// POST /pose and GET /pose
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_pose(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    try {
        auto j = nlohmann::json::parse(req.body());
        Pose pose;
        pose.t = std::chrono::system_clock::now();
        if (j.contains("position")) {
            auto& pos = j["position"];
            pose.p[0] = pos[0].template get<double>();
            pose.p[1] = pos[1].template get<double>();
            pose.p[2] = pos[2].template get<double>();
        }
        if (j.contains("orientation")) {
            auto& ori = j["orientation"];
            for (size_t i = 0; i < 9 && i < ori.size(); ++i)
                pose.R[i] = ori[i].template get<double>();
        }
        state.poses.set(pose);
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }
    return json_response(req, http::status::ok, {{"status", "ok"}});
}

template <class Body>
static auto handle_get_pose(const http::request<Body>& req, MarshalState& state)
{
    auto p = state.poses.get();
    return json_response(req, http::status::ok, pose_to_json(p));
}

// ---------------------------------------------------------------------------
// GET /dump/scanner and GET /dump/recon
// ---------------------------------------------------------------------------
static nlohmann::json list_dump_files(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    nlohmann::json arr = nlohmann::json::array();
    if (!fs::exists(dir)) return arr;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".h5" && entry.path().filename().string().rfind("scan_", 0) == 0) {
            nlohmann::json item;
            item["path"] = entry.path().string();
            std::error_code ec;
            item["size"] = fs::file_size(entry.path(), ec);
            auto ftime = fs::last_write_time(entry.path(), ec);
            if (!ec) {
                auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                item["modified"] = std::chrono::duration_cast<std::chrono::seconds>(
                    sys_time.time_since_epoch()).count();
            }
            arr.push_back(item);
        }
    }
    return arr;
}

template <class Body>
static auto handle_get_dump_scanner(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_dump_files(mrd::dump_scanner_dir(state.dump_dir)));
}

template <class Body>
static auto handle_get_dump_recon(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_dump_files(mrd::dump_recon_dir(state.dump_dir)));
}

// ---------------------------------------------------------------------------
// GET /health
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_health(const http::request<Body>& req, MarshalState& state)
{
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - state.start).count();
    return json_response(req, http::status::ok,
                         {{"status", "ok"}, {"uptime_s", uptime}});
}

// ---------------------------------------------------------------------------
// GET /status — one-glance operational summary for experiment operators:
// mode, scanner/recon connectivity, current scan, last-publish age, disk.
// Read-only; aggregates state that already exists in-process.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_status(const http::request<Body>& req, MarshalState& state)
{
    nlohmann::json j;
    j["mode"] = state.dump_enabled ? "dump" : "live";
    auto now = std::chrono::steady_clock::now();
    j["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(now - state.start).count();

    j["scanner_connected"] = state.mrd_scanner_connected();

    j["recon"]["configured"] = !state.recon_url.empty();
    j["recon"]["target"] = state.recon_url;
    j["recon"]["connected"] = state.recon_connected();

    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        j["scan"]["active"] = state.header_received.load();
        j["scan"]["file"] = state.current_scan_filename;
    }
    j["images"]["from_scanner_total"] = state.perf_scanner_images_received.load();
    j["images"]["from_recon_total"] = state.perf_recon_images_received.load();

    const int64_t lp = state.last_publish_ms.load();
    if (lp > 0) {
        j["last_image_age_s"] =
            static_cast<double>(static_cast<int64_t>(mrd::now_ms_epoch()) - lp) / 1000.0;
    } else {
        j["last_image_age_s"] = nullptr;
    }

    std::error_code ec;
    const auto sp = std::filesystem::space(state.dump_dir, ec);
    if (!ec) {
        j["disk_free_gb"] =
            std::round(static_cast<double>(sp.available) / 1e8) / 10.0;
    }

    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// GET /debug/sinks — per-pipeline message counters for retention testing.
// Returns sink-level acq/img/wf counters (only those incremented on
// successful HDF5 writes) plus dump drop totals. Use this — not viz_client
// FPS — to measure dump or live-history retention.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_debug_sinks(const http::request<Body>& req, MarshalState& state)
{
    nlohmann::json j;
    j["mode"] = state.dump_enabled ? "dump" : "live";

    if (state.dump_enabled && state.dump_recorder) {
        auto c = state.dump_recorder->counters();
        auto lane_json = [](const mrd::DumpRecorder::LaneSnapshot& l) {
            nlohmann::json o;
            o["spool_records"] = l.spool_records;
            o["spool_bytes"]   = l.spool_bytes;
            o["spool_open"]    = l.spool_open;
            o["write_error"]   = l.write_error;
            o["converted_acq"] = l.converted_acq;
            o["converted_img"] = l.converted_img;
            o["converted_wf"]  = l.converted_wf;
            o["conversion_ok"] = l.conversion_ok;
            return o;
        };
        j["dump"]["from_scanner"]        = lane_json(c.scanner);
        j["dump"]["from_reconstruction"] = lane_json(c.recon);
        j["dump"]["dropped_records"] = c.dropped_records;
        j["dump"]["dropped_bytes"]   = c.dropped_bytes;
        j["dump"]["had_overflow"]    = c.had_overflow;
        const char* status = "idle";
        switch (c.status) {
            case mrd::DumpRecorder::ConversionStatus::Idle:       status = "idle"; break;
            case mrd::DumpRecorder::ConversionStatus::Spooling:   status = "spooling"; break;
            case mrd::DumpRecorder::ConversionStatus::Converting: status = "converting"; break;
            case mrd::DumpRecorder::ConversionStatus::Complete:   status = "complete"; break;
            case mrd::DumpRecorder::ConversionStatus::Failed:     status = "failed"; break;
        }
        j["dump"]["conversion_status"] = status;
    }
    if (!state.dump_enabled) {
        auto live_lane_json = [](const mrd::LiveImageRecorder::CounterSnapshot& c,
                                 uint64_t dropped) {
            nlohmann::json o;
            o["acq"] = c.acq;
            o["img"] = c.img;
            o["wf"]  = c.wf;
            o["open"] = c.sink_open;
            // Post-fix: dropped should stay 0 under the lossless
            // contract. Retained for visibility.
            o["dropped"] = dropped;
            // Live queue depth and high-watermark surface: new with
            // the lossless-live change (no more drop-oldest). If the
            // watermark fires, the HDF5 worker is behind; memory
            // grows until it catches up.
            o["queued_jobs"] = c.queued_jobs;
            o["high_watermark_hit"] = c.high_watermark_hit;
            return o;
        };
        if (state.scanner_live.recorder) {
            j["live"]["from_scanner"] = live_lane_json(
                state.scanner_live.recorder->counters(),
                state.scanner_live.recorder->dropped_count());
        }
        if (state.recon_live.recorder) {
            j["live"]["from_reconstruction"] = live_lane_json(
                state.recon_live.recorder->counters(),
                state.recon_live.recorder->dropped_count());
        }
    }
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// GET /debug/perf — FPS-regression instrumentation. Free-running totals
// since process start. Deltas between two polls give rates.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_debug_perf(const http::request<Body>& req, MarshalState& state)
{
    nlohmann::json j;

    auto now = std::chrono::steady_clock::now();
    j["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(now - state.start).count();

    j["recv"]["scanner_images"]    = state.perf_scanner_images_received.load();
    j["recv"]["recon_images"]      = state.perf_recon_images_received.load();
    j["recv"]["scanner_waveforms"] = state.perf_scanner_waveforms_received.load();
    j["publish_attempts"]["scanner"] = state.perf_publish_attempts_scanner.load();
    j["publish_attempts"]["recon"]   = state.perf_publish_attempts_recon.load();

    if (state.latest_writer) {
        auto p = state.latest_writer->perf();
        j["latest_writer"]["enqueued"]          = p.enqueued;
        j["latest_writer"]["coalesced"]         = p.coalesced;
        j["latest_writer"]["dropped_oldest"]    = p.dropped_oldest;
        j["latest_writer"]["completed"]         = p.completed;
        j["latest_writer"]["failed"]            = p.failed;
        j["latest_writer"]["max_queue_depth"]   = p.max_queue_depth;
        j["latest_writer"]["last_write_us"]     = p.last_write_us;
        j["latest_writer"]["max_write_us"]      = p.max_write_us;
        j["latest_writer"]["last_drain_lag_us"] = p.last_drain_lag_us;
        j["latest_writer"]["max_drain_lag_us"]  = p.max_drain_lag_us;
    }

    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// Main dispatcher
// ---------------------------------------------------------------------------
template <class Body, class Send>
void handle_http_request(http::request<Body>&& req, MarshalState& state, Send&& send)
{
    auto target = req.target();
    auto method = req.method();

    // Body size check
    if (req.body().size() > state.max_body_bytes) {
        send(json_response(req, http::status::payload_too_large,
                           {{"error", "body too large"}}));
        return;
    }

    // Query/control endpoints (scanner data arrives via MRD TCP, not HTTP)
    if (method == http::verb::get && target == "/image/latest") {
        send(handle_get_image_latest(req, state));
        return;
    }
    if (method == http::verb::get && target == "/transform") {
        send(handle_get_transform(req, state));
        return;
    }
    if (method == http::verb::put && target == "/transform") {
        send(handle_put_transform(req, state));
        return;
    }
    if (method == http::verb::post &&
        (target == "/write/file_slice_translation" || target == "/write/file_slice_translation.json")) {
        send(handle_post_slice_translation(req, state));
        return;
    }
    if (method == http::verb::get &&
        (target == "/read/file_slice_translation" || target == "/read/file_slice_translation.json")) {
        send(handle_get_slice_translation(req, state));
        return;
    }
    if (method == http::verb::post && target == "/pose") {
        send(handle_post_pose(req, state));
        return;
    }
    if (method == http::verb::get && target == "/pose") {
        send(handle_get_pose(req, state));
        return;
    }
    if (method == http::verb::get && target == "/dump/scanner") {
        send(handle_get_dump_scanner(req, state));
        return;
    }
    if (method == http::verb::get && target == "/dump/recon") {
        send(handle_get_dump_recon(req, state));
        return;
    }
    if (method == http::verb::get && target == "/health") {
        send(handle_get_health(req, state));
        return;
    }
    if (method == http::verb::get && target == "/status") {
        send(handle_get_status(req, state));
        return;
    }
    if (method == http::verb::get && target == "/debug/sinks") {
        send(handle_get_debug_sinks(req, state));
        return;
    }
    if (method == http::verb::get && target == "/debug/perf") {
        send(handle_get_debug_perf(req, state));
        return;
    }

    // 404
    send(json_response(req, http::status::not_found,
                       {{"error", "not found"},
                        {"path", std::string(target)}}));
}
