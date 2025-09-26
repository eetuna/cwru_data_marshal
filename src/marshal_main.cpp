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
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <boost/asio.hpp>
#include <fcntl.h>
#include <unistd.h>
#include "marshal_http.hpp"
#include "marshal_ws.hpp"
#include "marshal_state.hpp"
// === Realtime additions (minimal) ===
#include "realtime.hpp"
#include <thread>

FrameQueue *g_queue = nullptr;
LastValueCache g_lvc;
static std::thread g_writer;
static std::atomic<bool> g_run{false};

static void writer_thread(const std::string &session_dir)
{
    try
    {
        SegmentWriter writer(session_dir);
        Frame f;
        while (g_run.load())
        {
            if (!g_queue->dequeue(f))
                break;
            auto meta = writer.append(f);
            g_lvc.publish(meta);
        }
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "writer_thread error: %s\n", e.what());
    }
}

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
    std::string segment_root = "";
    std::string sink_namedpipe = "";
    size_t rb_capacity = 256;
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
        else if (a == "--segment-root" && i + 1 < argc)
            segment_root = argv[++i];
        else if (a == "--sink-namedpipe" && i + 1 < argc)
            sink_namedpipe = argv[++i];
        else if (a == "--rb-capacity" && i + 1 < argc)
            rb_capacity = static_cast<size_t>(std::stoull(argv[++i]));
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
    // Make FIFO path available to HTTP ingest
    state.sink_namedpipe = sink_namedpipe;
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
                                       ? iso8601_now_ms()
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

    // Realtime: start queue + writer (under MRD sink root)
    static FrameQueue queue(rb_capacity);
    g_queue = &queue;
    g_run = true;
    g_writer = std::thread([&]
                           {
               std::string root = segment_root.empty()
                                ? (fs::path(state.data_dir) / "mrd").string()
                                : segment_root;
                try {
                   SegmentWriter writer(root);
                   int fifo_fd = -1;
                   if (!sink_namedpipe.empty()) {
                       fifo_fd = ::open(sink_namedpipe.c_str(), O_WRONLY | O_NONBLOCK);
                   }
                   Frame f;
                   while (g_run.load()) {
                      if (!g_queue->dequeue(f)) break;
                       auto meta = writer.append(f);
                       g_lvc.publish(meta);
                       if (fifo_fd != -1 && !f.payload.empty()) { (void)::write(fifo_fd, f.payload.data(), (ssize_t)f.payload.size()); }
                   }
               } catch (const std::exception &e) {
                   fprintf(stderr, "writer_thread error: %s\n", e.what());
               } });

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
