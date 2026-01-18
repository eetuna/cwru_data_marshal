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
#include <thread>
#include <atomic>
#include <limits>
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

namespace
{
bool parse_size_arg(const char *value, std::size_t &out)
{
    try
    {
        unsigned long long parsed = std::stoull(value);
        if (parsed > std::numeric_limits<std::size_t>::max())
            return false;
        out = static_cast<std::size_t>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parse_int_arg(const char *value, int &out)
{
    try
    {
        out = std::stoi(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parse_host_port(const std::string &input, std::string &host, unsigned short &port)
{
    auto p = input.find(':');
    if (p == std::string::npos || p == 0 || p + 1 >= input.size())
        return false;
    host = input.substr(0, p);
    try
    {
        int parsed = std::stoi(input.substr(p + 1));
        if (parsed <= 0 || parsed > std::numeric_limits<unsigned short>::max())
            return false;
        port = static_cast<unsigned short>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace

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
        {
            if (!parse_size_arg(argv[++i], max_body_size))
            {
                std::cerr << "Invalid --max-body-size value\n";
                return 1;
            }
        }
        else if (a == "--flush-max-frames" && i + 1 < argc)
        {
            if (!parse_size_arg(argv[++i], flush_max_frames))
            {
                std::cerr << "Invalid --flush-max-frames value\n";
                return 1;
            }
        }
        else if (a == "--flush-max-ms" && i + 1 < argc)
        {
            if (!parse_int_arg(argv[++i], flush_max_ms))
            {
                std::cerr << "Invalid --flush-max-ms value\n";
                return 1;
            }
        }
        else if (a == "--shutdown-timeout-sec" && i + 1 < argc)
        {
            if (!parse_int_arg(argv[++i], shutdown_timeout_sec))
            {
                std::cerr << "Invalid --shutdown-timeout-sec value\n";
                return 1;
            }
        }
    }

    std::string http_host;
    std::string ws_host;
    unsigned short http_port = 0;
    unsigned short ws_port = 0;
    if (!parse_host_port(http_bind, http_host, http_port))
    {
        std::cerr << "Invalid --http bind (expected host:port)\n";
        return 1;
    }
    if (!parse_host_port(ws_bind, ws_host, ws_port))
    {
        std::cerr << "Invalid --ws bind (expected host:port)\n";
        return 1;
    }

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

    // Periodic cleanup for idle streams
    auto cleanup_timer = std::make_shared<boost::asio::steady_timer>(ioc);
    std::function<void()> schedule_cleanup;
    schedule_cleanup = [&, cleanup_timer]() {
        cleanup_timer->expires_after(std::chrono::seconds(60));
        cleanup_timer->async_wait([&, cleanup_timer](const boost::system::error_code &ec) {
            if (ec)
                return;
            if (state.mrd_sink)
                state.mrd_sink->cleanup_idle_streams();
            schedule_cleanup();
        });
    };
    schedule_cleanup();

    // Graceful shutdown handler (async)
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    auto shutdown_timer = std::make_shared<boost::asio::steady_timer>(ioc);
    std::atomic<bool> shutdown_started{false};
    std::thread flush_thread;

    signals.async_wait([&, shutdown_timer](const boost::system::error_code &ec, int signum) {
        if (!ec)
        {
            if (shutdown_started.exchange(true))
                return;
            std::cerr << "\n[SHUTDOWN] Received signal " << signum
                      << ", shutting down (timeout: " << shutdown_timeout_sec << "s)...\n";

            // Start timeout timer
            shutdown_timer->expires_after(std::chrono::seconds(shutdown_timeout_sec));
            shutdown_timer->async_wait([&](const boost::system::error_code &) {
                std::cerr << "[SHUTDOWN] Timeout reached, forcing exit.\n";
                std::exit(1);
            });

            // Flush on background thread so timer can still fire.
            flush_thread = std::thread([&]() {
                if (state.mrd_sink)
                {
                    std::cerr << "[SHUTDOWN] Flushing all HDF5 streams...\n";
                    state.mrd_sink->flush_all();
                    std::cerr << "[SHUTDOWN] Flush complete.\n";
                }
                boost::asio::post(ioc, [shutdown_timer, &ioc]() {
                    shutdown_timer->cancel();
                    ioc.stop();
                });
            });
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

    if (flush_thread.joinable())
        flush_thread.join();

    std::cerr << "[SHUTDOWN] Server stopped.\n";
    return 0;
}
