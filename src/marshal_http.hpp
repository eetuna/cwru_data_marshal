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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <ismrmrd/ismrmrd.h>

#include "marshal_state.hpp"
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
// handle_recon_image: archive recon image + write standalone + push to scanner
// Called from ReconForwarder on_image callback (MRD TCP path).
// ---------------------------------------------------------------------------
inline void handle_recon_image(MarshalState& state, const void* data, size_t size)
{
    if (size < mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t)) return;

    const auto* ihdr = static_cast<const ISMRMRD::ImageHeader*>(data);
    const char* after_hdr = static_cast<const char*>(data) + mrd::IMAGE_HEADER_BYTES;
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, after_hdr, sizeof(uint64_t));
    const char* attr_str = after_hdr + sizeof(uint64_t);
    const char* pixel_data = attr_str + attr_len;
    size_t pixel_bytes = size - mrd::IMAGE_HEADER_BYTES - sizeof(uint64_t) - attr_len;

    // Archive to recon HDF5 sink
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        if (!state.recon_sink && state.header_received.load()) {
            auto recon_path = mrd::recon_dir(state.dump_dir) / mrd::scan_filename();
            state.recon_sink = std::make_unique<mrd::MrdSink>(recon_path);
            if (!state.current_xml_header.empty())
                state.recon_sink->set_header(state.current_xml_header);
        }
        if (state.recon_sink) {
            std::string varname = "image_" + std::to_string(ihdr->image_series_index);
            state.recon_sink->append_image(varname, *ihdr,
                                           attr_str, static_cast<size_t>(attr_len),
                                           pixel_data, pixel_bytes);
        }
    }

    // Multi-slice buffering for viz standalone file.
    // Collect all spatial slices for one volume, then write atomically.
    // File format: uint16_t nz + nz × (wire-format image: 198B hdr + 8B attr_len + attr + pixels)
    uint16_t nz = state.expected_slices;
    uint16_t slice_idx = ihdr->slice;

    if (nz <= 1) {
        // Single-slice or unknown: write immediately (no buffering)
        auto standalone = mrd::recon_dir(state.dump_dir) / "latest_image.bin";
        try {
            // Write: uint16_t nz=1, then the single image
            uint16_t one = 1;
            std::vector<uint8_t> buf(sizeof(uint16_t) + size);
            std::memcpy(buf.data(), &one, sizeof(uint16_t));
            std::memcpy(buf.data() + sizeof(uint16_t), data, size);
            mrd::write_standalone_file(standalone, buf.data(), buf.size());
            std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
            state.latest_image_path = standalone.string();
            state.latest_image_error = false;
            state.latest_image_count++;
        } catch (const std::exception& e) {
            LOG_WARN("Standalone write failed: " << e.what());
        }
    } else {
        // Multi-slice: buffer by slice index
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        state.slice_buffer[slice_idx].assign(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + size);

        if (static_cast<uint16_t>(state.slice_buffer.size()) >= nz) {
            // Once all slices have been seen at least once, write the latest
            // slice set after every incoming image. This keeps the file-based
            // viz path live at image rate while still providing all slices.
            // Compute total size: uint16_t nz + sum of all slice sizes
            size_t total = sizeof(uint16_t);
            for (auto& [idx, bytes] : state.slice_buffer)
                total += bytes.size();

            std::vector<uint8_t> buf(total);
            size_t off = 0;
            std::memcpy(buf.data() + off, &nz, sizeof(uint16_t));
            off += sizeof(uint16_t);
            // Write slices in order of slice index (map is sorted)
            for (auto& [idx, bytes] : state.slice_buffer) {
                std::memcpy(buf.data() + off, bytes.data(), bytes.size());
                off += bytes.size();
            }

            auto standalone = mrd::recon_dir(state.dump_dir) / "latest_image.bin";
            try {
                mrd::write_standalone_file(standalone, buf.data(), buf.size());
                std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
                state.latest_image_path = standalone.string();
                state.latest_image_error = false;
                state.latest_image_count++;
            } catch (const std::exception& e) {
                LOG_WARN("Standalone write failed: " << e.what());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// GET /image/latest
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_image_latest(const http::request<Body>& req, MarshalState& state)
{
    std::lock_guard<std::mutex> lk(state.latest_image_mtx);
    nlohmann::json j;
    j["path"] = state.latest_image_path;
    j["error"] = state.latest_image_error;
    j["slices"] = state.latest_image_count;
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
static nlohmann::json list_h5_files(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    nlohmann::json arr = nlohmann::json::array();
    if (!fs::exists(dir)) return arr;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".h5") {
            nlohmann::json item;
            item["path"] = entry.path().string();
            std::error_code ec;
            item["size"] = fs::file_size(entry.path(), ec);
            auto ftime = fs::last_write_time(entry.path(), ec);
            item["modified"] = std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch()).count();
            arr.push_back(item);
        }
    }
    return arr;
}

template <class Body>
static auto handle_get_dump_scanner(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_h5_files(mrd::scanner_dir(state.dump_dir)));
}

template <class Body>
static auto handle_get_dump_recon(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_h5_files(mrd::recon_dir(state.dump_dir)));
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

    // 404
    send(json_response(req, http::status::not_found,
                       {{"error", "not found"},
                        {"path", std::string(target)}}));
}
