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

#include <iostream>
#include <filesystem>
#include <system_error>
#include <boost/asio.hpp>
#include "marshal_http.hpp"
#include "marshal_ws.hpp"
#include "marshal_state.hpp"
#include "mrd_io.hpp"

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    // Defaults aligned with devcontainer: everything under /src/data (via --data ./data)
    std::string http_bind = "0.0.0.0:8080";
    std::string ws_bind = "0.0.0.0:8090";
    std::string data_dir = "./data";
    std::string sink = "mrd"; // "mrd" or "dumpbox"
    std::string dumpbox_root = "./data/dumpbox";
    std::string dumpbox_session = "";
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
            dumpbox_session = argv[++i];
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

    // Apply CLI to state FIRST
    state.sink_mode = (sink == "dumpbox") ? SinkMode::DUMPBOX : SinkMode::MRD;
    state.data_dir = data_dir;
    state.dumpbox_root = dumpbox_root;
    state.dumpbox_session = dumpbox_session;

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

    // Networking endpoints
    boost::asio::ip::tcp::endpoint http_ep{boost::asio::ip::make_address(http_host), http_port};
    boost::asio::ip::tcp::endpoint ws_ep{boost::asio::ip::make_address(ws_host), ws_port};

    // Servers (share state by reference)
    HttpServer http{ioc, http_ep, state};
    WsServer ws{ioc, ws_ep, state};

    // Log effective config
    std::cout << "marshal listening http=" << http_bind
              << " ws=" << ws_bind
              << " data=" << state.data_dir;
    if (state.sink_mode == SinkMode::DUMPBOX)
    {
        std::cout << " dumpbox_root=" << state.dumpbox_root
                  << " session=" << state.dumpbox_session;
    }
    else
    {
        std::cout << " sink=mrd";
    }
    std::cout << "\n";

    ioc.run();
    return 0;
}
