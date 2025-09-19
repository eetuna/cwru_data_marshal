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

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "marshal_state.hpp"
#include "common/pose.hpp" // Pose, PoseStore, pose_to_json

namespace http = boost::beast::http;
namespace fs = std::filesystem;

// -------- time / fs helpers --------

// RFC3339 UTC with milliseconds (e.g., 2025-09-12T14:59:01.234Z)
inline std::string iso8601_now_ms()
{
    using namespace std::chrono;
    auto now = time_point_cast<milliseconds>(system_clock::now());
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm = *std::gmtime(&tt);

    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);

    std::ostringstream oss;
    oss << base << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}

// Seconds-precision ISO8601 for pose endpoint (keeps your original behavior)
inline std::string iso8601_now()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%FT%TZ", std::gmtime(&t));
    return buf;
}

inline void ensure_dir(const fs::path &p)
{
    std::error_code ec;
    if (!fs::exists(p, ec))
    {
        fs::create_directories(p, ec);
        if (ec)
            throw std::runtime_error("create_directories failed: " + ec.message());
    }
}

// atomic file write: write to .tmp then rename
inline void // Atomic write helper
write_atomic(const fs::path &dst, const void *data, size_t n)
{
    fs::path tmp = dst;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f)
            throw std::runtime_error("open tmp failed: " + tmp.string());
        f.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(n));
        if (!f)
            throw std::runtime_error("write tmp failed: " + tmp.string());
        f.flush();
        f.close();
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec)
        throw std::runtime_error("rename tmp->dst failed: " + ec.message());
}

