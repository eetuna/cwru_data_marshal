/*
 * File: src/marshal_main.cpp
 * Project: CWRU Data Marshal
 * Purpose: Main server binary: HTTP /v1/mrd/* endpoints, WS broker
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#include <iostream>
#include <boost/asio.hpp>
#include "marshal_http.hpp"
#include "marshal_ws.hpp"
#include "marshal_state.hpp"

int main(int argc, char **argv)
{
    std::string http_bind = "0.0.0.0:8080";
    std::string ws_bind = "0.0.0.0:8090";
    std::string data_dir = "/data";
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--http" && i + 1 < argc)
            http_bind = argv[++i];
        else if (a == "--ws" && i + 1 < argc)
            ws_bind = argv[++i];
        else if (a == "--data" && i + 1 < argc)
            data_dir = argv[++i];
    }
    auto split = [](const std::string &s)
    { auto p=s.find(":"); return std::pair{s.substr(0,p), static_cast<unsigned short>(std::stoi(s.substr(p+1)))}; };
    auto [http_host, http_port] = split(http_bind);
    auto [ws_host, ws_port] = split(ws_bind);

    boost::asio::io_context ioc{1};
    MarshalState state;
    state.io = &ioc;

    boost::asio::ip::tcp::endpoint http_ep{boost::asio::ip::make_address(http_host), http_port};
    boost::asio::ip::tcp::endpoint ws_ep{boost::asio::ip::make_address(ws_host), ws_port};

    HttpServer http{ioc, http_ep, state};
    WsServer ws{ioc, ws_ep, state};

    // Default root is /data (set in MarshalState)
    std::string data_root = state.data_dir;
    // Simple override: if --data is provided, grab its argument
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--data")
        {
            data_dir = argv[i + 1];
            break;
        }
    }

    // Ensure mrd subfolder under data_dir exists
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(data_root) / "mrd", ec);
    if (ec)
    {
        std::cerr << "WARN: failed to ensure " << data_root << "/mrd : " << ec.message() << "\n";
    }
    // Pass into state
    state.data_dir = data_root;
    std::cout << "marshal listening http=" << http_bind << " ws=" << ws_bind << " data=" << state.data_dir << "\n";

    ioc.run();
    return 0;
}