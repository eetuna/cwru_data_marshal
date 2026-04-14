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
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

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
// handle_recon_image: archive recon image + update latest-file side channel.
// Called from ReconForwarder callback (MRD TCP path). Scanner return is handled
// by the callback in marshal_main.cpp and must not wait on latest-file HDF5 I/O.
// ---------------------------------------------------------------------------
inline void handle_recon_image(MarshalState& state, const void* data, size_t size)
{
    if (size < mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t)) return;

    const auto* ihdr = static_cast<const ISMRMRD::ImageHeader*>(data);
    const char* after_hdr = static_cast<const char*>(data) + mrd::IMAGE_HEADER_BYTES;
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, after_hdr, sizeof(uint64_t));
    if (size < mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) + attr_len) return;

    if (state.dump_enabled && state.dump_recorder) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        state.dump_recorder->append_recon_image(std::vector<uint8_t>(bytes, bytes + size));
    }

    uint16_t nz = state.expected_slices;
    uint16_t slice_idx = ihdr->slice;
    std::string xml;
    std::vector<std::vector<uint8_t>> latest_images;

    if (nz <= 1) {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        xml = state.current_xml_header;
        latest_images.emplace_back(static_cast<const uint8_t*>(data),
                                   static_cast<const uint8_t*>(data) + size);
    } else {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        xml = state.current_xml_header;
        state.slice_buffer[slice_idx].assign(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + size);

        if (static_cast<uint16_t>(state.slice_buffer.size()) < nz)
            return;

        latest_images.reserve(state.slice_buffer.size());
        for (auto& [idx, bytes] : state.slice_buffer)
            latest_images.push_back(bytes);
    }

    auto dest = mrd::recon_dir(state.dump_dir) / "latest_image.h5";
    const auto generation = state.latest_image_generation.load();
    auto on_complete = [&state, generation](const std::filesystem::path& path) {
        if (state.latest_image_generation.load() != generation) return;
        std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
        state.latest_image_path = path.string();
        state.latest_image_error = false;
        state.latest_image_count++;
    };

    if (state.latest_writer) {
        state.latest_writer->enqueue(dest, std::move(xml), std::move(latest_images),
                                     std::move(on_complete));
        return;
    }

    try {
        mrd::write_latest_image_h5_file(dest, xml, latest_images);
        on_complete(dest);
    } catch (const std::exception& e) {
        LOG_WARN("Latest H5 write failed: " << e.what());
    }
}

inline void handle_recon_waveform(MarshalState& state, const void* data, size_t size)
{
    if (size < mrd::WAVEFORM_HEADER_BYTES) return;
    const auto* whdr = static_cast<const ISMRMRD::WaveformHeader*>(data);
    size_t data_bytes = size_t(whdr->number_of_samples) * whdr->channels * sizeof(uint32_t);
    if (size < mrd::WAVEFORM_HEADER_BYTES + data_bytes) return;
    if (state.dump_enabled && state.dump_recorder) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        state.dump_recorder->append_recon_waveform(std::vector<uint8_t>(bytes, bytes + size));
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
                         list_dump_files(mrd::scanner_dir(state.dump_dir)));
}

template <class Body>
static auto handle_get_dump_recon(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_dump_files(mrd::recon_dir(state.dump_dir)));
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