inline void append_line(const fs::path &dst, const std::string &line)
{
    std::ofstream f(dst, std::ios::app);
    if (!f)
        throw std::runtime_error("open index for append failed: " + dst.string());
    f << line << '\n';
    if (!f)
        throw std::runtime_error("append index failed: " + dst.string());
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

static std::atomic<uint64_t> g_seq{1}; // per-process sequence for filenames

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
            http::async_read(socket, buffer, req, [self](auto ec, auto)
                             {
                if (!ec) self->handle(); });
        }

        // FIX: keep response alive through async_write
        void respond(http::response<http::string_body> &&res)
        {
            auto self = shared_from_this();
            auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
            sp->set(http::field::server, "marshal-beast");

            http::async_write(socket, *sp, [self, sp](boost::beast::error_code, std::size_t)
                              {
                boost::system::error_code ignored;
                self->socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored); });
        }

        // crude parser for ?ts=…&limit=…
        static inline void parse_ts_limit(const std::string &target, std::string &ts, size_t &limit)
        {
            ts.clear();
            limit = 0;
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
        }

        void handle()
        {
            using nlohmann::json;

            // GET /health
            if (req.method() == http::verb::get && req.target() == "/health")
            {
                auto up = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                res.body() = json{{"status", "ok"}, {"uptime_s", up}}.dump();
                res.prepare_payload();
                return respond(std::move(res));
            }

            // GET /v1/pose/current
            if (req.method() == http::verb::get && req.target() == "/v1/pose/current")
            {
                auto p = state.poses.get();
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                auto jpose = pose_to_json(p);
                jpose["ts"] = iso8601_now();
                json body{{"pose", jpose}, {"source", p.source}};
                res.body() = body.dump();
                res.prepare_payload();
                return respond(std::move(res));
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
                        http::response<http::string_body> res{http::status::bad_request, req.version()};
                        res.set(http::field::content_type, "application/json");
                        res.body() = json{{"error", "missing fields"}, {"required", {"p", "R"}}}.dump();
                        res.prepare_payload();
                        return respond(std::move(res));
                    }

                    auto jp = body["p"];
                    auto jR = body["R"];
                    if (!jp.is_array() || !jR.is_array() || jp.size() != 3 || jR.size() != 9)
                    {
                        http::response<http::string_body> res{http::status::bad_request, req.version()};
                        res.set(http::field::content_type, "application/json");
                        res.body() = json{{"error", "invalid shapes"}, {"p_len", jp.size()}, {"R_len", jR.size()}}.dump();
                        res.prepare_payload();
                        return respond(std::move(res));
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

                    auto jpose = pose_to_json(pose);
                    jpose["ts"] = iso8601_now();

                    http::response<http::string_body> res{http::status::ok, req.version()};
                    res.set(http::field::content_type, "application/json");
                    res.body() = json{{"status", "ok"}, {"pose", jpose}}.dump();
                    res.prepare_payload();
                    return respond(std::move(res));
                }
                catch (const std::exception &e)
                {
                    http::response<http::string_body> res{http::status::bad_request, req.version()};
                    res.set(http::field::content_type, "application/json");
                    res.body() = json{{"error", "bad json"}, {"what", e.what()}}.dump();
                    res.prepare_payload();
                    return respond(std::move(res));
                }
            }

            // GET /v1/config
            if (req.method() == http::verb::get && req.target() == "/v1/config")
            {
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                res.body() = json{
                    {"data_dir", state.data_dir},
                    {"ws_port", 8090},
                    {"max_entries", 100000}}
                                 .dump();
                res.prepare_payload();
                return respond(std::move(res));
            }

            // POST /v1/mrd/ingest  (writes ${data_dir}/mrd/*.mrd and updates index/latest)
            if (req.method() == http::verb::post && req.target() == "/v1/mrd/ingest")
            {
                try
                {
                    const std::string &body = req.body();
                    if (body.empty())
                    {
                        json j = {{"error", "empty body"}};
                        http::response<http::string_body> res{http::status::bad_request, req.version()};
                        res.set(http::field::content_type, "application/json");
                        res.body() = j.dump();
                        res.prepare_payload();
                        return respond(std::move(res));
                    }

                    fs::path mrd_root = fs::path(state.data_dir) / "mrd";
                    ensure_dir(mrd_root);

                    const std::string ts = iso8601_now_ms();
                    const uint64_t seq = g_seq.fetch_add(1);
                    std::ostringstream name;
                    name << ts << '_' << std::setw(6) << std::setfill('0') << seq << ".mrd";
                    fs::path out_path = mrd_root / name.str();

                    // Atomic write helper
                    write_atomic(out_path, body.data(), body.size());

                    std::error_code ec;
                    auto size_bytes = fs::file_size(out_path, ec);
                    if (ec)
                        size_bytes = body.size();

                    json entry = {{"path", out_path.string()}, {"ts", ts}, {"size_bytes", size_bytes}, {"type", "acq"}, {"seq", seq}};

                    // fs::path meta_root = fs::path(state.data_dir);
                    append_line(mrd_root / "index.jsonl", entry.dump());
                    const std::string latest_dump = entry.dump();
                    // Atomic write helper
                    write_atomic(mrd_root / "latest.json", latest_dump.data(), latest_dump.size());

                    http::response<http::string_body> res{http::status::created, req.version()};
                    res.set(http::field::content_type, "application/json");
                    res.body() = entry.dump();
                    res.prepare_payload();
                    return respond(std::move(res));
                }
                catch (const std::exception &e)
                {
                    json j = {{"error", "ingest failed"}, {"what", e.what()}};
                    http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                    res.set(http::field::content_type, "application/json");
                    res.body() = j.dump();
                    res.prepare_payload();
                    return respond(std::move(res));
                }
            }

            // GET /v1/mrd/latest  (reads ${data_dir}/mrd/latest.json)
            if (req.method() == http::verb::get && req.target() == "/v1/mrd/latest")
            {
                fs::path latest = fs::path(state.data_dir) / "mrd" / "latest.json";
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                if (fs::exists(latest))
                {
                    std::string s;
                    if (read_file_all(latest, s) && !s.empty())
                    {
                        res.body() = s;
                    }
                    else
                    {
                        res.result(http::status::no_content);
                        res.body() = "";
                    }
                }
                else
                {
                    res.result(http::status::no_content);
                    res.body() = "";
                }
                res.prepare_payload();
                return respond(std::move(res));
            }

            // GET /v1/mrd/since?ts=...&limit=...  (reads ${data_dir}/mrd/index.jsonl)
            if (req.method() == http::verb::get && std::string(req.target()).rfind("/v1/mrd/since", 0) == 0)
            {
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                try
                {
                    std::string ts;
                    size_t limit = 0;
                    parse_ts_limit(std::string(req.target()), ts, limit);
                    if (ts.empty())
                    {
                        nlohmann::json j = {{"error", "missing ts param"}};
                        res.result(http::status::bad_request);
                        res.body() = j.dump();
                        res.prepare_payload();
                        return respond(std::move(res));
                    }

                    // fs::path index = fs::path(state.data_dir) / "index.jsonl";
                    fs::path index = fs::path(state.data_dir) / "mrd" / "index.jsonl";
                    nlohmann::json out = nlohmann::json::array();

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
                    res.body() = out.dump();
                    res.prepare_payload();
                    return respond(std::move(res));
                }
                catch (const std::exception &e)
                {
                    nlohmann::json j = {{"error", "since failed"}, {"what", e.what()}};
                    res.result(http::status::internal_server_error);
                    res.body() = j.dump();
                    res.prepare_payload();
                    return respond(std::move(res));
                }
            }

            // 404 fallback
            {
                http::response<http::string_body> res{http::status::not_found, req.version()};
                res.set(http::field::content_type, "application/json");
                res.body() = R"({"error":"not found"})";
                res.prepare_payload();
                return respond(std::move(res));
            }
        }
    };
};
