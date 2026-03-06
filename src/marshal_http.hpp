/*
 * File: src/marshal_http.hpp
 * Project: CWRU Data Marshal
 * Purpose: HTTP routing and handlers
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_http"
#include "logging.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>

#include <nlohmann/json.hpp>
#include "mrd_io.hpp"
#include "mrd_type_detector.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <iostream>
#include <vector>
#include <cctype>

#include "marshal_state.hpp"
#include "common/pose.hpp" // Pose, PoseStore, pose_to_json
#include "mrd_sink.hpp"
#include <ismrmrd/ismrmrd.h>

namespace http = boost::beast::http;
namespace fs = std::filesystem;

inline constexpr std::size_t kMaxHttpBodyBytes = 128ULL * 1024ULL * 1024ULL; // 128 MiB ceiling for inbound payloads

// Milliseconds-precision ISO8601 for all endpoints
inline std::string iso8601_now()
{
    return mrd::iso8601_now_ms();
}

inline bool read_file_all(const fs::path &p, std::string &out)
{
    std::ifstream f(p);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// crude parser for ?ts=…&limit=…&last=…
inline void parse_ts_limit(const std::string &target, std::string &ts, size_t &limit, size_t &last)
{
    ts.clear();
    limit = 0;
    last = 0;
    auto qpos = target.find('?');
    if (qpos == std::string::npos)
        return;
    auto qp = target.substr(qpos + 1);
    auto get = [&](const char *k)
    {
        std::string key = std::string(k) + "=";
        auto p = qp.find(key);
        if (p == std::string::npos)
            return std::string();
        auto v = qp.substr(p + key.size());
        auto a = v.find('&');
        return (a == std::string::npos) ? v : v.substr(0, a);
    };
    ts = get("ts");
    auto lim = get("limit");
    if (!lim.empty())
    {
        try
        {
            limit = static_cast<size_t>(std::stoull(lim));
        }
        catch (...)
        {
        }
    }
    auto lst = get("last");
    if (!lst.empty())
    {
        try
        {
            last = static_cast<size_t>(std::stoull(lst));
        }
        catch (...)
        {
        }
    }
}

template <class Body>
http::response<http::string_body> handle_http_request(const http::request<Body> &req, MarshalState &state, const std::string &req_id = "")
{
    using nlohmann::json;

    auto make_response = [&](http::status status, json body)
    {
        http::response<http::string_body> res{status, req.version()};
        res.set(http::field::content_type, "application/json");
        if (!req_id.empty())
            res.set("X-Request-Id", req_id);

        if (status == http::status::no_content) {
            res.body() = "";
            res.prepare_payload();
            return res;
        }

        json envelope;
        if (static_cast<int>(status) >= 200 && static_cast<int>(status) < 300) {
            envelope["status"] = "ok";
            envelope["data"] = body;
        } else {
            envelope["status"] = "error";
            // If body is already an error object, merge it, otherwise wrap it
            if (body.is_object() && body.contains("error")) {
                envelope["error"] = body["error"];
                if (body.contains("what")) envelope["what"] = body["what"];
            } else {
                envelope["error"] = body.dump();
            }
        }

        res.body() = envelope.dump();
        res.prepare_payload();
        return res;
    };

    // GET /health
    if (req.method() == http::verb::get && req.target() == "/health")
    {
        auto up = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
        bool sink_ok = (state.mrd_sink != nullptr);
        bool writer_ok = state.json_writer_running.load();
        size_t queue_depth = 0;
        {
            std::lock_guard<std::mutex> lock(state.json_queue_mutex);
            queue_depth = state.json_write_queue.size();
        }
        bool healthy = sink_ok && writer_ok;
        auto status = healthy ? http::status::ok : http::status::service_unavailable;
        return make_response(status, {
            {"status", healthy ? "healthy" : "degraded"},
            {"uptime_s", up},
            {"hdf5_sink", sink_ok},
            {"json_writer", writer_ok},
            {"json_queue_depth", queue_depth}
        });
    }

    // GET /v1/pose/current  (reads from in-memory cache)
    if (req.method() == http::verb::get && req.target() == "/v1/pose/current")
    {
        std::string cached_json;
        {
            std::lock_guard<std::mutex> lock(state.latest_pose_mutex);
            cached_json = state.latest_pose_json;
        }

        if (!cached_json.empty())
        {
            try {
                auto pose_data = json::parse(cached_json);
                return make_response(http::status::ok, {{"pose", pose_data}, {"source", pose_data.value("source", "unknown")}});
            } catch (...) {
                return make_response(http::status::internal_server_error,
                    {{"error", "failed to parse pose cache"}});
            }
        }
        return make_response(http::status::no_content, json::object());
    }

    // POST /v1/pose/update
    // Body (JSON): { "p":[x,y,z], "R":[9], "frame":"scanner"?, "source":"fk"? }
    if (req.method() == http::verb::post && req.target() == "/v1/pose/update")
    {
        try
        {
            auto body = json::parse(req.body());

            if (!body.contains("p") || !body.contains("R"))
            {
                return make_response(http::status::bad_request, {{"error", "missing fields"}, {"required", {"p", "R"}}});
            }

            auto jp = body["p"];
            auto jR = body["R"];
            if (!jp.is_array() || !jR.is_array() || jp.size() != 3 || jR.size() != 9)
            {
                return make_response(http::status::bad_request, {{"error", "invalid shapes"}, {"p_len", jp.size()}, {"R_len", jR.size()}});
            }

            // Build pose JSON with server timestamp
            std::string frame = body.value("frame", std::string("scanner"));
            std::string source = body.value("source", std::string("api"));
            std::string ts = mrd::iso8601_now_ms();
            int64_t t_ms = mrd::now_ms_epoch();

            json pose_data = {
                {"p", jp},
                {"R", jR},
                {"frame", frame},
                {"source", source},
                {"ts", ts},
                {"t_ms", t_ms}
            };
            const std::string pose_json = pose_data.dump();

            // Update in-memory cache (for GET /v1/pose/current)
            {
                std::lock_guard<std::mutex> lock(state.latest_pose_mutex);
                state.latest_pose_json = pose_json;
            }

            // Also update PoseStore (keep for potential internal use)
            Pose pose{};
            pose.frame = frame;
            pose.source = source;
            for (int i = 0; i < 3; ++i)
                pose.p[i] = static_cast<double>(jp[i]);
            for (int i = 0; i < 9; ++i)
                pose.R[i] = static_cast<double>(jR[i]);
            pose.t = std::chrono::system_clock::now();
            state.poses.set(pose);

            // Queue pose for async persistence (NON-BLOCKING!)
            {
                std::lock_guard<std::mutex> lock(state.json_queue_mutex);
                state.json_write_queue.push({MarshalState::WriteType::POSE, pose_json});
            }
            state.json_queue_cv.notify_one();

            // WS: broadcast pose (non-fatal if WS not set)
            try
            {
                json evt = pose_data;
                evt["type"] = "pose";
                if (state.ws_emit_topic) {
                    state.ws_emit_topic(evt.dump(), "pose");
                }
            }
            catch (const std::exception &e)
            {
                LOG_WARN("req=" << req_id << " Pose WS broadcast failed: " << e.what());
            }

            return make_response(http::status::ok, {{"pose", pose_data}});
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::bad_request, {{"error", "bad json"}, {"what", e.what()}});
        }
    }

    // POST /v1/bio/signal
    // Body (JSON): { "source": "ecg", "data": [...], "rate_hz": 100.0 }
    // Server generates ts automatically
    if (req.method() == http::verb::post && req.target() == "/v1/bio/signal")
    {
        try
        {
            auto body = json::parse(req.body());

            if (!body.contains("source") || !body.contains("data") || !body.contains("rate_hz"))
            {
                return make_response(http::status::bad_request,
                    {{"error", "missing fields"}, {"required", {"source", "data", "rate_hz"}}});
            }

            if (!body["data"].is_array())
            {
                return make_response(http::status::bad_request, {{"error", "data must be an array"}});
            }

            // Server generates timestamp
            body["ts"] = mrd::iso8601_now_ms();
            const std::string bio_json = body.dump();

            // Update in-memory cache (for GET /v1/bio/latest)
            {
                std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
                state.latest_bio_json = bio_json;
            }

            // Queue bio for async persistence (NON-BLOCKING!)
            {
                std::lock_guard<std::mutex> lock(state.json_queue_mutex);
                state.json_write_queue.push({MarshalState::WriteType::BIO, bio_json});
            }
            state.json_queue_cv.notify_one();

            // WS: broadcast (topic="bio")
            try
            {
                // Ensure the message has a type for consumers who don't use topics yet
                json evt = body;
                evt["type"] = "bio";
                evt["t_ms"] = mrd::now_ms_epoch();
                
                // Broadcast to "bio" topic specifically
                if (state.ws_emit_topic) {
                    state.ws_emit_topic(evt.dump(), "bio");
                }
            }
            catch (const std::exception &e)
            {
                LOG_WARN("req=" << req_id << " Bio WS broadcast failed: " << e.what());
            }

            return make_response(http::status::ok, {{"count", body["data"].size()}});
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::bad_request, {{"error", "bad json"}, {"what", e.what()}});
        }
    }

    // GET /v1/bio/latest  (reads from in-memory cache)
    if (req.method() == http::verb::get && req.target() == "/v1/bio/latest")
    {
        std::string cached_json;
        {
            std::lock_guard<std::mutex> lock(state.latest_bio_mutex);
            cached_json = state.latest_bio_json;
        }

        if (!cached_json.empty())
        {
            try {
                return make_response(http::status::ok, json::parse(cached_json));
            } catch (...) {
                return make_response(http::status::internal_server_error,
                    {{"error", "failed to parse bio cache"}});
            }
        }
        return make_response(http::status::no_content, json::object());
    }

    // GET /v1/config
    if (req.method() == http::verb::get && req.target() == "/v1/config")
    {
        return make_response(http::status::ok, {{"data_dir", state.data_dir}, {"ws_port", 8090}, {"max_entries", 100000}});
    }

    // POST /v1/mrd/callback  (receives reconstructed images from async recon service)
    if (req.method() == http::verb::post && req.target() == "/v1/mrd/callback")
    {
        using namespace std::string_literals;
        try
        {
            if (!state.mrd_sink)
                throw std::runtime_error("MRD sink unavailable");

            auto stream_header = std::string(req["X-MRD-Stream"]);
            if (stream_header.empty())
                throw std::runtime_error("missing X-MRD-Stream header");

            const std::string &body = req.body();
            if (body.empty())
                throw std::runtime_error("empty body");

            // Optional job tracking ID (for debugging/monitoring)
            auto job_id = std::string(req["X-MRD-Job-Id"]);

            LOG_INFO("req=" << req_id << " Received reconstruction callback: "
                      << "stream=" << stream_header
                      << ", size=" << body.size() << " bytes"
                      << (job_id.empty() ? "" : ", job_id=" + job_id));

            // Validate ImageHeader
            if (body.size() < sizeof(ISMRMRD::ImageHeader))
                throw std::runtime_error("body too small for ISMRMRD ImageHeader");

            const auto *img_header = reinterpret_cast<const ISMRMRD::ImageHeader *>(body.data());
            mrd::ImageDimensions dims;
            dims.spatial = {
                static_cast<hsize_t>(img_header->matrix_size[0]),
                static_cast<hsize_t>(img_header->matrix_size[1]),
                static_cast<hsize_t>(img_header->matrix_size[2] ? img_header->matrix_size[2] : 1)
            };
            dims.channels = img_header->channels ? static_cast<hsize_t>(img_header->channels) : 1;

            if (dims.spatial[0] == 0 || dims.spatial[1] == 0)
                throw std::runtime_error("invalid matrix size in header");

            auto element_type = mrd::element_type_from_ismrmrd(img_header->data_type);
            const size_t payload_bytes = body.size() - sizeof(ISMRMRD::ImageHeader);
            const size_t expected_bytes = mrd::element_type_bytes(element_type) *
                                          static_cast<size_t>(dims.spatial[0]) *
                                          static_cast<size_t>(dims.spatial[1]) *
                                          static_cast<size_t>(dims.spatial[2]) *
                                          static_cast<size_t>(dims.channels);

            if (payload_bytes != expected_bytes)
                throw std::runtime_error("payload size (" + std::to_string(payload_bytes) +
                                         ") does not match expected (" + std::to_string(expected_bytes) + ")");

            std::string header_xml = mrd::default_ismrmrd_header(dims, element_type, stream_header);
            std::string session_header = std::string(req["X-MRD-Session"]);
            const void *payload = body.data() + sizeof(ISMRMRD::ImageHeader);

            // THREAD-SAFE: MrdSink::append_frame has internal mutex protection
            auto result = state.mrd_sink->append_frame(
                stream_header,
                dims,
                element_type,
                header_xml,
                payload,
                payload_bytes,
                session_header
            );

            LOG_INFO("req=" << req_id << " Stored reconstructed callback image: "
                      << "stream=" << stream_header
                      << ", frame=" << result.frame_index
                      << ", path=" << result.file_path.string());

            json resp_data = {
                {"status", "stored"},
                {"path", result.file_path.string()},
                {"stream", result.stream_id},
                {"frame_index", result.frame_index},
                {"flushed", result.flushed},
                {"ts", result.timestamp},
                {"t_ms", mrd::now_ms_epoch()},
                {"dims", {result.dims.spatial[0], result.dims.spatial[1], result.dims.spatial[2]}},
                {"channels", result.dims.channels},
                {"datatype", mrd::element_type_to_string(result.element_type)},
                {"size_bytes", result.bytes},
                {"callback", true}
            };

            if (!job_id.empty()) resp_data["job_id"] = job_id;

            // Forward reconstructed image to scanner if reply-to URL was provided
            if (!job_id.empty()) {
                std::string reply_to;
                {
                    std::lock_guard<std::mutex> lock(state.reply_to_mutex);
                    auto it = state.reply_to_urls.find(job_id);
                    if (it != state.reply_to_urls.end()) {
                        reply_to = it->second;
                        state.reply_to_urls.erase(it);
                    }
                }

                if (!reply_to.empty()) {
                    LOG_INFO("req=" << req_id << " Forwarding reconstructed image to scanner: "
                              << reply_to << " (job_id=" << job_id << ")");

                    // ASYNC: Forward to scanner without blocking callback response
                    std::string image_data = body;
                    std::thread([reply_to, image_data = std::move(image_data),
                                 stream_header, job_id]() {
                        try {
                            namespace beast = boost::beast;
                            namespace net = boost::asio;
                            using tcp = net::ip::tcp;

                            std::string host = reply_to;
                            std::string port = "80";
                            std::string path = "/";

                            size_t protocol_pos = host.find("://");
                            if (protocol_pos != std::string::npos) {
                                host = host.substr(protocol_pos + 3);
                            }
                            size_t path_pos = host.find("/");
                            if (path_pos != std::string::npos) {
                                path = host.substr(path_pos);
                                host = host.substr(0, path_pos);
                            }
                            size_t port_pos = host.find(":");
                            if (port_pos != std::string::npos) {
                                port = host.substr(port_pos + 1);
                                host = host.substr(0, port_pos);
                            }

                            net::io_context ioc;
                            tcp::resolver resolver(ioc);
                            beast::tcp_stream stream(ioc);

                            auto const results = resolver.resolve(host, port);
                            stream.connect(results);

                            http::request<http::string_body> scanner_req{http::verb::post, path, 11};
                            scanner_req.set(http::field::host, host);
                            scanner_req.set(http::field::user_agent, "MRI-Marshal/1.0");
                            scanner_req.set(http::field::content_type, "application/octet-stream");
                            scanner_req.set("X-MRD-Stream", stream_header);
                            scanner_req.set("X-MRD-Job-Id", job_id);
                            scanner_req.body() = image_data;
                            scanner_req.prepare_payload();

                            http::write(stream, scanner_req);

                            beast::flat_buffer buf;
                            http::response<http::string_body> scanner_res;
                            http::read(stream, buf, scanner_res);

                            beast::error_code ec;
                            stream.socket().shutdown(tcp::socket::shutdown_both, ec);

                            LOG_INFO("Scanner reply-to response: HTTP "
                                      << scanner_res.result_int() << " (job_id=" << job_id << ")");
                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("Failed to forward image to scanner: "
                                      << e.what() << " (job_id=" << job_id << ")");
                        }
                    }).detach();
                }
            }

            return make_response(http::status::ok, resp_data);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("req=" << req_id << " Callback processing failed: " << e.what());
            return make_response(http::status::bad_request, {{"error", e.what()}});
        }
    }

    // POST /v1/mrd/frame  (append ISMRMRD data with smart type detection)
    if (req.method() == http::verb::post && req.target() == "/v1/mrd/frame")
    {
        using namespace std::string_literals;
        try
        {
            if (!state.mrd_sink)
                throw std::runtime_error("MRD sink unavailable");
            auto stream_header = std::string(req["X-MRD-Stream"]);
            if (stream_header.empty())
                throw std::runtime_error("missing X-MRD-Stream header");
            const std::string &body = req.body();
            if (body.empty())
                throw std::runtime_error("empty body");

            // SMART DETECTION: Determine data type automatically
            mrd::MrdDataType detected_type = mrd::detect_mrd_type(body.data(), body.size());
            LOG_INFO("req=" << req_id << " Detected MRD type: " << mrd::mrd_data_type_to_string(detected_type)
                      << " (stream=" << stream_header << ", size=" << body.size() << " bytes)");

            // Route based on detected type
            switch (detected_type)
            {
            case mrd::MrdDataType::ACQUISITION:
                // RAW K-SPACE DATA - Forward to external reconstruction service ASYNCHRONOUSLY
                {
                    if (!state.recon_enabled) {
                        return make_response(http::status::not_implemented, {
                            {"error", "reconstruction service not configured"},
                            {"detected_type", "ACQUISITION"},
                            {"message", "Raw k-space detected but no reconstruction service available."},
                            {"hint", "Start marshal with --recon-endpoint http://localhost:9002"}
                        });
                    }

                    LOG_INFO("req=" << req_id << " ASYNC: Queuing k-space for reconstruction: "
                              << state.recon_endpoint);

                    // Capture data for async task (COPY - body will be invalid after response)
                    std::string kspace_data = body;
                    std::string stream_id = stream_header;
                    std::string session_id = std::string(req["X-MRD-Session"]);
                    std::string recon_endpoint = state.recon_endpoint;

                    // Build callback URL (Docker internal hostname)
                    const std::string callback_url = "http://mri-marshal:8080/v1/mrd/callback";

                    // Generate unique job ID for tracking
                    const std::string job_id = stream_id + "_" + mrd::iso8601_now_ms();

                    // Capture reply-to URL if provided (for returning images to scanner)
                    std::string reply_to = std::string(req["X-MRD-Reply-To"]);
                    if (!reply_to.empty()) {
                        std::lock_guard<std::mutex> lock(state.reply_to_mutex);
                        state.reply_to_urls[job_id] = reply_to;
                    }

                    // ASYNC: Spawn detached thread to forward to recon
                    // Note: Using detached thread is acceptable here because:
                    //   1. The thread only does network I/O (no shared state mutation)
                    //   2. Failure is logged but non-fatal (recon might be slow/down)
                    //   3. The callback endpoint handles actual storage
                    std::thread([kspace_data = std::move(kspace_data),
                                 stream_id,          // Copy for thread (original moved above)
                                 session_id = std::move(session_id),
                                 recon_endpoint = std::move(recon_endpoint),
                                 callback_url,       // Copy for thread (need original for response)
                                 job_id]() {         // Copy for thread (need original for response)
                        try {
                            namespace beast = boost::beast;
                            namespace net = boost::asio;
                            using tcp = net::ip::tcp;

                            // Parse endpoint URL (format: http://host:port)
                            std::string host = recon_endpoint;
                            std::string port = "9002";

                            size_t protocol_pos = host.find("://");
                            if (protocol_pos != std::string::npos) {
                                host = host.substr(protocol_pos + 3);
                            }
                            size_t port_pos = host.find(":");
                            if (port_pos != std::string::npos) {
                                port = host.substr(port_pos + 1);
                                host = host.substr(0, port_pos);
                            }
                            size_t path_pos = port.find("/");
                            if (path_pos != std::string::npos) {
                                port = port.substr(0, path_pos);
                            }

                            // Connect to reconstruction service
                            net::io_context ioc;
                            tcp::resolver resolver(ioc);
                            beast::tcp_stream stream(ioc);

                            auto const results = resolver.resolve(host, port);
                            stream.connect(results);

                            // Prepare HTTP POST request with CALLBACK HEADER
                            http::request<http::string_body> recon_req{http::verb::post, "/reconstruct", 11};
                            recon_req.set(http::field::host, host);
                            recon_req.set(http::field::user_agent, "MRI-Marshal/1.0");
                            recon_req.set(http::field::content_type, "application/octet-stream");
                            recon_req.set("X-MRD-Stream", stream_id);
                            recon_req.set("X-MRD-Session", session_id);
                            recon_req.set("X-MRD-Callback", callback_url);  // Callback URL for async response
                            recon_req.set("X-MRD-Job-Id", job_id);          // Job tracking ID
                            recon_req.body() = kspace_data;
                            recon_req.prepare_payload();

                            // Send request
                            http::write(stream, recon_req);

                            // Read acknowledgment (should be 202 Accepted quickly)
                            beast::flat_buffer recon_buffer;
                            http::response<http::string_body> recon_res;
                            http::read(stream, recon_buffer, recon_res);

                            // Close connection
                            beast::error_code ec;
                            stream.socket().shutdown(tcp::socket::shutdown_both, ec);

                            LOG_INFO("ASYNC: Recon service acknowledged job " << job_id
                                      << ": HTTP " << recon_res.result_int());

                            if (recon_res.result() != http::status::accepted &&
                                recon_res.result() != http::status::ok &&
                                recon_res.result() != http::status::created) {
                                LOG_WARN("ASYNC: Recon returned unexpected status: "
                                          << recon_res.result_int() << " - " << recon_res.body());
                            }
                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("ASYNC: Failed to forward to recon: "
                                      << e.what());
                        }
                    }).detach();

                    // Return 202 IMMEDIATELY - don't wait for reconstruction
                    return make_response(http::status::accepted, {
                        {"status", "processing"},
                        {"job_id", job_id},
                        {"stream", stream_header},
                        {"recon_endpoint", state.recon_endpoint},
                        {"callback_url", callback_url},
                        {"message", "K-space queued for asynchronous reconstruction. "
                                    "Results will be POSTed to callback URL."}
                    });
                }

            case mrd::MrdDataType::HDF5_FILE:
                // COMPLETE HDF5 FILE - Forward to /v1/mrd/ingest
                LOG_INFO("req=" << req_id << " HDF5 file detected, forwarding to /v1/mrd/ingest");
                {
                    auto entry = mrd::ingest_payload(state, body.data(), body.size(), "http");
                    return make_response(http::status::created, entry);
                }

            case mrd::MrdDataType::IMAGE:
                // RECONSTRUCTED IMAGE - Process normally (existing logic below)
                LOG_INFO("req=" << req_id << " Reconstructed image (ImageHeader) detected, processing normally");
                break;

            case mrd::MrdDataType::UNKNOWN:
            default:
                LOG_ERROR("req=" << req_id << " Unknown or invalid MRD data format");
                return make_response(http::status::bad_request, {
                    {"error", "unknown MRD data format"},
                    {"detected_type", "UNKNOWN"},
                    {"message", "Could not identify data as AcquisitionHeader, ImageHeader, or HDF5 file"},
                    {"stream", stream_header},
                    {"body_size", body.size()}
                });
            }

            // EXISTING LOGIC: Process reconstructed image (ImageHeader)
            if (body.size() < sizeof(ISMRMRD::ImageHeader))
                throw std::runtime_error("body too small for ISMRMRD ImageHeader");

            const auto *img_header = reinterpret_cast<const ISMRMRD::ImageHeader *>(body.data());
            mrd::ImageDimensions dims;
            dims.spatial = {static_cast<hsize_t>(img_header->matrix_size[0]),
                            static_cast<hsize_t>(img_header->matrix_size[1]),
                            static_cast<hsize_t>(img_header->matrix_size[2])};
            dims.channels = img_header->channels ? static_cast<hsize_t>(img_header->channels) : 1;
            if (dims.spatial[0] == 0 || dims.spatial[1] == 0)
                throw std::runtime_error("invalid matrix size in header");

            auto element_type = mrd::element_type_from_ismrmrd(img_header->data_type);
            const size_t payload_bytes = body.size() - sizeof(ISMRMRD::ImageHeader);
            const size_t expected_bytes = mrd::element_type_bytes(element_type) *
                                          static_cast<size_t>(dims.spatial[0]) *
                                          static_cast<size_t>(dims.spatial[1]) *
                                          static_cast<size_t>(dims.spatial[2] ? dims.spatial[2] : 1) *
                                          static_cast<size_t>(dims.channels);
            if (payload_bytes != expected_bytes)
                throw std::runtime_error("payload size does not match header");

            std::string header_xml = mrd::default_ismrmrd_header(dims, element_type, stream_header);
            std::string session_header = std::string(req["X-MRD-Session"]);
            const void *payload = body.data() + sizeof(ISMRMRD::ImageHeader);

            auto result = state.mrd_sink->append_frame(stream_header,
                                                      dims,
                                                      element_type,
                                                      header_xml,
                                                      payload,
                                                      payload_bytes,
                                                      session_header);

            json resp_data = {
                {"path", result.file_path.string()},
                {"stream", result.stream_id},
                {"frame_index", result.frame_index},
                {"flushed", result.flushed},
                {"ts", result.timestamp},
                {"t_ms", mrd::now_ms_epoch()},
                {"dims", {result.dims.spatial[0], result.dims.spatial[1], result.dims.spatial[2]}},
                {"channels", result.dims.channels},
                {"datatype", mrd::element_type_to_string(result.element_type)},
                {"size_bytes", result.bytes}};

            return make_response(http::status::ok, resp_data);
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::bad_request, {{"error", e.what()}});
        }
    }

    // POST /v1/mrd/ingest  (writes ${data_dir}/mrd/*.mrd with smart type detection)
    if (req.method() == http::verb::post && req.target() == "/v1/mrd/ingest")
    {
        try
        {
            const std::string &body = req.body();
            if (body.empty())
            {
                return make_response(http::status::bad_request, {{"error", "empty body"}});
            }

            // SMART DETECTION: Determine data type automatically
            mrd::MrdDataType detected_type = mrd::detect_mrd_type(body.data(), body.size());
            LOG_INFO("req=" << req_id << " /v1/mrd/ingest - Detected type: " << mrd::mrd_data_type_to_string(detected_type)
                      << " (size=" << body.size() << " bytes)");

            // Route based on detected type
            switch (detected_type)
            {
            case mrd::MrdDataType::ACQUISITION:
                // RAW K-SPACE DATA - Forward to external reconstruction service
                {
                    if (!state.recon_enabled) {
                        return make_response(http::status::not_implemented, {
                            {"error", "reconstruction service not configured"},
                            {"detected_type", "ACQUISITION"},
                            {"message", "Raw k-space detected but no reconstruction service available."},
                            {"hint", "Start marshal with --recon-endpoint http://localhost:9002"}
                        });
                    }

                    LOG_INFO("req=" << req_id << " /ingest: Forwarding raw k-space to reconstruction service: "
                              << state.recon_endpoint);

                    try {
                        namespace beast = boost::beast;
                        namespace net = boost::asio;
                        using tcp = net::ip::tcp;

                        // Parse endpoint URL
                        std::string host = state.recon_endpoint;
                        std::string port = "9002";

                        size_t protocol_pos = host.find("://");
                        if (protocol_pos != std::string::npos) {
                            host = host.substr(protocol_pos + 3);
                        }
                        size_t port_pos = host.find(":");
                        if (port_pos != std::string::npos) {
                            port = host.substr(port_pos + 1);
                            host = host.substr(0, port_pos);
                        }
                        size_t path_pos = port.find("/");
                        if (path_pos != std::string::npos) {
                            port = port.substr(0, path_pos);
                        }

                        // Connect to reconstruction service
                        net::io_context ioc;
                        tcp::resolver resolver(ioc);
                        beast::tcp_stream stream(ioc);

                        auto const results = resolver.resolve(host, port);
                        stream.connect(results);

                        // Get stream name from header or generate one
                        std::string stream_name = std::string(req["X-MRD-Stream"]);
                        if (stream_name.empty()) {
                            stream_name = "ingest_" + mrd::iso8601_now_ms();
                        }

                        // Prepare HTTP POST request
                        http::request<http::string_body> recon_req{http::verb::post, "/reconstruct", 11};
                        recon_req.set(http::field::host, host);
                        recon_req.set(http::field::user_agent, "MRI-Marshal/1.0");
                        recon_req.set(http::field::content_type, "application/octet-stream");
                        recon_req.set("X-MRD-Stream", stream_name);
                        recon_req.body() = body;
                        recon_req.prepare_payload();

                        http::write(stream, recon_req);

                        beast::flat_buffer recon_buffer;
                        http::response<http::string_body> recon_res;
                        http::read(stream, recon_buffer, recon_res);

                        beast::error_code ec;
                        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

                        LOG_INFO("req=" << req_id << " /ingest: Reconstruction service responded: HTTP "
                                  << recon_res.result_int());

                        if (recon_res.result() != http::status::ok && recon_res.result() != http::status::created) {
                            return make_response(http::status::bad_gateway, {
                                {"error", "reconstruction service failed"},
                                {"service_status", recon_res.result_int()},
                                {"service_response", recon_res.body()}
                            });
                        }

                        // For /ingest, save reconstructed data as complete file
                        const std::string& reconstructed = recon_res.body();

                        // Use ingest_payload to save the reconstructed data as a complete file
                        auto entry = mrd::ingest_payload(state, reconstructed.data(), reconstructed.size(), "http-reconstructed");

                        LOG_INFO("req=" << req_id << " /ingest: Saved reconstructed data as complete file");

                        // Forward to scanner if reply-to URL provided
                        std::string reply_to = std::string(req["X-MRD-Reply-To"]);
                        if (!reply_to.empty()) {
                            LOG_INFO("req=" << req_id << " /ingest: Forwarding reconstructed image to scanner: "
                                      << reply_to);

                            std::thread([reply_to, reconstructed, stream_name]() {
                                try {
                                    namespace beast = boost::beast;
                                    namespace net = boost::asio;
                                    using tcp = net::ip::tcp;

                                    std::string host = reply_to;
                                    std::string port = "80";
                                    std::string path = "/";

                                    size_t protocol_pos = host.find("://");
                                    if (protocol_pos != std::string::npos) {
                                        host = host.substr(protocol_pos + 3);
                                    }
                                    size_t path_pos = host.find("/");
                                    if (path_pos != std::string::npos) {
                                        path = host.substr(path_pos);
                                        host = host.substr(0, path_pos);
                                    }
                                    size_t port_pos = host.find(":");
                                    if (port_pos != std::string::npos) {
                                        port = host.substr(port_pos + 1);
                                        host = host.substr(0, port_pos);
                                    }

                                    net::io_context ioc;
                                    tcp::resolver resolver(ioc);
                                    beast::tcp_stream stream(ioc);

                                    auto const results = resolver.resolve(host, port);
                                    stream.connect(results);

                                    http::request<http::string_body> scanner_req{http::verb::post, path, 11};
                                    scanner_req.set(http::field::host, host);
                                    scanner_req.set(http::field::user_agent, "MRI-Marshal/1.0");
                                    scanner_req.set(http::field::content_type, "application/octet-stream");
                                    scanner_req.set("X-MRD-Stream", stream_name);
                                    scanner_req.body() = reconstructed;
                                    scanner_req.prepare_payload();

                                    http::write(stream, scanner_req);

                                    beast::flat_buffer buf;
                                    http::response<http::string_body> scanner_res;
                                    http::read(stream, buf, scanner_res);

                                    beast::error_code ec;
                                    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

                                    LOG_INFO("/ingest: Scanner reply-to response: HTTP "
                                              << scanner_res.result_int());
                                }
                                catch (const std::exception& e) {
                                    LOG_ERROR("/ingest: Failed to forward image to scanner: "
                                              << e.what());
                                }
                            }).detach();

                            entry["reply_to"] = reply_to;
                        }

                        // Add reconstruction info to response
                        entry["reconstructed"] = true;
                        entry["message"] = "Raw k-space reconstructed and saved as complete file";

                        return make_response(http::status::created, entry);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("req=" << req_id << " /ingest reconstruction forwarding failed: "
                                  << e.what());
                        return make_response(http::status::bad_gateway, {
                            {"error", "reconstruction forwarding failed"},
                            {"what", e.what()},
                            {"endpoint", state.recon_endpoint}
                        });
                    }
                }

            case mrd::MrdDataType::IMAGE:
                // SINGLE IMAGE FRAME - Should use /v1/mrd/frame instead
                LOG_WARN("req=" << req_id << " Single ImageHeader detected on /v1/mrd/ingest. "
                          << "Consider using /v1/mrd/frame for streaming.");
                // Allow it but log warning (user might intentionally upload single frame)
                break;

            case mrd::MrdDataType::HDF5_FILE:
                // COMPLETE HDF5 FILE - This is the expected format
                LOG_INFO("req=" << req_id << " HDF5 file detected, proceeding with ingest");
                break;

            case mrd::MrdDataType::UNKNOWN:
            default:
                LOG_ERROR("req=" << req_id << " Unknown data format for /v1/mrd/ingest");
                return make_response(http::status::bad_request, {
                    {"error", "unknown data format"},
                    {"detected_type", "UNKNOWN"},
                    {"message", "Expected complete ISMRMRD HDF5 file. Could not identify data format."},
                    {"body_size", body.size()}
                });
            }

            // Process the data (works for both HDF5 files and single frames)
            auto entry = mrd::ingest_payload(state, body.data(), body.size(), "http");
            return make_response(http::status::created, entry);
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", "ingest failed"}, {"what", e.what()}});
        }
    }

    // GET /v1/mrd/frame?path=...&index=...  (returns frame metadata for client to read via SWMR)
    // index < 0 or omitted = latest frame
    // Client receives metadata and does direct HDF5 SWMR read
    if (req.method() == http::verb::get && std::string(req.target()).rfind("/v1/mrd/frame", 0) == 0)
    {
        try
        {
            // Parse query params
            std::string target_str(req.target());
            auto qpos = target_str.find('?');
            std::string mrd_path;
            int64_t frame_index = -1;

            if (qpos != std::string::npos)
            {
                auto query = target_str.substr(qpos + 1);
                // Parse path=...
                auto path_pos = query.find("path=");
                if (path_pos != std::string::npos)
                {
                    auto val_start = path_pos + 5;
                    auto val_end = query.find('&', val_start);
                    mrd_path = (val_end == std::string::npos)
                        ? query.substr(val_start)
                        : query.substr(val_start, val_end - val_start);
                }
                // Parse index=...
                auto idx_pos = query.find("index=");
                if (idx_pos != std::string::npos)
                {
                    auto val_start = idx_pos + 6;
                    auto val_end = query.find('&', val_start);
                    auto idx_str = (val_end == std::string::npos)
                        ? query.substr(val_start)
                        : query.substr(val_start, val_end - val_start);
                    try { frame_index = std::stoll(idx_str); } catch (...) {}
                }
            }

            if (mrd_path.empty())
            {
                // Try to get path from latest.json
                fs::path latest_path = fs::path(state.data_dir) / "mrd" / "latest.json";
                if (fs::exists(latest_path))
                {
                    std::string s;
                    if (read_file_all(latest_path, s) && !s.empty())
                    {
                        auto j = json::parse(s, nullptr, false);
                        if (!j.is_discarded() && j.contains("path"))
                            mrd_path = j["path"].get<std::string>();
                    }
                }
            }

            if (mrd_path.empty())
                return make_response(http::status::bad_request, {{"error", "missing path param and no latest.json"}});

            if (!state.mrd_sink)
                return make_response(http::status::service_unavailable, {{"error", "MRD sink unavailable"}});

            auto result = state.mrd_sink->read_frame(mrd_path, frame_index);

            if (!result.success)
                return make_response(http::status::no_content, {});

            // Return metadata as JSON - client does the actual HDF5 SWMR read
            return make_response(http::status::ok, {
                {"path", mrd_path},
                {"frame_index", result.frame_index},
                {"total_frames", result.total_frames},
                {"dims", {
                    {"x", result.dims.spatial[0]},
                    {"y", result.dims.spatial[1]},
                    {"z", result.dims.spatial[2]}
                }},
                {"channels", result.dims.channels},
                {"datatype", mrd::element_type_to_string(result.element_type)}
            });
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", e.what()}});
        }
    }

    // GET /v1/mrd/ingest?path=...  (returns file metadata for client to read/copy)
    // If no path provided, uses latest.json to find current file
    if (req.method() == http::verb::get && std::string(req.target()).rfind("/v1/mrd/ingest", 0) == 0)
    {
        try
        {
            std::string target_str(req.target());
            std::string mrd_path;

            // Parse path= query param
            auto qpos = target_str.find('?');
            if (qpos != std::string::npos)
            {
                auto query = target_str.substr(qpos + 1);
                auto path_pos = query.find("path=");
                if (path_pos != std::string::npos)
                {
                    auto val_start = path_pos + 5;
                    auto val_end = query.find('&', val_start);
                    mrd_path = (val_end == std::string::npos)
                        ? query.substr(val_start)
                        : query.substr(val_start, val_end - val_start);
                }
            }

            // If no path provided, get from latest.json
            if (mrd_path.empty())
            {
                fs::path latest_path = fs::path(state.data_dir) / "mrd" / "latest.json";
                if (fs::exists(latest_path))
                {
                    std::string s;
                    if (read_file_all(latest_path, s) && !s.empty())
                    {
                        auto j = json::parse(s, nullptr, false);
                        if (!j.is_discarded() && j.contains("path"))
                            mrd_path = j["path"].get<std::string>();
                    }
                }
            }

            if (mrd_path.empty())
                return make_response(http::status::bad_request, {{"error", "missing path param and no latest.json"}});

            if (!fs::exists(mrd_path))
                return make_response(http::status::not_found, {{"error", "file not found"}});

            // Get file size and metadata
            auto file_size = fs::file_size(mrd_path);
            std::string filename = fs::path(mrd_path).filename().string();

            // Return metadata as JSON - client reads/copies the file directly
            return make_response(http::status::ok, {
                {"path", mrd_path},
                {"filename", filename},
                {"size_bytes", file_size}
            });
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", e.what()}});
        }
    }

    // GET /v1/mrd/latest  (reads from in-memory cache)
    if (req.method() == http::verb::get && req.target() == "/v1/mrd/latest")
    {
        std::string cached_json;
        {
            std::lock_guard<std::mutex> lock(state.latest_mrd_mutex);
            cached_json = state.latest_mrd_json;
        }

        if (!cached_json.empty())
        {
            try {
                return make_response(http::status::ok, json::parse(cached_json));
            } catch (...) {
                return make_response(http::status::internal_server_error, {{"error", "failed to parse latest JSON"}});
            }
        }
        return make_response(http::status::no_content, {});
    }

    // GET /v1/mrd/since?ts=...&limit=...&last=...  (reads ${data_dir}/mrd/index.jsonl)
    // - ts + limit: return frames where ts > provided_ts, up to limit
    // - last: return the last N frames (most recent)
    // PERFORMANCE FIX: Uses efficient reverse reading for last=N queries
    if (req.method() == http::verb::get && std::string(req.target()).rfind("/v1/mrd/since", 0) == 0)
    {
        try
        {
            std::string ts;
            size_t limit = 0;
            size_t last = 0;
            parse_ts_limit(std::string(req.target()), ts, limit, last);

            if (ts.empty() && last == 0)
            {
                return make_response(http::status::bad_request, {{"error", "missing ts or last param"}});
            }

            fs::path index = fs::path(state.data_dir) / "mrd" / "index.jsonl";
            nlohmann::json out = nlohmann::json::array();

            if (!ts.empty())
            {
                // ts-based filtering: return frames after timestamp (forward scan)
                std::ifstream f(index);
                if (f)
                {
                    std::string line;
                    while (std::getline(f, line))
                    {
                        if (line.empty())
                            continue;
                        nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
                        if (j.is_discarded())
                            continue;
                        if (j.value("ts", std::string()) > ts)
                        {
                            out.push_back(j);
                            if (limit && out.size() >= limit)
                                break;
                        }
                    }
                }
            }
            else
            {
                // last-based: return the last N frames
                // OPTIMIZATION: Read from end of file to avoid loading entire file into memory
                std::ifstream f(index, std::ios::ate | std::ios::binary);
                if (f)
                {
                    auto file_size = f.tellg();
                    if (file_size > 0)
                    {
                        std::vector<std::string> lines;
                        lines.reserve(last);

                        // Read backwards from end of file
                        std::streamoff pos = file_size;
                        std::string current_line;

                        // Start from end - 1 to skip potential trailing newline
                        pos--;
                        while (pos >= 0 && lines.size() < last)
                        {
                            f.seekg(pos);
                            char c;
                            f.get(c);

                            if (c == '\n')
                            {
                                if (!current_line.empty())
                                {
                                    // Reverse the line since we read it backwards
                                    std::reverse(current_line.begin(), current_line.end());
                                    lines.push_back(std::move(current_line));
                                    current_line.clear();
                                }
                            }
                            else
                            {
                                current_line.push_back(c);
                            }
                            pos--;
                        }

                        // Handle last line (at start of file)
                        if (!current_line.empty() && lines.size() < last)
                        {
                            std::reverse(current_line.begin(), current_line.end());
                            lines.push_back(std::move(current_line));
                        }

                        // Reverse to get chronological order
                        std::reverse(lines.begin(), lines.end());

                        // Parse and add to output
                        for (const auto &line : lines)
                        {
                            if (line.empty())
                                continue;
                            nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
                            if (!j.is_discarded())
                                out.push_back(std::move(j));
                        }
                    }
                }
            }

            return make_response(http::status::ok, out);
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", "since failed"}, {"what", e.what()}});
        }
    }

    // 404 fallback
    {
        return make_response(http::status::not_found, {{"error", "not found"}});
    }
}

// -------- HTTP server --------

class HttpServer
{
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket socket_;
    MarshalState &state_;

public:
    HttpServer(boost::asio::io_context &ioc, boost::asio::ip::tcp::endpoint ep, MarshalState &s)
        : acceptor_(ioc), socket_(ioc), state_(s)
    {
        boost::system::error_code ec;
        acceptor_.open(ep.protocol(), ec);
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
        acceptor_.bind(ep, ec);
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        do_accept();
    }

private:
    void do_accept()
    {
        acceptor_.async_accept(socket_, [this](auto ec)
                               {
            if (!ec) std::make_shared<Session>(std::move(socket_), state_)->run();
            do_accept(); });
    }

    struct Session : std::enable_shared_from_this<Session>
    {
        boost::asio::ip::tcp::socket socket;
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> req;
        MarshalState &state;

        Session(boost::asio::ip::tcp::socket &&s, MarshalState &st)
            : socket(std::move(s)), state(st) {}

        void run() { do_read(); }

        void do_read()
        {
            auto self = shared_from_this();
            self->req = {};
            auto parser = std::make_shared<http::request_parser<http::string_body>>();
            parser->body_limit(state.max_body_bytes);

            http::async_read(socket, buffer, *parser, [self, parser](auto ec, auto)
                             {
                if (ec)
                {
                    if (ec == http::error::body_limit)
                    {
                        using nlohmann::json;
                        unsigned version = parser->get().version();
                        if (version == 0)
                            version = 11;
                        http::response<http::string_body> res{http::status::payload_too_large, version};
                        res.set(http::field::content_type, "application/json");
                        res.body() = json{{"error", "request body exceeds limit"}, {"limit_bytes", self->state.max_body_bytes}}.dump();
                        res.prepare_payload();
                        res.keep_alive(false);
                        self->respond(std::move(res));
                    }
                    return;
                }

                self->req = parser->release();
                self->handle(); });
        }

        void respond(http::response<http::string_body> &&res)
        {
            auto self = shared_from_this();
            const bool keep_alive = req.keep_alive() && res.keep_alive();
            auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
            sp->set(http::field::server, "marshal-beast");
            sp->keep_alive(keep_alive);

            http::async_write(socket, *sp, [self, sp, keep_alive](boost::beast::error_code, std::size_t)
                              {
                boost::system::error_code ignored;
                if (keep_alive)
                {
                    self->do_read();
                }
                else
                {
                    self->socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    self->socket.close(ignored);
                }
            });
        }

        void handle()
        {
            auto rid = marshal_log::generate_request_id();
            auto start = std::chrono::steady_clock::now();
            auto res = handle_http_request(req, state, rid);
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            LOG_INFO("req=" << rid
                << " " << req.method_string()
                << " " << req.target()
                << " status=" << static_cast<int>(res.result())
                << " ms=" << elapsed_ms);
            respond(std::move(res));
        }
    };
};
