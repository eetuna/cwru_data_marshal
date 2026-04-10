/*
 * File: src/marshal_http.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: HTTP routing — new API per v2 spec
 *
 * Scanner-facing: POST /header, /config, /frame, /close
 * Recon-facing: POST /image
 * Query: GET /image/latest, /transform, /pose, /health, /dump/scanner, /dump/recon
 * Control: PUT /transform, POST /pose
 *
 * No /v1/* routes. No X-MRD-* or X-Recon-* headers.
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_http"
#include "logging.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <sstream>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "marshal_state.hpp"
#include "mrd_io.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"
#include "mrd_type_detector.hpp"

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
// POST /header
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_header(const http::request<Body>& req, MarshalState& state)
{
    namespace fs = std::filesystem;

    const std::string xml(req.body());
    if (xml.empty())
        return json_response(req, http::status::bad_request, {{"error", "empty header body"}});

    std::lock_guard<std::mutex> lk(state.scan_mtx);

    // Close any open scan
    if (state.scanner_sink) {
        state.scanner_sink->close();
        state.scanner_sink.reset();
    }
    if (state.recon_sink) {
        state.recon_sink->close();
        state.recon_sink.reset();
    }

    // Open new scanner sink
    auto scanner_path = mrd::scanner_dir(state.dump_dir) / mrd::scan_filename();
    state.scanner_sink = std::make_unique<mrd::MrdSink>(scanner_path);
    try {
        state.scanner_sink->set_header(xml);
    } catch (const std::exception& e) {
        state.scanner_sink.reset();
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("writeHeader failed: ") + e.what()}});
    }

    state.current_xml_header = xml;
    state.current_config.clear();
    state.header_received.store(true);
    state.config_received.store(false);

    LOG_INFO("POST /header: new scan " << scanner_path.filename().string());

    // Forward to recon if configured
    // (forwarder is accessed from marshal_main, not here — it's triggered there)

    return json_response(req, http::status::ok, {{"status", "ok"}});
}

// ---------------------------------------------------------------------------
// POST /config
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_config(const http::request<Body>& req, MarshalState& state)
{
    if (!state.header_received.load())
        return json_response(req, http::status::conflict,
                             {{"error", "no /header received"}});

    std::string config(req.body());
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        state.current_config = config;
    }
    state.config_received.store(true);

    LOG_INFO("POST /config: " << config);
    return json_response(req, http::status::ok, {{"status", "ok"}});
}

// ---------------------------------------------------------------------------
// POST /frame — scanner-side, archive + forward
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_frame(const http::request<Body>& req, MarshalState& state)
{
    if (!state.header_received.load())
        return json_response(req, http::status::conflict,
                             {{"error", "no /header received"}});
    if (!state.config_received.load())
        return json_response(req, http::status::conflict,
                             {{"error", "no /config received (D8)"}});

    const auto& body = req.body();
    const void* data = body.data();
    size_t size = body.size();

    auto type = mrd::detect_mrd_type(data, size);

    std::lock_guard<std::mutex> lk(state.scan_mtx);
    if (!state.scanner_sink) {
        return json_response(req, http::status::internal_server_error,
                             {{"error", "scanner sink not open"}});
    }

    switch (type) {
    case mrd::MrdDataType::ACQUISITION: {
        // Deserialize via libismrmrd
        const auto* ahdr = static_cast<const ISMRMRD::AcquisitionHeader*>(data);
        ISMRMRD::Acquisition acq(ahdr->number_of_samples, ahdr->active_channels,
                                 ahdr->trajectory_dimensions);
        ISMRMRD::AcquisitionHeader hdr_copy;
        std::memcpy(&hdr_copy, data, mrd::ACQUISITION_HEADER_BYTES);
        acq.setHead(hdr_copy);
        // Copy trajectory
        size_t traj_bytes = static_cast<size_t>(ahdr->trajectory_dimensions)
                          * ahdr->number_of_samples * sizeof(float);
        if (traj_bytes > 0)
            std::memcpy(acq.getTrajPtr(),
                        static_cast<const char*>(data) + mrd::ACQUISITION_HEADER_BYTES,
                        traj_bytes);
        // Copy samples
        size_t sample_bytes = static_cast<size_t>(ahdr->number_of_samples)
                            * ahdr->active_channels * sizeof(complex_float_t);
        std::memcpy(acq.getDataPtr(),
                    static_cast<const char*>(data) + mrd::ACQUISITION_HEADER_BYTES + traj_bytes,
                    sample_bytes);
        state.scanner_sink->append_acquisition(acq);
        break;
    }
    case mrd::MrdDataType::IMAGE: {
        const auto* ihdr = static_cast<const ISMRMRD::ImageHeader*>(data);
        const char* after_hdr = static_cast<const char*>(data) + mrd::IMAGE_HEADER_BYTES;
        uint64_t attr_len = 0;
        std::memcpy(&attr_len, after_hdr, sizeof(uint64_t));
        const char* attr_str = after_hdr + sizeof(uint64_t);
        const char* pixel_data = attr_str + attr_len;
        size_t pixel_bytes = size - mrd::IMAGE_HEADER_BYTES - sizeof(uint64_t) - attr_len;

        std::string varname = "image_" + std::to_string(ihdr->image_series_index);
        state.scanner_sink->append_image(varname, *ihdr,
                                         attr_str, static_cast<size_t>(attr_len),
                                         pixel_data, pixel_bytes);

        // Update standalone file for live viz
        auto standalone = mrd::recon_dir(state.dump_dir) / "latest_image.bin";
        try {
            mrd::write_standalone_file(standalone, data, size);
            std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
            state.latest_image_path = standalone.string();
            state.latest_image_error = false;
        } catch (const std::exception& e) {
            LOG_WARN("Standalone write failed: " << e.what());
        }
        break;
    }
    case mrd::MrdDataType::WAVEFORM: {
        const auto* whdr = static_cast<const ISMRMRD::WaveformHeader*>(data);
        ISMRMRD::Waveform wf(whdr->number_of_samples, whdr->channels);
        std::memcpy(&wf.head, data, mrd::WAVEFORM_HEADER_BYTES);
        size_t wf_data_bytes = static_cast<size_t>(whdr->number_of_samples)
                             * whdr->channels * sizeof(uint32_t);
        std::memcpy(wf.data,
                    static_cast<const char*>(data) + mrd::WAVEFORM_HEADER_BYTES,
                    wf_data_bytes);
        state.scanner_sink->append_waveform(wf);
        break;
    }
    case mrd::MrdDataType::UNKNOWN:
        state.scanner_sink->append_unknown_bytes(data, size);
        LOG_WARN("UNKNOWN MRD type, " << size << " bytes archived");
        break;
    }

    // Forwarding to recon is handled by marshal_main (reads forwarder pointer)
    // Return 202 regardless
    return json_response(req, http::status::accepted, {{"status", "accepted"}});
}

// ---------------------------------------------------------------------------
// POST /close — end of scan
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_close(const http::request<Body>& req, MarshalState& state)
{
    state.close_scan();
    LOG_INFO("POST /close: scan finalized");
    return json_response(req, http::status::ok, {{"status", "ok"}});
}

// ---------------------------------------------------------------------------
// POST /image — recon-facing (archive to from_reconstruction/)
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_image(const http::request<Body>& req, MarshalState& state)
{
    const auto& body = req.body();
    if (body.size() < mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t))
        return json_response(req, http::status::bad_request,
                             {{"error", "body too small for image"}});

    std::lock_guard<std::mutex> lk(state.scan_mtx);

    // If scan already closed (no header), drop gracefully
    if (!state.header_received.load()) {
        LOG_WARN("POST /image after /close — dropping late image");
        return json_response(req, http::status::ok, {{"status", "ok_late"}});
    }

    // Open recon sink lazily
    if (!state.recon_sink) {
        auto recon_path = mrd::recon_dir(state.dump_dir) / mrd::scan_filename();
        state.recon_sink = std::make_unique<mrd::MrdSink>(recon_path);
        if (!state.current_xml_header.empty())
            state.recon_sink->set_header(state.current_xml_header);
    }

    const void* data = body.data();
    size_t size = body.size();

    const auto* ihdr = static_cast<const ISMRMRD::ImageHeader*>(data);
    const char* after_hdr = static_cast<const char*>(data) + mrd::IMAGE_HEADER_BYTES;
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, after_hdr, sizeof(uint64_t));
    const char* attr_str = after_hdr + sizeof(uint64_t);
    const char* pixel_data = attr_str + attr_len;
    size_t pixel_bytes = size - mrd::IMAGE_HEADER_BYTES - sizeof(uint64_t) - attr_len;

    std::string varname = "image_" + std::to_string(ihdr->image_series_index);
    state.recon_sink->append_image(varname, *ihdr,
                                   attr_str, static_cast<size_t>(attr_len),
                                   pixel_data, pixel_bytes);

    // Write standalone file for live viz
    auto standalone = mrd::recon_dir(state.dump_dir) / "latest_image.bin";
    try {
        mrd::write_standalone_file(standalone, data, size);
        std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
        state.latest_image_path = standalone.string();
        state.latest_image_error = false;
    } catch (const std::exception& e) {
        LOG_WARN("Standalone write failed: " << e.what());
    }

    return json_response(req, http::status::ok, {{"status", "ok"}});
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

    // Scanner-facing
    if (method == http::verb::post && target == "/header") {
        send(handle_post_header(req, state));
        return;
    }
    if (method == http::verb::post && target == "/config") {
        send(handle_post_config(req, state));
        return;
    }
    if (method == http::verb::post && target == "/frame") {
        send(handle_post_frame(req, state));
        return;
    }
    if (method == http::verb::post && target == "/close") {
        send(handle_post_close(req, state));
        return;
    }

    // Recon-facing
    if (method == http::verb::post && target == "/image") {
        send(handle_post_image(req, state));
        return;
    }

    // Query
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
