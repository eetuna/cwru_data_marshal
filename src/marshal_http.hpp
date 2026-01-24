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
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>

#include <nlohmann/json.hpp>
#include "mrd_io.hpp"

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
http::response<http::string_body> handle_http_request(const http::request<Body> &req, MarshalState &state)
{
    using nlohmann::json;

    auto make_response = [&](http::status status, json body)
    {
        http::response<http::string_body> res{status, req.version()};
        res.set(http::field::content_type, "application/json");
        
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
        return make_response(http::status::ok, {{"uptime_s", up}});
    }

    // GET /v1/pose/current
    if (req.method() == http::verb::get && req.target() == "/v1/pose/current")
    {
        auto p = state.poses.get();
        auto jpose = pose_to_json(p);
        jpose["ts"] = iso8601_now();
        jpose["t_ms"] = mrd::now_ms_epoch();
        return make_response(http::status::ok, {{"pose", jpose}, {"source", p.source}});
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

            Pose pose{};
            pose.frame = body.value("frame", std::string("scanner"));
            pose.source = body.value("source", std::string("api"));
            for (int i = 0; i < 3; ++i)
                pose.p[i] = static_cast<double>(jp[i]);
            for (int i = 0; i < 9; ++i)
                pose.R[i] = static_cast<double>(jR[i]);
            pose.t = std::chrono::system_clock::now();

            state.poses.set(pose);

            // Persist pose to disk
            try
            {
                auto paths = mrd::resolve_sink_paths(state);
                fs::path pose_log = paths.index_root / "poses.jsonl";
                auto j_persist = pose_to_json(pose);
                j_persist["ts"] = mrd::iso8601_now_ms();
                mrd::append_line(pose_log, j_persist.dump());
            }
            catch (const std::exception &e)
            {
                std::cerr << "Pose persistence failed: " << e.what() << "\n";
            }

            // WS: broadcast pose (non-fatal if WS not set)
            try
            {
                nlohmann::json evt = {
                    {"type", "pose"},
                    {"p", {pose.p[0], pose.p[1], pose.p[2]}},
                    {"R", {pose.R[0], pose.R[1], pose.R[2], pose.R[3], pose.R[4], pose.R[5], pose.R[6], pose.R[7], pose.R[8]}},
                    {"ts", mrd::iso8601_now_ms()}};
                if (state.ws_emit_topic) {
                    state.ws_emit_topic(evt.dump(), "pose");
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Pose WS broadcast failed: " << e.what() << "\n";
            }

            auto jpose = pose_to_json(pose);
            jpose["ts"] = iso8601_now();
            jpose["t_ms"] = mrd::now_ms_epoch();

            return make_response(http::status::ok, {{"pose", jpose}});
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

            // Persist to disk
            try
            {
                auto paths = mrd::resolve_sink_paths(state);
                fs::path bio_log = paths.index_root / "bio.jsonl";
                mrd::append_line(bio_log, body.dump());
            }
            catch (const std::exception &e)
            {
                std::cerr << "Bio persistence failed: " << e.what() << "\n";
                // Continue to broadcast even if disk fails? Or fail? 
                // Following pose pattern: catch and continue, but we log it now.
            }

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
                std::cerr << "Bio WS broadcast failed: " << e.what() << "\n";
            }

            return make_response(http::status::ok, {{"count", body["data"].size()}});
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::bad_request, {{"error", "bad json"}, {"what", e.what()}});
        }
    }

    // GET /v1/bio/latest
    // Returns the most recent bio signal entry from bio.jsonl
    if (req.method() == http::verb::get && req.target() == "/v1/bio/latest")
    {
        try
        {
            auto paths = mrd::resolve_sink_paths(state);
            fs::path bio_log = paths.index_root / "bio.jsonl";

            if (!fs::exists(bio_log))
            {
                return make_response(http::status::no_content, json::object());
            }

            // Read the last line from bio.jsonl
            std::ifstream ifs(bio_log);
            std::string last_line;
            std::string line;
            while (std::getline(ifs, line))
            {
                if (!line.empty())
                    last_line = line;
            }

            if (last_line.empty())
            {
                return make_response(http::status::no_content, json::object());
            }

            auto bio_entry = json::parse(last_line);
            return make_response(http::status::ok, bio_entry);
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error,
                {{"error", "failed to read bio log"}, {"what", e.what()}});
        }
    }

    // GET /v1/config
    if (req.method() == http::verb::get && req.target() == "/v1/config")
    {
        return make_response(http::status::ok, {{"data_dir", state.data_dir}, {"ws_port", 8090}, {"max_entries", 100000}});
    }

    // POST /v1/mrd/frame  (append ISMRMRD Image to SWMR dataset)
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

    // POST /v1/mrd/ingest  (writes ${data_dir}/mrd/*.mrd and updates index/latest)
    if (req.method() == http::verb::post && req.target() == "/v1/mrd/ingest")
    {
        try
        {
            const std::string &body = req.body();
            if (body.empty())
            {
                return make_response(http::status::bad_request, {{"error", "empty body"}});
            }

            auto entry = mrd::ingest_payload(state, body.data(), body.size(), "http");
            return make_response(http::status::created, entry);
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", "ingest failed"}, {"what", e.what()}});
        }
    }

    // GET /v1/mrd/frame?path=...&index=...  (read frame from SWMR - safe while writing)
    // index < 0 or omitted = latest frame
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

            if (!result.success || result.data.empty())
                return make_response(http::status::no_content, {});

            // Return binary frame data with metadata in headers
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/octet-stream");
            res.set("X-MRD-Frame-Index", std::to_string(result.frame_index));
            res.set("X-MRD-Total-Frames", std::to_string(result.total_frames));
            res.set("X-MRD-Dims-X", std::to_string(result.dims.spatial[0]));
            res.set("X-MRD-Dims-Y", std::to_string(result.dims.spatial[1]));
            res.set("X-MRD-Dims-Z", std::to_string(result.dims.spatial[2]));
            res.set("X-MRD-Channels", std::to_string(result.dims.channels));
            res.set("X-MRD-Datatype", mrd::element_type_to_string(result.element_type));
            res.body().assign(reinterpret_cast<const char*>(result.data.data()), result.data.size());
            res.prepare_payload();
            return res;
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", e.what()}});
        }
    }

    // GET /v1/mrd/ingest?path=...  (download complete .mrd file)
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

            std::ifstream ifs(mrd_path, std::ios::binary);
            if (!ifs)
                return make_response(http::status::internal_server_error, {{"error", "failed to open file"}});

            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

            // Extract filename from path
            std::string filename = fs::path(mrd_path).filename().string();

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/octet-stream");
            res.set(http::field::content_disposition, "attachment; filename=\"" + filename + "\"");
            res.body() = std::move(content);
            res.prepare_payload();
            return res;
        }
        catch (const std::exception &e)
        {
            return make_response(http::status::internal_server_error, {{"error", e.what()}});
        }
    }

    // GET /v1/mrd/latest  (reads ${data_dir}/mrd/latest.json)
    if (req.method() == http::verb::get && req.target() == "/v1/mrd/latest")
    {
        fs::path latest_path = fs::path(state.data_dir) / "mrd" / "latest.json";
        if (fs::exists(latest_path))
        {
            std::string s;
            if (read_file_all(latest_path, s) && !s.empty())
            {
                try {
                    return make_response(http::status::ok, json::parse(s));
                } catch (...) {
                    return make_response(http::status::internal_server_error, {{"error", "failed to parse latest.json"}});
                }
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
            respond(handle_http_request(req, state));
        }
    };
};
