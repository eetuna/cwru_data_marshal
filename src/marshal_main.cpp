/*
 * File: src/marshal_main.cpp
 * Project: CWRU Data Marshal
 * Purpose: Main server binary: HTTP /v1/mrd/* endpoints, WS broker
 * Notes:
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-21
 */

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <system_error>
#include <boost/asio.hpp>
#include "marshal_http.hpp"
#include "marshal_ws.hpp"
#include "marshal_state.hpp"
#include "mrd_io.hpp"
#include "mrd_sink.hpp"

namespace fs = std::filesystem;

inline std::string sanitize_session(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out.push_back(c);
    }
    return out.empty() ? mrd::iso8601_now_ms() : out;
}

int main(int argc, char **argv)
{
    // Defaults aligned with devcontainer: everything under /src/data (via --data ./data)
    std::string http_bind = "0.0.0.0:8080";
    std::string ws_bind = "0.0.0.0:8090";
    std::string data_dir = "./data";
    std::string sink = "mrd"; // "mrd" or "dumpbox"
    std::string dumpbox_root = "./data/dumpbox";
    std::string dumpbox_session = "";
    std::size_t flush_max_frames = 4;
    int flush_max_ms = 50;
    std::size_t max_body_size = 128ULL * 1024ULL * 1024ULL;
    int shutdown_timeout_sec = 30;

    // Parse CLI
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--http" && i + 1 < argc)
            http_bind = argv[++i];
        else if (a == "--ws" && i + 1 < argc)
            ws_bind = argv[++i];
        else if (a == "--data" && i + 1 < argc)
            data_dir = argv[++i];
        else if (a == "--sink" && i + 1 < argc)
            sink = argv[++i];
        else if (a == "--dumpbox-root" && i + 1 < argc)
            dumpbox_root = argv[++i];
        else if (a == "--dumpbox-session" && i + 1 < argc)
            dumpbox_session = sanitize_session(argv[++i]);
        else if (a == "--max-body-size" && i + 1 < argc)
            max_body_size = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (a == "--flush-max-frames" && i + 1 < argc)
            flush_max_frames = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (a == "--flush-max-ms" && i + 1 < argc)
            flush_max_ms = std::stoi(argv[++i]);
        else if (a == "--shutdown-timeout-sec" && i + 1 < argc)
            shutdown_timeout_sec = std::stoi(argv[++i]);
    }

    auto split = [](const std::string &s)
    {
        auto p = s.find(':');
        return std::pair{s.substr(0, p), static_cast<unsigned short>(std::stoi(s.substr(p + 1)))};
    };
    auto [http_host, http_port] = split(http_bind);
    auto [ws_host, ws_port] = split(ws_bind);

    // IO + state
    boost::asio::io_context ioc{1};
    MarshalState state;
    state.io = &ioc;
    state.max_body_bytes = max_body_size;

    // Apply CLI to state FIRST
    state.sink_mode = (sink == "dumpbox") ? SinkMode::DUMPBOX : SinkMode::MRD;
    state.data_dir = data_dir;
    state.dumpbox_root = dumpbox_root;
    state.dumpbox_session = dumpbox_session;
    state.flush_policy.max_pending_frames = std::max<std::size_t>(1, flush_max_frames);
    state.flush_policy.max_pending_interval = std::chrono::milliseconds(std::max(flush_max_ms, 0));

    // Ensure directories
    std::error_code ec;
    if (state.sink_mode == SinkMode::MRD)
    {
        fs::create_directories(fs::path(state.data_dir) / "mrd", ec);
        if (ec)
        {
            std::cerr << "WARN: ensure " << (fs::path(state.data_dir) / "mrd")
                      << " failed: " << ec.message() << "\n";
        }
    }
    else
    {
        std::string session_name = state.dumpbox_session.empty()
                                       ? mrd::iso8601_now_ms()
                                       : state.dumpbox_session;
        state.dumpbox_session = session_name;
        fs::create_directories(fs::path(state.dumpbox_root) / session_name / "files", ec);
        if (ec)
        {
            std::cerr << "WARN: ensure "
                      << (fs::path(state.dumpbox_root) / session_name / "files")
                      << " failed: " << ec.message() << "\n";
        }
    }

    state.mrd_sink = std::make_shared<mrd::MrdSink>(state);

    // Networking endpoints
    boost::asio::ip::tcp::endpoint http_ep{boost::asio::ip::make_address(http_host), http_port};
    boost::asio::ip::tcp::endpoint ws_ep{boost::asio::ip::make_address(ws_host), ws_port};

    // Servers (share state by reference)
    HttpServer http{ioc, http_ep, state};
    WsServer ws{ioc, ws_ep, state};

    // Graceful shutdown handler (async)
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    auto shutdown_timer = std::make_shared<boost::asio::steady_timer>(ioc);

    signals.async_wait([&, shutdown_timer](const boost::system::error_code &ec, int signum) {
        if (!ec)
        {
            std::cerr << "\n[SHUTDOWN] Received signal " << signum
                      << ", shutting down (timeout: " << shutdown_timeout_sec << "s)...\n";

            // Start timeout timer
            shutdown_timer->expires_after(std::chrono::seconds(shutdown_timeout_sec));
            shutdown_timer->async_wait([&](const boost::system::error_code &) {
                std::cerr << "[SHUTDOWN] Timeout reached, forcing exit.\n";
                std::exit(1);
            });

            // Flush all HDF5 data
            if (state.mrd_sink)
            {
                std::cerr << "[SHUTDOWN] Flushing all HDF5 streams...\n";
                state.mrd_sink->flush_all();
                std::cerr << "[SHUTDOWN] Flush complete.\n";
            }

            ioc.stop();
        }
    });

    // Log effective config
    std::cout << "marshal listening http=" << http_bind
              << " ws=" << ws_bind
              << " data=" << state.data_dir
              << " max_body=" << state.max_body_bytes;
    if (state.sink_mode == SinkMode::DUMPBOX)
    {
        std::cout << " dumpbox_root=" << state.dumpbox_root
                  << " session=" << state.dumpbox_session;
    }
    else
    {
        std::cout << " sink=mrd"
                  << " flush_frames=" << state.flush_policy.max_pending_frames
                  << " flush_ms=" << state.flush_policy.max_pending_interval.count();
    }
    std::cout << " shutdown_timeout=" << shutdown_timeout_sec << "s\n";

    ioc.run();

    std::cerr << "[SHUTDOWN] Server stopped.\n";
    return 0;
}
